#include "digest.h"
#include "extension.h"
#include "plugin_module.h"
#include "run_transport.h"

#include <huxerui/pluginkit/plugin.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>

namespace huxerui::pluginkit {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace detail;

constexpr std::size_t MaxManifest = 64 * 1024;
constexpr std::size_t MaxModule = 64 * 1024 * 1024;
constexpr std::size_t MaxProfile = 1024 * 1024;
constexpr int MaxManifestDepth = 16;

fs::path NativePath(const File& file) {
  const auto path = file.Path();
  return fs::path(std::u8string(path.begin(), path.end()));
}

Json ParseJson(std::span<const std::byte> bytes, int max_depth, ErrorCode error) {
  return Json::parse(AsText(bytes), [max_depth, error](int depth, Json::parse_event_t, Json&) {
    if (depth > max_depth) throw Error(error, "JSON exceeds the nesting limit");
    return true;
  });
}

bool Matches(const PackageInfo& info, ContractInfo contract) {
  return ContractInfo{info.contract_id, info.contract_version, info.contract_fingerprint} == contract;
}

std::string_view ModeName(ExecutionMode mode) {
  switch (mode) {
    case ExecutionMode::InProcess: return "in_process";
    case ExecutionMode::WorkerProcess: return "worker_process";
  }
  throw Error(ErrorCode::InvalidArgument, "Unknown plugin execution mode");
}

ExecutionMode ParseMode(std::string_view mode, ErrorCode error) {
  if (mode == "in_process") return ExecutionMode::InProcess;
  if (mode == "worker_process") return ExecutionMode::WorkerProcess;
  throw Error(error, "Unknown plugin execution mode");
}

PackageInfo ParseManifest(const Json& manifest, std::string digest) {
  try {
    if (!manifest.at("schema_version").is_number_integer() || manifest.at("schema_version") != 1 ||
        manifest.at("target") != PackageTarget) {
      throw Error(ErrorCode::InvalidPackage, "The package schema or target is unsupported");
    }
    const auto& contract = manifest.at("contract");
    const auto& extension = manifest.at("extension");
    const auto& version = contract.at("version");
    if (!version.is_number_unsigned() || version == 0 || version > UINT32_MAX) {
      throw Error(ErrorCode::InvalidPackage, "The contract version must be a positive 32-bit integer");
    }
    PackageInfo info{
        manifest.at("id").get<std::string>(), manifest.at("version").get<std::string>(), std::move(digest),
        contract.at("id").get<std::string>(), version.get<std::uint32_t>(),
        contract.at("fingerprint").get<std::string>(), extension.at("point").get<std::string>(),
        extension.at("id").get<std::string>(), extension.at("extensions").get<std::vector<std::string>>(), {},
        manifest.at("cpp_abi").get<std::string>()};
    if (!IsId(info.id) || !IsId(info.version) || !IsId(info.contract_id) || !IsId(info.point) ||
        !IsId(info.contribution) || !IsDigest(info.contract_fingerprint) || info.extensions.size() > 32) {
      throw Error(ErrorCode::InvalidPackage, "Invalid plugin identity, contract, or extension metadata");
    }
    const auto modes = manifest.at("execution_modes").get<std::vector<std::string>>();
    if (modes.empty() || modes.size() > 2) throw Error(ErrorCode::InvalidPackage, "Invalid execution mode list");
    for (const auto& mode : modes) {
      const auto value = ParseMode(mode, ErrorCode::InvalidPackage);
      if (std::ranges::find(info.execution_modes, value) != info.execution_modes.end()) {
        throw Error(ErrorCode::InvalidPackage, "Duplicate execution mode");
      }
      info.execution_modes.push_back(value);
    }
    if (std::ranges::find(info.execution_modes, ExecutionMode::InProcess) != info.execution_modes.end() &&
        (info.cpp_abi.empty() || info.cpp_abi.size() > 256 || info.cpp_abi.find('\0') != std::string::npos)) {
      throw Error(ErrorCode::InvalidPackage, "Missing or invalid in-process ABI identity");
    }
    for (const auto& suffix : info.extensions) {
      if (!suffix.starts_with('.') || !IsId(std::string_view(suffix).substr(1))) {
        throw Error(ErrorCode::InvalidPackage, "Invalid file suffix in the manifest");
      }
    }
    return info;
  } catch (const Json::exception& error) {
    throw Error(ErrorCode::InvalidPackage, "Invalid plugin manifest: " + std::string(error.what()));
  }
}

struct Package {
  Bytes manifest_bytes;
  Bytes module_bytes;
  Handle module_file;
  Json manifest;
  PackageInfo info;
};

Package ReadPackage(const fs::path& directory) {
  auto manifest_bytes = ReadFileBytes(directory / "plugin.json", MaxManifest);
  auto module_file = OpenRegularFile(directory / ModuleName);
  auto module_bytes = ReadFileBytes(module_file.get(), MaxModule);
  if (module_bytes.empty()) throw Error(ErrorCode::InvalidPackage, std::string(ModuleName) + " is empty");
  // Approval covers the exact manifest and library bytes, including manifest formatting.
  const auto digest = Sha256(AsBytes(Sha256(manifest_bytes) + Sha256(module_bytes)));
  try {
    auto manifest = ParseJson(manifest_bytes, MaxManifestDepth, ErrorCode::InvalidPackage);
    auto info = ParseManifest(manifest, digest);
    return {std::move(manifest_bytes), std::move(module_bytes), std::move(module_file), std::move(manifest), std::move(info)};
  } catch (const Json::exception& error) {
    throw Error(ErrorCode::InvalidPackage, "Cannot parse plugin.json: " + std::string(error.what()));
  }
}

struct Installed {
  PackageInfo info;
  Json manifest;
  bool enabled;
  InstallOptions approval;
};

using Catalog = std::map<std::string, Installed, std::less<>>;

Catalog ReadCatalog(const fs::path& file) {
  std::error_code error;
  const auto status = fs::symlink_status(file, error);
  if (status.type() == fs::file_type::not_found) return {};
  if (error) throw Error(ErrorCode::Io, "Cannot inspect the plugin profile: " + error.message());
  try {
    const auto bytes = ReadFileBytes(file, MaxProfile);
    const auto profile = ParseJson(bytes, MaxManifestDepth + 4, ErrorCode::Io);
    const auto& plugins = profile.at("plugins");
    if (profile.at("schema_version") != 1 || !plugins.is_array()) {
      throw Error(ErrorCode::Io, "Invalid plugin profile format");
    }
    Catalog catalog;
    for (const auto& plugin : plugins) {
      const auto digest = plugin.at("digest").get<std::string>();
      if (!IsDigest(digest)) throw Error(ErrorCode::Io, "Invalid package digest in the profile");
      auto manifest = plugin.at("manifest");
      auto info = ParseManifest(manifest, digest);
      const auto id = info.id;
      InstallOptions approval{ParseMode(plugin.at("mode").get<std::string>(), ErrorCode::Io),
                              plugin.at("services").get<std::vector<std::string>>()};
      if (std::ranges::find(info.execution_modes, approval.mode) == info.execution_modes.end() ||
          approval.services.size() > 64 || !std::ranges::is_sorted(approval.services) ||
          std::ranges::adjacent_find(approval.services) != approval.services.end() ||
          !std::ranges::all_of(approval.services, [](const auto& service) { return IsId(service); })) {
        throw Error(ErrorCode::Io, "Invalid plugin approval in the profile");
      }
      if (!catalog.emplace(id, Installed{std::move(info), std::move(manifest), plugin.at("enabled").get<bool>(),
                                         std::move(approval)}).second) {
        throw Error(ErrorCode::Io, "Duplicate plugin ID in the profile");
      }
    }
    return catalog;
  } catch (const Json::exception& error) {
    throw Error(ErrorCode::Io, "Cannot parse the plugin profile: " + std::string(error.what()));
  }
}

void SaveCatalog(const fs::path& file, const Catalog& catalog) {
  auto plugins = Json::array();
  for (const auto& [id, installed] : catalog) {
    plugins.push_back({{"digest", installed.info.digest}, {"manifest", installed.manifest}, {"enabled", installed.enabled},
                       {"mode", ModeName(installed.approval.mode)}, {"services", installed.approval.services}});
  }
  const auto text = Json{{"schema_version", 1}, {"plugins", std::move(plugins)}}.dump(2);
  if (text.size() > MaxProfile) throw Error(ErrorCode::PayloadTooLarge, "The plugin profile exceeds the byte limit");
  WriteAtomicFile(file, AsBytes(text));
}

struct RegisteredContract {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;
  bool remote;

