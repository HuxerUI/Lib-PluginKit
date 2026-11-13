#include "digest.h"
#include "worker_process.h"
#include "run_transport.h"

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
        extension.at("id").get<std::string>(), extension.at("extensions").get<std::vector<std::string>>()};
    if (!IsId(info.id) || !IsId(info.version) || !IsId(info.contract_id) || !IsId(info.point) ||
        !IsId(info.contribution) || !IsDigest(info.contract_fingerprint) || info.extensions.size() > 32) {
      throw Error(ErrorCode::InvalidPackage, "Invalid plugin identity, contract, or extension metadata");
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
      if (!catalog.emplace(id, Installed{std::move(info), std::move(manifest), plugin.at("enabled").get<bool>()}).second) {
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
    plugins.push_back({{"digest", installed.info.digest}, {"manifest", installed.manifest}, {"enabled", installed.enabled}});
  }
  const auto text = Json{{"schema_version", 1}, {"plugins", std::move(plugins)}}.dump(2);
  if (text.size() > MaxProfile) throw Error(ErrorCode::PayloadTooLarge, "The plugin profile exceeds the byte limit");
  WriteAtomicFile(file, AsBytes(text));
}

struct RegisteredContract {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;

  ContractInfo Info() const { return {id, version, fingerprint}; }
};

} // namespace

struct PluginManager::Impl {
  Impl(const fs::path& directory, const fs::path& executable, TaskScope callbacks)
      : profile(fs::absolute(directory)), worker_executable(fs::absolute(executable)), callbacks(std::move(callbacks)) {
    fs::create_directories(profile);
    ownership = LockProfile(profile);
    catalog = ReadCatalog(profile / "profile.json");
  }

  ~Impl() {
    for (const auto& [id, worker] : workers) worker->Shutdown();
  }

  void Shutdown() {
    std::call_once(shutdown, [&] {
      decltype(workers) running;
      {
        std::lock_guard lock(mutex);
        closed = true;
        running.swap(workers);
      }
      for (const auto& [id, worker] : running) worker->BeginShutdown();
      for (const auto& [id, worker] : running) worker->Shutdown();
    });
  }

  void Commit(Catalog candidate) {
    // Publish the candidate in memory after the profile file is committed.
    SaveCatalog(profile / "profile.json", candidate);
    catalog.swap(candidate);
  }

  fs::path PackageDirectory(std::string_view digest) const { return profile / "packages" / std::string(digest); }

  void RequirePoint(std::string_view point, ContractInfo expected) const {
    const auto found = points.find(point);
    if (found == points.end()) throw Error(ErrorCode::NotFound, "The extension point is not registered");
    if (found->second.Info() != expected) {
      throw Error(ErrorCode::ContractMismatch, "The registered extension point uses a different contract");
    }
  }

  fs::path profile;
  fs::path worker_executable;
  TaskScope callbacks;
  Handle ownership;
  mutable std::mutex mutex;
  Catalog catalog;
  std::map<std::string, RegisteredContract, std::less<>> points;
  std::map<std::string, std::vector<Worker::Service>, std::less<>> services;
  std::map<std::string, std::shared_ptr<Worker>, std::less<>> workers;
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
  }
  return "unknown";
}

PluginManager::PluginManager(const File& profile, const File& executable, TaskScope callbacks)
    : impl_(std::make_shared<Impl>(NativePath(profile), NativePath(executable), std::move(callbacks))) {}

PluginManager::~PluginManager() {
  if (impl_) impl_->Shutdown();
}

Task<void> PluginManager::Shutdown() {
  return ShutdownManager(impl_);
}

Task<void> PluginManager::ShutdownManager(std::shared_ptr<Impl> impl) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  co_await RunTransport([impl](std::stop_token) { impl->Shutdown(); });
}

void PluginManager::Register(std::string_view point, ContractInfo contract) {
  RejectServiceReentry();
  if (!IsId(point) || !IsId(contract.id) || !contract.version || !IsDigest(contract.fingerprint)) {
    throw Error(ErrorCode::InvalidArgument, "Invalid extension point or contract identity");
  }
  std::lock_guard lock(impl_->mutex);
  if (!impl_->points.try_emplace(std::string(point), std::string(contract.id), contract.version,
                                 std::string(contract.fingerprint)).second) {
    throw Error(ErrorCode::InvalidArgument, "The extension point is already registered");
  }
}

PackageInfo PluginManager::InspectPackage(const File& directory) {
  RejectServiceReentry();
  return ReadPackage(NativePath(directory)).info;
}

void PluginManager::Provide(ContractInfo contract, std::string_view approved_digest, ServiceDispatch dispatch) {
  RejectServiceReentry();
  if (!IsId(contract.id) || !contract.version || !IsDigest(contract.fingerprint) || !IsDigest(approved_digest)) {
    throw Error(ErrorCode::InvalidArgument, "Invalid host service contract or package digest");
  }
  std::lock_guard lock(impl_->mutex);
  auto& services = impl_->services[std::string(approved_digest)];
  if (std::ranges::find(services, contract.id, &Worker::Service::id) != services.end()) {
    throw Error(ErrorCode::InvalidArgument, "The host service is already registered for this package");
  }
  services.push_back({std::string(contract.id), contract.version, std::string(contract.fingerprint), std::move(dispatch)});
}

void PluginManager::InstallPackage(const File& directory, std::string_view approved_digest) {
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

  const auto current = impl_->catalog.find(package.info.id);
  if (current != impl_->catalog.end() && current->second.info.digest != package.info.digest) {
    throw Error(ErrorCode::AlreadyInstalled, "A different package is installed for this plugin ID");
  }
  const auto destination = impl_->PackageDirectory(package.info.digest);
  if (current != impl_->catalog.end()) {
    try {
      if (ReadPackage(destination).info.digest == approved_digest) return;
    } catch (const Error&) {
      // Reinstall the approved bytes if the stored copy is unreadable.
    }
    if (const auto running = impl_->workers.find(package.info.id); running != impl_->workers.end()) {
      running->second->Shutdown();
      impl_->workers.erase(running);
    }
  }

  fs::create_directories(destination);
  // Put both package files in place before publishing a new catalog entry.
  WriteAtomicFile(destination / "plugin.json", package.manifest_bytes);
  WriteAtomicFile(destination / ModuleName, package.module_bytes);
  if (current == impl_->catalog.end()) {
    auto candidate = impl_->catalog;
    const auto id = package.info.id;
    candidate.emplace(id, Installed{std::move(package.info), std::move(package.manifest), true});
    impl_->Commit(std::move(candidate));
  }
}

Task<void> PluginManager::SetEnabled(std::string_view id, bool enabled) {
  return ChangeEnabled(impl_, std::string(id), enabled);
}

Task<void> PluginManager::ChangeEnabled(std::shared_ptr<Impl> impl, std::string id, bool enabled) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  auto change = [impl, id = std::move(id), enabled](std::stop_token stop) {
    std::shared_ptr<Worker> worker;
    {
      std::lock_guard lock(impl->mutex);
      if (stop.stop_requested()) return;
      if (impl->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
      const auto found = impl->catalog.find(id);
      if (found == impl->catalog.end()) throw Error(ErrorCode::NotFound, "The plugin is not installed");
      if (found->second.enabled == enabled) return;
      auto candidate = impl->catalog;
      candidate.find(id)->second.enabled = enabled;
      impl->Commit(std::move(candidate));
      if (!enabled) {
        if (const auto running = impl->workers.find(id); running != impl->workers.end()) {
          worker = running->second;
          worker->BeginShutdown();
        }
      }
    }
    if (worker) {
      worker->Shutdown();
      std::lock_guard lock(impl->mutex);
      const auto running = impl->workers.find(id);
      if (running != impl->workers.end() && running->second == worker) impl->workers.erase(running);
    }
  };
  co_await RunTransport(std::move(change));
}