  ContractInfo Info() const { return {id, version, fingerprint}; }
};

} // namespace

struct PluginManager::Impl {
  Impl(const File& directory, const std::optional<File>& executable, TaskScope callbacks)
      : profile(fs::absolute(NativePath(directory))),
        worker_executable(executable ? std::optional(fs::absolute(NativePath(*executable))) : std::nullopt),
        callbacks(std::move(callbacks)) {
    fs::create_directories(profile);
    ownership = LockProfile(profile);
    catalog = ReadCatalog(profile / "profile.json");
  }

  ~Impl() {
    for (const auto& [id, worker] : workers) worker->Shutdown();
  }

  void ShutdownWorkers() {
    std::call_once(shutdown, [&] {
      decltype(workers) running;
      {
        std::lock_guard lock(mutex);
        running.swap(workers);
      }
      for (const auto& [id, worker] : running) worker->BeginShutdown();
      for (const auto& [id, worker] : running) worker->Shutdown();
    });
  }

  std::vector<RegisteredService> ApprovedServices(const Installed& installed) const {
    std::vector<RegisteredService> approved;
    for (const auto& id : installed.approval.services) {
      if (const auto service = services.find(id); service != services.end()) approved.push_back(service->second);
    }
    return approved;
  }

  Package VerifyPackage(const Installed& installed) const;
  PluginStartup Startup(const PackageInfo& info) const;
  std::shared_ptr<Worker> StartWorker(const std::string& id, const std::stop_token& admission,
                                     Deadline deadline, std::stop_token stop);
  Task<void> ChangeLocalEnabled(const std::string& id, bool enabled);
  void ChangeWorkerEnabled(const std::string& id, bool enabled, std::stop_token stop);