std::vector<PluginInfo> PluginManager::ListPlugins() const {
  // Read the snapshot without entering the worker's request queue, including during a host callback.
  std::lock_guard lock(impl_->mutex);
  std::vector<PluginInfo> result;
  for (const auto& [id, plugin] : impl_->catalog) {
    const auto worker = impl_->workers.find(id);
    result.push_back({plugin.info, plugin.enabled, worker == impl_->workers.end() ? std::nullopt : worker->second->Pid()});
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

Task<ExtensionInstance> PluginManager::Create(std::shared_ptr<Impl> impl, std::string point, ContractInfo contract,
                                             ExtensionId id, std::chrono::milliseconds timeout) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  auto create = [impl, point = std::move(point), contract, id = std::move(id), timeout](std::stop_token stop) {
    return CreateWorkerInstance(*impl, point, contract, id, timeout, stop);
  };
  co_return co_await RunTransport(std::move(create));
}

ExtensionInstance PluginManager::CreateWorkerInstance(Impl& state, std::string_view point, ContractInfo contract,
                                                       const ExtensionId& id, std::chrono::milliseconds timeout,
                                                       std::stop_token stop) {
  std::unique_lock lock(state.mutex);
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
  if (state.closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  state.RequirePoint(point, contract);
  const auto plugin = state.catalog.find(id.plugin);
  if (plugin == state.catalog.end() || plugin->second.info.point != point ||
      plugin->second.info.contribution != id.contribution) {
    throw Error(ErrorCode::NotFound, "The extension is not installed at this point");
  }
  const auto& installed = plugin->second;
  if (!installed.enabled) throw Error(ErrorCode::Disabled, "The plugin is disabled");
  if (!Matches(installed.info, contract)) throw Error(ErrorCode::ContractMismatch, "The installed contract differs");

  std::stop_source lifetime;
  const auto running = state.workers.find(id.plugin);
  if (running != state.workers.end()) {
    if (running->second->Pid()) {
      auto worker = running->second;
      lock.unlock();
      const auto reply = worker->Request({static_cast<std::uint32_t>(Command::Create), 0, 0, {}}, timeout, nullptr, stop);
      return ExtensionInstance(std::move(worker), reply.instance, std::move(lifetime));
    }
    running->second->Stop();
    state.workers.erase(running);
  }

  const auto directory = state.PackageDirectory(installed.info.digest);
  const auto package = [&] {
    try {
      return ReadPackage(directory);
    } catch (const Error& error) {
      throw Error(ErrorCode::PackageChanged, "Cannot verify the installed package: " + std::string(error.what()));
    }
  }();
  if (package.info.digest != installed.info.digest || package.info.id != id.plugin || package.info.point != point ||
      package.info.contribution != id.contribution || !Matches(package.info, contract)) {
    throw Error(ErrorCode::PackageChanged, "The installed package differs from the approved package");
  }
  // The fixed prefix and suffix keep valid plugin IDs distinct under Windows path normalization.
  const auto data = state.profile / "data" / ("plugin-" + package.info.id + ".data");
  std::error_code directory_error;
  fs::create_directories(data, directory_error);
  if (directory_error) throw Error(ErrorCode::Io, "Cannot create the plugin data directory: " + directory_error.message());
  auto startup = PluginStartup{package.info.id, package.info.version, package.info.digest,
                               PathText(directory), PathText(data)}.Encode();
  // The open file lets the Linux worker load the same library that was verified above.
  const auto services = state.services.find(installed.info.digest);
  auto worker = std::make_shared<Worker>(state.worker_executable, directory / ModuleName, package.module_file, contract,
                                         services == state.services.end() ? std::vector<Worker::Service>{}
                                                                          : services->second, state.callbacks);
  const auto started = std::chrono::steady_clock::now();
  try {
    const auto reply = worker->Request({static_cast<std::uint32_t>(Command::Create), 0, 0, std::move(startup)},
                                       timeout, nullptr, stop);
    // Publish a generation only after activation and its first instance creation succeed.
    state.workers.emplace(id.plugin, worker);
    return ExtensionInstance(std::move(worker), reply.instance, std::move(lifetime));
  } catch (...) {
    // A failed first constructor can leave an activated plugin. Teardown uses the remaining creation budget.
    const auto elapsed = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed < timeout) worker->Shutdown(timeout - elapsed);
    else worker->Stop();
    throw;
  }
}

} // namespace huxerui::pluginkit