  void Commit(Catalog candidate) {
    // Publish the candidate in memory after the profile file is committed.
    SaveCatalog(profile / "profile.json", candidate);
    catalog.swap(candidate);
  }

  const Installed& RequireInstalled(std::string_view id) const {
    if (closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
    const auto found = catalog.find(id);
    if (found == catalog.end()) throw Error(ErrorCode::NotFound, "The plugin is not installed");
    return found->second;
  }

  void CommitEnabled(std::string_view id, bool enabled) {
    auto candidate = catalog;
    candidate.find(id)->second.enabled = enabled;
    Commit(std::move(candidate));
  }

  fs::path PackageDirectory(std::string_view digest) const { return profile / "packages" / std::string(digest); }

  const RegisteredContract& RequirePoint(std::string_view point, ContractInfo expected) const {
    const auto found = points.find(point);
    if (found == points.end()) throw Error(ErrorCode::NotFound, "The extension point is not registered");
    if (found->second.Info() != expected) {
      throw Error(ErrorCode::ContractMismatch, "The registered extension point uses a different contract");
    }
    return found->second;
  }

  fs::path profile;
  const std::optional<fs::path> worker_executable;
  TaskScope callbacks;
  Handle ownership;
  mutable std::mutex mutex;
  Catalog catalog;
  std::map<std::string, RegisteredContract, std::less<>> points;
  std::map<std::string, RegisteredService, std::less<>> services;
  std::map<std::string, std::shared_ptr<Worker>, std::less<>> workers;
  std::map<std::string, std::shared_ptr<LocalPlugin>, std::less<>> local_plugins;
  std::map<std::string, std::stop_source, std::less<>> admissions;
  bool closed = false;
  std::once_flag shutdown;
};

std::string_view ErrorName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::Io: return "io";
    case ErrorCode::ProfileBusy: return "profile_busy";
    case ErrorCode::InvalidPackage: return "invalid_package";
    case ErrorCode::ApprovalMismatch: return "approval_mismatch";
    case ErrorCode::PackageChanged: return "package_changed";
    case ErrorCode::AlreadyInstalled: return "already_installed";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::Disabled: return "disabled";
    case ErrorCode::ContractMismatch: return "contract_mismatch";
    case ErrorCode::Closed: return "closed";
    case ErrorCode::DeadlineExceeded: return "deadline_exceeded";
    case ErrorCode::WorkerExited: return "worker_exited";
    case ErrorCode::Protocol: return "protocol";
    case ErrorCode::PluginFailed: return "plugin_failed";
    case ErrorCode::PayloadTooLarge: return "payload_too_large";
    case ErrorCode::ServiceDenied: return "service_denied";
    case ErrorCode::ServiceUnavailable: return "service_unavailable";
    case ErrorCode::ReentrantCall: return "reentrant_call";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ReceiverClosed: return "receiver_closed";
    case ErrorCode::UnsupportedMode: return "unsupported_mode";
    case ErrorCode::CallbackUnavailable: return "callback_unavailable";
    case ErrorCode::Busy: return "busy";
  }
  return "unknown";
}

PluginManager::PluginManager(const File& profile, const std::optional<File>& executable, TaskScope callbacks)
    : impl_(std::make_shared<Impl>(profile, executable, std::move(callbacks))) {}

PluginManager::~PluginManager() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->closed = true;
    for (auto& [id, admission] : impl_->admissions) admission.request_stop();
  }
  for (const auto& [id, local] : impl_->local_plugins) BeginLocalShutdown(local);
  impl_->ShutdownWorkers();
}

Task<void> PluginManager::Shutdown() {
  return ShutdownManager(impl_);
}

Task<void> PluginManager::ShutdownManager(std::shared_ptr<Impl> impl) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  {
    std::lock_guard lock(impl->mutex);
    impl->closed = true;
    for (auto& [id, admission] : impl->admissions) admission.request_stop();
  }
  for (const auto& [id, local] : impl->local_plugins) BeginLocalShutdown(local);
  co_await RunTransport([impl](std::stop_token) { impl->ShutdownWorkers(); });
  std::vector<std::shared_ptr<LocalPlugin>> locals;
  for (const auto& [id, local] : impl->local_plugins) locals.push_back(local);
  for (const auto& local : locals) co_await FinishLocalShutdown(local);
  impl->local_plugins.clear();
}

void PluginManager::Register(std::string_view point, ContractInfo contract, bool remote) {
  RejectServiceReentry();
  if (!IsId(point) || !IsId(contract.id) || !contract.version || !IsDigest(contract.fingerprint)) {
    throw Error(ErrorCode::InvalidArgument, "Invalid extension point or contract identity");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  if (!impl_->points.try_emplace(std::string(point), std::string(contract.id), contract.version,
                                 std::string(contract.fingerprint), remote).second) {
    throw Error(ErrorCode::InvalidArgument, "The extension point is already registered");
  }
}

PackageInfo PluginManager::InspectPackage(const File& directory) {
  RejectServiceReentry();
  return ReadPackage(NativePath(directory)).info;
}

void PluginManager::Provide(ContractInfo contract, std::shared_ptr<void> implementation, ServiceDispatch dispatch) {
  RejectServiceReentry();
  if (!implementation || !IsId(contract.id) || !contract.version || !IsDigest(contract.fingerprint)) {
    throw Error(ErrorCode::InvalidArgument, "Invalid host service implementation or contract");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  if (!impl_->services.try_emplace(std::string(contract.id), std::string(contract.id), contract.version,
          std::string(contract.fingerprint), std::move(implementation), std::move(dispatch)).second) {
    throw Error(ErrorCode::InvalidArgument, "The host service is already registered");
  }
}

void PluginManager::InstallPackage(const File& directory, std::string_view approved_digest, InstallOptions options) {
  RejectServiceReentry();
  auto package = ReadPackage(NativePath(directory));
  if (package.info.digest != approved_digest) {
    throw Error(ErrorCode::ApprovalMismatch, "The package contents differ from the approved digest");
  }
  package.module_file.reset();
  std::lock_guard lock(impl_->mutex);
  const auto point = impl_->points.find(package.info.point);
  if (point == impl_->points.end() || !Matches(package.info, point->second.Info())) {
    throw Error(ErrorCode::ContractMismatch, "The package does not match a registered extension point");
  }
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  if (std::ranges::find(package.info.execution_modes, options.mode) == package.info.execution_modes.end() ||
      (options.mode == ExecutionMode::WorkerProcess && (!point->second.remote || !impl_->worker_executable))) {
    throw Error(ErrorCode::UnsupportedMode, "The package or extension point cannot use the selected execution mode");
  }
  if (options.mode == ExecutionMode::InProcess && package.info.cpp_abi != CppAbi) {
    throw Error(ErrorCode::ContractMismatch, "The package requires a different in-process SDK or C++ ABI");
  }
  std::ranges::sort(options.services);
  if (options.services.size() > 64 || std::ranges::adjacent_find(options.services) != options.services.end() ||
      !std::ranges::all_of(options.services, [](const auto& service) { return IsId(service); })) {
    throw Error(ErrorCode::InvalidArgument, "Invalid approved service list");
  }
  for (const auto& service : options.services) {
    const auto found = impl_->services.find(service);
    if (found == impl_->services.end() || (options.mode == ExecutionMode::WorkerProcess && !found->second.dispatch)) {
      throw Error(ErrorCode::InvalidArgument, "The approved service is unavailable in the selected mode: " + service);
    }
  }

  const auto current = impl_->catalog.find(package.info.id);
  if (current != impl_->catalog.end() && current->second.info.digest != package.info.digest) {
    throw Error(ErrorCode::AlreadyInstalled, "A different package is installed for this plugin ID");
  }
  if (current != impl_->catalog.end() &&
      (current->second.approval.mode != options.mode || current->second.approval.services != options.services)) {
    throw Error(ErrorCode::AlreadyInstalled, "The installed package already has a different approval");
  }
  const auto destination = impl_->PackageDirectory(package.info.digest);
  if (current != impl_->catalog.end()) {
    try {
      if (ReadPackage(destination).info.digest == approved_digest) return;
    } catch (const Error&) {
      // Reinstall the approved bytes if the stored copy is unreadable.
    }
    if (impl_->workers.contains(package.info.id) || impl_->local_plugins.contains(package.info.id)) {
      throw Error(ErrorCode::Busy, "Disable the plugin before repairing its package");
    }
  }

  fs::create_directories(destination);
  // Put both package files in place before publishing a new catalog entry.
  WriteAtomicFile(destination / "plugin.json", package.manifest_bytes);
  WriteAtomicFile(destination / ModuleName, package.module_bytes);
  if (current == impl_->catalog.end()) {
    auto candidate = impl_->catalog;
    const auto id = package.info.id;
    candidate.emplace(id, Installed{std::move(package.info), std::move(package.manifest), true, std::move(options)});
    impl_->Commit(std::move(candidate));
  }
}

Task<void> PluginManager::SetEnabled(std::string_view id, bool enabled) {
  return ChangeEnabled(impl_, std::string(id), enabled);
}

Task<void> PluginManager::ChangeEnabled(std::shared_ptr<Impl> impl, std::string id, bool enabled) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  // This Task owns a committed disable even if its caller cancels the wait.
  auto start = [impl, id = std::move(id), enabled](auto complete) mutable {
    (void)impl->callbacks.Launch([impl, id = std::move(id), enabled, complete]() -> Task<void> {
      try {
        ExecutionMode mode;
        {
          std::lock_guard lock(impl->mutex);
          mode = impl->RequireInstalled(id).approval.mode;
        }
        if (mode == ExecutionMode::InProcess) {
          co_await impl->ChangeLocalEnabled(id, enabled);
        } else {
          co_await RunTransport([impl, id, enabled](std::stop_token stop) {
            impl->ChangeWorkerEnabled(id, enabled, stop);
          });
        }
        complete.Complete();
      } catch (...) {
        complete.Fail(std::current_exception());
      }
    });
    return std::function<void()>{};
  };
  co_await AwaitCallback<void>(std::move(start));
}

Task<void> PluginManager::Impl::ChangeLocalEnabled(const std::string& id, bool enabled) {
  for (;;) {
    const auto running = local_plugins.find(id);
    const auto local = running == local_plugins.end() ? nullptr : running->second;
    {
      std::lock_guard lock(mutex);
      const auto& installed = RequireInstalled(id);
      if (!enabled || installed.enabled || !local) {
        if (installed.enabled != enabled) CommitEnabled(id, enabled);
        if (enabled || !local) co_return;
      }
    }
    // Commit the disabled state and close admission on the application thread before yielding to another caller.
    if (!enabled) BeginLocalShutdown(local);
    co_await FinishLocalShutdown(local);
    if (const auto current = local_plugins.find(id); current != local_plugins.end() && current->second == local) {
      local_plugins.erase(current);
    }
    if (!enabled) co_return;
    // Recheck the catalog after waiting in case another caller has already changed the activation.
  }
}

void PluginManager::Impl::ChangeWorkerEnabled(const std::string& id, bool enabled, std::stop_token stop) {
  std::shared_ptr<Worker> worker;
  {
    std::lock_guard lock(mutex);
    if (stop.stop_requested()) return;
    if (RequireInstalled(id).enabled != enabled) CommitEnabled(id, enabled);
    if (enabled) return;
    if (const auto admission = admissions.find(id); admission != admissions.end()) {
      admission->second.request_stop();
      admissions.erase(admission);
    }
    if (const auto running = workers.find(id); running != workers.end()) {
      worker = running->second;
      worker->BeginShutdown();
    }
  }
  if (!worker) return;
  worker->Shutdown();
  std::lock_guard lock(mutex);
  const auto running = workers.find(id);
  if (running != workers.end() && running->second == worker) workers.erase(running);
}

std::vector<PluginInfo> PluginManager::ListPlugins() const {
  // Read the snapshot without entering the worker's request queue, including during a host callback.
  std::lock_guard lock(impl_->mutex);
  std::vector<PluginInfo> result;
  for (const auto& [id, plugin] : impl_->catalog) {
    const auto worker = impl_->workers.find(id);
    result.push_back({plugin.info, plugin.enabled, plugin.approval.mode,
                      worker == impl_->workers.end() ? std::nullopt : worker->second->Pid()});
  }
  return result;
}

std::vector<ExtensionInfo> PluginManager::List(std::string_view point, ContractInfo contract) const {
  std::lock_guard lock(impl_->mutex);
  impl_->RequirePoint(point, contract);
  std::vector<ExtensionInfo> result;
  for (const auto& [id, plugin] : impl_->catalog) {
    if (plugin.enabled && plugin.info.point == point && Matches(plugin.info, contract)) {
      result.push_back({{id, plugin.info.contribution}, plugin.info.extensions});
    }
  }
  return result;
}

std::shared_ptr<ExtensionTarget> PluginManager::Create(std::string_view point, ContractInfo contract,
                                                         const ExtensionId& id, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24)) {
    throw Error(ErrorCode::InvalidArgument, "Extension timeout must be between 1 ms and 24 hours");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  const auto& registered = impl_->RequirePoint(point, contract);
  const auto plugin = impl_->catalog.find(id.plugin);
  if (plugin == impl_->catalog.end() || plugin->second.info.point != point ||
      plugin->second.info.contribution != id.contribution) {
    throw Error(ErrorCode::NotFound, "The extension is not installed at this point");
  }
  const auto& installed = plugin->second;
  if (!installed.enabled) throw Error(ErrorCode::Disabled, "The plugin is disabled");
  if (!Matches(installed.info, contract)) throw Error(ErrorCode::ContractMismatch, "The installed contract differs");

  if (installed.approval.mode == ExecutionMode::WorkerProcess) {
    if (!registered.remote || !impl_->worker_executable) {
      throw Error(ErrorCode::UnsupportedMode, "The extension cannot use a worker");
    }
    const auto admission = impl_->admissions.try_emplace(id.plugin).first->second.get_token();
    return MakeWorkerExtension([impl = impl_, id = id.plugin, admission](Deadline deadline, std::stop_token stop) {
      return impl->StartWorker(id, admission, deadline, stop);
    }, timeout, admission);
  }

  const auto running = impl_->local_plugins.find(id.plugin);
  if (running != impl_->local_plugins.end()) return MakeLocalExtension(running->second, timeout);
  auto startup = impl_->Startup(installed.info);
  auto local = CreateLocalPlugin(impl_->callbacks, [manager = std::weak_ptr<Impl>(impl_), installed]() -> const hpk_plugin_v1& {
    const auto impl = manager.lock();
    if (!impl) throw Error(ErrorCode::Closed, "The plugin manager is closed");
    if (installed.info.cpp_abi != CppAbi) {
      throw Error(ErrorCode::ContractMismatch, "The package requires a different in-process SDK or C++ ABI");
    }
    auto package = impl->VerifyPackage(installed);
    const auto module = impl->PackageDirectory(package.info.digest) / ModuleName;
    const ContractInfo contract{installed.info.contract_id, installed.info.contract_version,
                                installed.info.contract_fingerprint};
    const auto* binding = &LoadInProcessModule(module, std::move(package.module_file), contract);
    const auto* runtime = RuntimeIdentity();
    if (!runtime || !binding->cpp_abi || installed.info.cpp_abi != binding->cpp_abi || !binding->runtime_identity ||
        binding->runtime_identity() != runtime) {
      throw Error(ErrorCode::ContractMismatch, "The plugin does not share the host's C++ ABI and HuxerUI Runtime");
    }
    return *binding;
  }, std::move(startup), impl_->ApprovedServices(installed));
  impl_->local_plugins.emplace(id.plugin, local);
  return MakeLocalExtension(std::move(local), timeout);
}

Package PluginManager::Impl::VerifyPackage(const Installed& installed) const {
  try {
    auto package = ReadPackage(PackageDirectory(installed.info.digest));
    if (package.info.digest == installed.info.digest) return package;
  } catch (const Error& error) {
    throw Error(ErrorCode::PackageChanged, "Cannot verify the installed package: " + std::string(error.what()));
  }
  throw Error(ErrorCode::PackageChanged, "The installed package differs from the approved package");
}

PluginStartup PluginManager::Impl::Startup(const PackageInfo& info) const {
  const auto directory = PackageDirectory(info.digest);
  const auto data = profile / "data" / ("plugin-" + info.id + ".data");
  return {info.id, info.version, info.digest, PathText(directory), PathText(data)};
}

std::shared_ptr<Worker> PluginManager::Impl::StartWorker(const std::string& id, const std::stop_token& admission,
                                                        Deadline deadline, std::stop_token stop) {
  std::unique_lock lock(mutex);
  if (closed || admission.stop_requested()) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
  const auto installed = catalog.find(id);
  if (installed == catalog.end() || !installed->second.enabled) throw Error(ErrorCode::Closed, "The plugin is disabled");

  if (const auto running = workers.find(id); running != workers.end()) {
    if (running->second->Available() && running->second->Pid()) return running->second;
    running->second->Stop();
    workers.erase(running);
  }

  const auto package = VerifyPackage(installed->second);
  auto startup = Startup(package.info);
  std::vector<Worker::Service> granted;
  for (auto& service : ApprovedServices(installed->second)) {
    if (service.dispatch) granted.push_back({std::move(service.id), service.version, std::move(service.fingerprint),
                                             std::move(service.dispatch)});
  }
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
  if (std::chrono::steady_clock::now() >= deadline) {
    throw Error(ErrorCode::DeadlineExceeded, "The plugin call exceeded its deadline before starting the worker");
  }
  auto worker = std::make_shared<Worker>(*worker_executable, PackageDirectory(package.info.digest) / ModuleName,
      package.module_file, ContractInfo{package.info.contract_id, package.info.contract_version,
                                       package.info.contract_fingerprint}, std::move(granted), callbacks,
      std::move(startup));
  workers.emplace(id, worker);
  return worker;
}

} // namespace huxerui::pluginkit
