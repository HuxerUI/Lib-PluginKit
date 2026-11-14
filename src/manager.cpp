#include "digest.h"
#include "extension.h"
#include "plugin_module.h"
#include "run_transport.h"

#include <huxerui/pluginkit/plugin.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <set>

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

const PackageContract& ProvidedContract(const PackageInfo& info, std::string_view id) {
  const auto found = std::ranges::find(info.provides, id, &PackageContract::id);
  if (found == info.provides.end()) throw Error(ErrorCode::InvalidPackage, "An extension references an undeclared contract");
  return *found;
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
    PackageInfo info{
        manifest.at("id").get<std::string>(), manifest.at("version").get<std::string>(), std::move(digest),
        {}, {}, {}, manifest.at("cpp_abi").get<std::string>()};
    if (!IsId(info.id) || !IsId(info.version)) throw Error(ErrorCode::InvalidPackage, "Invalid plugin identity");
    const auto& provides = manifest.at("provides");
    const auto& extensions = manifest.at("extensions");
    if (!provides.is_array() || provides.empty() || provides.size() > HPK_MAX_EXTENSIONS ||
        !extensions.is_array() || extensions.empty() || extensions.size() > HPK_MAX_EXTENSIONS) {
      throw Error(ErrorCode::InvalidPackage, "Invalid provided contract or extension list");
    }
    for (const auto& contract : provides) {
      const auto& version = contract.at("version");
      PackageContract entry{contract.at("id").get<std::string>(), 0, contract.at("fingerprint").get<std::string>()};
      if (!IsId(entry.id) || !IsDigest(entry.fingerprint) || !version.is_number_unsigned() ||
          version == 0 || version > UINT32_MAX ||
          std::ranges::find(info.provides, entry.id, &PackageContract::id) != info.provides.end()) {
        throw Error(ErrorCode::InvalidPackage, "Invalid or duplicate provided contract");
      }
      entry.version = version.get<std::uint32_t>();
      info.provides.push_back(std::move(entry));
    }
    for (const auto& extension : extensions) {
      if (extension.contains("id")) throw Error(ErrorCode::InvalidPackage, "An extension is identified by its point and order");
      PackageExtension entry{extension.at("point").get<std::string>(), extension.at("contract").get<std::string>(), {}};
      if (const auto metadata = extension.find("metadata"); metadata != extension.end()) {
        if (!metadata->is_object()) throw Error(ErrorCode::InvalidPackage, "Invalid extension metadata");
        entry.file_extensions = metadata->value("extensions", std::vector<std::string>{});
      }
      if (!IsId(entry.point) || entry.file_extensions.size() > 32) {
        throw Error(ErrorCode::InvalidPackage, "Invalid extension point or metadata");
      }
      (void)ProvidedContract(info, entry.contract);
      for (const auto& suffix : entry.file_extensions) {
        if (!suffix.starts_with('.') || !IsId(std::string_view(suffix).substr(1))) {
          throw Error(ErrorCode::InvalidPackage, "Invalid file suffix in the manifest");
        }
      }
      const auto same_point = std::ranges::find(info.extensions, entry.point, &PackageExtension::point);
      if (same_point != info.extensions.end() && same_point->contract != entry.contract) {
        throw Error(ErrorCode::InvalidPackage, "Factories at one extension point must use the same contract");
      }
      info.extensions.push_back(std::move(entry));
    }
    for (const auto& contract : info.provides) {
      if (std::ranges::find(info.extensions, contract.id, &PackageExtension::contract) == info.extensions.end()) {
        throw Error(ErrorCode::InvalidPackage, "A provided contract has no extension factory");
      }
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
  std::string data;
};

struct PendingUpgrade {
  Installed target;
  std::string expected_digest;
  std::string process;
  bool applying;
};

using Catalog = std::map<std::string, Installed, std::less<>>;

struct Profile {
  Catalog plugins;
  std::map<std::string, PendingUpgrade, std::less<>> upgrades;
  std::map<std::string, std::string, std::less<>> retained_data;
  std::set<std::string, std::less<>> discarded_data;
};

std::string DefaultDataName(std::string_view id) { return "plugin-" + std::string(id) + ".data"; }

bool IsDataName(std::string_view id, std::string_view name) {
  const auto base = DefaultDataName(id);
  return name == base || (name.starts_with(base + ".") && IsDigest(name.substr(base.size() + 1)));
}

bool IsManagedDataName(std::string_view name) {
  const auto suffix = name.rfind(".data");
  if (!name.starts_with("plugin-") || suffix == std::string_view::npos || suffix <= 7) return false;
  const auto id = name.substr(7, suffix - 7);
  return IsId(id) && IsDataName(id, name);
}

void RequireManagedDirectory(const fs::path& directory) {
  // Keep package and data operations inside the profile, including when deleting an abandoned snapshot.
  const auto status = fs::symlink_status(directory);
  if (status.type() == fs::file_type::not_found) return;
  if (!fs::is_directory(status)) throw Error(ErrorCode::Io, "Expected a managed directory at " + PathText(directory));
#if defined(_WIN32)
  const auto attributes = GetFileAttributesW(directory.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) WindowsError(ErrorCode::Io, "Cannot inspect a managed directory");
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    throw Error(ErrorCode::Io, "A managed directory cannot redirect outside the profile");
  }
#endif
}

Installed ReadInstalled(const Json& entry) {
  const auto digest = entry.at("digest").get<std::string>();
  if (!IsDigest(digest)) throw Error(ErrorCode::Io, "Invalid package digest in the profile");
  auto manifest = entry.at("manifest");
  auto info = ParseManifest(manifest, digest);
  InstallOptions approval{ParseMode(entry.at("mode").get<std::string>(), ErrorCode::Io),
                          entry.at("services").get<std::vector<std::string>>()};
  auto data = entry.at("data").get<std::string>();
  if (!IsDataName(info.id, data) ||
      std::ranges::find(info.execution_modes, approval.mode) == info.execution_modes.end() ||
      approval.services.size() > 64 || !std::ranges::is_sorted(approval.services) ||
      std::ranges::adjacent_find(approval.services) != approval.services.end() ||
      !std::ranges::all_of(approval.services, [](const auto& service) { return IsId(service); })) {
    throw Error(ErrorCode::Io, "Invalid plugin data or approval in the profile");
  }
  return {std::move(info), std::move(manifest), entry.at("enabled").get<bool>(), std::move(approval), std::move(data)};
}

Json DescribeInstalled(const Installed& plugin) {
  return {{"digest", plugin.info.digest}, {"manifest", plugin.manifest}, {"enabled", plugin.enabled},
          {"mode", ModeName(plugin.approval.mode)}, {"services", plugin.approval.services}, {"data", plugin.data}};
}

Profile ReadProfile(const fs::path& file) {
  std::error_code error;
  const auto status = fs::symlink_status(file, error);
  if (status.type() == fs::file_type::not_found) return {};
  if (error) throw Error(ErrorCode::Io, "Cannot inspect the plugin profile: " + error.message());
  try {
    const auto bytes = ReadFileBytes(file, MaxProfile);
    const auto profile = ParseJson(bytes, MaxManifestDepth + 4, ErrorCode::Io);
    const auto& plugins = profile.at("plugins");
    const auto& upgrades = profile.at("upgrades");
    const auto& retained = profile.at("retained_data");
    const auto& discarded = profile.at("discarded_data");
    if (profile.at("schema_version") != 1 || !plugins.is_array() || !upgrades.is_array() ||
        !retained.is_object() || !discarded.is_array()) {
      throw Error(ErrorCode::Io, "Invalid plugin profile format");
    }
    Profile result;
    for (const auto& plugin : plugins) {
      auto installed = ReadInstalled(plugin);
      const auto id = installed.info.id;
      if (!result.plugins.emplace(id, std::move(installed)).second) {
        throw Error(ErrorCode::Io, "Duplicate plugin ID in the profile");
      }
    }
    for (const auto& entry : upgrades) {
      PendingUpgrade upgrade{ReadInstalled(entry.at("target")), entry.at("expected_digest").get<std::string>(),
                             entry.at("process").get<std::string>(), entry.at("applying").get<bool>()};
      const auto id = upgrade.target.info.id;
      const auto current = result.plugins.find(id);
      if (current == result.plugins.end() || current->second.info.digest != upgrade.expected_digest ||
          current->second.approval.mode != upgrade.target.approval.mode ||
          current->second.info.version == upgrade.target.info.version ||
          current->second.info.digest == upgrade.target.info.digest || !IsDigest(upgrade.process) ||
          (!upgrade.applying && upgrade.target.approval.mode != ExecutionMode::InProcess) ||
          upgrade.target.data == current->second.data ||
          !result.upgrades.emplace(id, std::move(upgrade)).second) {
        throw Error(ErrorCode::Io, "Invalid pending upgrade in the profile");
      }
    }
    for (const auto& [id, value] : retained.items()) {
      auto data = value.get<std::string>();
      if (!IsId(id) || !IsDataName(id, data) || result.plugins.contains(id)) {
        throw Error(ErrorCode::Io, "Invalid retained data in the profile");
      }
      result.retained_data.emplace(id, std::move(data));
    }
    for (const auto& entry : discarded) {
      auto name = entry.get<std::string>();
      if (!IsManagedDataName(name)) throw Error(ErrorCode::Io, "Invalid data cleanup entry in the profile");
      result.discarded_data.insert(std::move(name));
    }
    return result;
  } catch (const Json::exception& error) {
    throw Error(ErrorCode::Io, "Cannot parse the plugin profile: " + std::string(error.what()));
  }
}

void SaveProfile(const fs::path& file, const Profile& profile) {
  auto plugins = Json::array();
  for (const auto& [id, installed] : profile.plugins) plugins.push_back(DescribeInstalled(installed));
  auto upgrades = Json::array();
  for (const auto& [id, upgrade] : profile.upgrades) {
    upgrades.push_back({{"target", DescribeInstalled(upgrade.target)}, {"expected_digest", upgrade.expected_digest},
                        {"process", upgrade.process}, {"applying", upgrade.applying}});
  }
  const auto text = Json{{"schema_version", 1}, {"plugins", std::move(plugins)}, {"upgrades", std::move(upgrades)},
                         {"retained_data", profile.retained_data}, {"discarded_data", profile.discarded_data}}.dump(2);
  if (text.size() > MaxProfile) throw Error(ErrorCode::PayloadTooLarge, "The plugin profile exceeds the byte limit");
  WriteAtomicFile(file, AsBytes(text));
}

std::string NewToken() {
  std::random_device random;
  std::array<std::uint32_t, 8> values;
  for (auto& value : values) value = random();
  return Sha256(std::as_bytes(std::span(values)));
}

const std::string& HostProcessToken() {
  static const auto token = NewToken();
  return token;
}

void CopyPluginData(const fs::path& source, const fs::path& destination) {
  if (!fs::create_directory(destination)) throw Error(ErrorCode::Io, "The candidate data directory already exists");
  const auto status = fs::symlink_status(source);
  if (status.type() == fs::file_type::not_found) return;
  RequireManagedDirectory(source);
  for (const auto& entry : fs::recursive_directory_iterator(source)) {
    const auto target = destination / entry.path().lexically_relative(source);
    const auto type = entry.symlink_status();
    if (fs::is_directory(type)) {
      RequireManagedDirectory(entry.path());
      fs::create_directory(target);
    } else if (fs::is_regular_file(type)) {
      fs::copy_file(entry.path(), target);
    } else {
      throw Error(ErrorCode::Io, "Unsupported entry in plugin data: " + PathText(entry.path()));
    }
  }
}

void RemoveUnusedDirectories(const fs::path& root, const std::set<std::string, std::less<>>& retained,
                             const auto& eligible) {
  std::error_code error;
  auto entries = fs::directory_iterator(root, error);
  const fs::directory_iterator end;
  for (; !error && entries != end; entries.increment(error)) {
    const auto path = entries->path();
    const auto name = PathText(path.filename());
    if (!retained.contains(name) && eligible(path, name)) {
      std::error_code removal;
      fs::remove_all(path, removal);
    }
  }
}

template<class Result, class Impl, class Work>
Task<Result> RunManagement(std::shared_ptr<Impl> impl, Work work) {
  RejectServiceReentry(co_await CurrentTaskExecution{});
  auto start = [impl, work = std::move(work)](auto complete) mutable {
    (void)impl->callbacks.Launch([impl, work = std::move(work), complete]() mutable -> Task<void> {
      try {
        if constexpr (std::is_void_v<Result>) {
          co_await work(*impl);
          complete.Complete();
        } else {
          complete.Complete(co_await work(*impl));
        }
      } catch (...) {
        complete.Fail(std::current_exception());
      }
    });
    return std::function<void()>{};
  };
  co_return co_await AwaitCallback<Result>(std::move(start));
}

struct RegisteredContract {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;
  bool remote;

  ContractInfo Info() const { return {id, version, fingerprint}; }
};

} // namespace

struct PluginManager::Impl : std::enable_shared_from_this<PluginManager::Impl> {
  Impl(const File& directory, const std::optional<File>& executable, TaskScope callbacks)
      : profile(fs::absolute(NativePath(directory))),
        worker_executable(executable ? std::optional(fs::absolute(NativePath(*executable))) : std::nullopt),
        callbacks(std::move(callbacks)) {
    fs::create_directories(profile);
    profile = fs::canonical(profile);
    ownership = LockProfile(profile);
    RequireManagedDirectory(profile / "packages");
    RequireManagedDirectory(profile / "data");
    profile_state = ReadProfile(profile / "profile.json");
    if (std::ranges::any_of(profile_state.upgrades, [](const auto& entry) { return entry.second.applying; })) {
      auto recovered = profile_state;
      std::erase_if(recovered.upgrades, [&](const auto& entry) {
        if (!entry.second.applying) return false;
        recovered.discarded_data.insert(entry.second.target.data);
        return true;
      });
      Commit(std::move(recovered));
    }
    CleanupFilesAfterCommit();
  }

  void ShutdownWorkers() {
    std::call_once(shutdown, [&] {
      decltype(workers) running;
      decltype(retired_workers) retired;
      {
        std::lock_guard lock(mutex);
        running.swap(workers);
        retired.swap(retired_workers);
      }
      for (const auto& [id, worker] : running) worker->BeginShutdown();
      for (const auto& [id, worker] : running) worker->Shutdown();
      for (const auto& [digest, entry] : retired) entry.worker->Shutdown();
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
  void ValidatePackage(const PackageInfo& info, InstallOptions& approval) const;
  void StorePackage(const Package& package);
  PluginStartup Startup(const Installed& installed) const;
  std::shared_ptr<LocalPlugin> MakeLocal(const Installed& installed);
  std::shared_ptr<Worker> MakeWorker(const Installed& installed, const Package& package,
                                     std::vector<Worker::Service> granted);
  std::vector<Worker::Service> WorkerServices(const Installed& installed) const;
  std::vector<std::shared_ptr<Worker>> ActiveAndRetiredWorkers(std::string_view id) const;
  std::shared_ptr<Worker> StartWorker(const std::string& id, const std::stop_token& admission,
                                     Deadline deadline, std::stop_token stop);
  Task<void> ChangeLocalEnabled(const std::string& id, bool enabled);
  void ChangeWorkerEnabled(const std::string& id, bool enabled, std::stop_token stop);
  Task<void> SetEnabled(std::string id, bool enabled);
  Task<UpgradeStatus> Upgrade(File directory, UpgradeApproval approval);
  Task<UninstallStatus> Uninstall(std::string id, std::string expected_digest, PluginDataPolicy data);
  Task<std::vector<PendingChangeResult>> ApplyPending();
  Task<UpgradeStatus> ApplyUpgrade(std::string id, std::optional<Package> source, UpgradeApproval approval = {});
  Task<void> RetirePlugin(const std::string& id);
  void CleanupFiles();
  bool IsPluginBusy(std::string_view id, const Installed& installed) const;
  void CleanupFilesAfterCommit() noexcept;
  void PruneExitedWorkers();

  void Commit(Profile candidate) {
    // Publish the candidate in memory after the profile file is committed.
    SaveProfile(profile / "profile.json", candidate);
    std::swap(profile_state, candidate);
  }

  const Installed& RequireInstalled(std::string_view id) const {
    if (closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
    const auto found = profile_state.plugins.find(id);
    if (found == profile_state.plugins.end()) throw Error(ErrorCode::NotFound, "The plugin is not installed");
    return found->second;
  }

  void CommitEnabled(std::string_view id, bool enabled) {
    auto candidate = profile_state;
    candidate.plugins.find(id)->second.enabled = enabled;
    Commit(std::move(candidate));
  }

  void RequireIdle(std::string_view id) const {
    if (changes.contains(id)) throw Error(ErrorCode::Busy, "The plugin has a management operation in progress");
  }

  struct Change {
    Impl& manager;
    const std::string& id;
    bool exclusive;

    // The caller holds the manager mutex until admission has been reserved.
    Change(Impl& manager, const std::string& id, bool exclusive = true)
        : manager(manager), id(id), exclusive(exclusive) {
      const auto current = manager.changes.find(id);
      if (current != manager.changes.end() && (exclusive || current->second == 0)) {
        throw Error(ErrorCode::Busy, "The plugin has a management operation in progress");
      }
      if (exclusive) manager.changes.emplace(id, 0);
      else ++manager.changes[id];
    }
    ~Change() {
      std::lock_guard lock(manager.mutex);
      const auto current = manager.changes.find(id);
      if (exclusive || --current->second == 0) manager.changes.erase(current);
    }
    Change(const Change&) = delete;
    Change& operator=(const Change&) = delete;
  };

  fs::path PackageDirectory(std::string_view digest) const { return profile / "packages" / std::string(digest); }
  fs::path DataDirectory(std::string_view name) const { return profile / "data" / std::string(name); }

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
  Profile profile_state;
  // Zero is an exclusive package change. A positive value counts in-flight enablement changes.
  std::map<std::string, std::size_t, std::less<>> changes;
  struct RetiredWorker {
    std::string id;
    std::shared_ptr<Worker> worker;
    std::string data;
  };
  std::map<std::string, RetiredWorker, std::less<>> retired_workers;
  std::map<std::string, RegisteredContract, std::less<>> points;
  std::map<std::string, RegisteredService, std::less<>> services;
  std::map<std::string, std::shared_ptr<Worker>, std::less<>> workers;
  std::map<std::string, std::shared_ptr<LocalPlugin>, std::less<>> local_plugins;
  std::map<std::string, std::stop_source, std::less<>> admissions;
  bool closed = false;
  std::once_flag shutdown;
};

void PluginManager::Impl::CleanupFiles() {
  RequireManagedDirectory(profile / "packages");
  RequireManagedDirectory(profile / "data");
  std::set<std::string, std::less<>> packages;
  std::set<std::string, std::less<>> data;
  for (const auto& [id, plugin] : profile_state.plugins) {
    packages.insert(plugin.info.digest);
    data.insert(plugin.data);
  }
  for (const auto& [id, upgrade] : profile_state.upgrades) {
    packages.insert(upgrade.target.info.digest);
    data.insert(upgrade.target.data);
  }
  for (const auto& [id, directory] : profile_state.retained_data) data.insert(directory);

  // A mapped library may still be referenced by C++ code. Its directory can be cleaned in a later process.
  RemoveUnusedDirectories(profile / "packages", packages,
      [&](const fs::path& path, std::string_view name) {
        return IsDigest(name) && !retired_workers.contains(name) && !IsInProcessModuleMapped(path / ModuleName);
      });
  auto remaining = profile_state.discarded_data;
  std::erase_if(remaining, [&](const auto& name) {
    if (data.contains(name) || std::ranges::any_of(retired_workers, [&](const auto& retired) {
          return retired.second.data == name;
        })) return false;
    std::error_code removal;
    fs::remove_all(DataDirectory(name), removal);
    return !removal;
  });
  if (remaining.size() != profile_state.discarded_data.size()) {
    auto next = profile_state;
    next.discarded_data = std::move(remaining);
    Commit(std::move(next));
  }
}

void PluginManager::Impl::CleanupFilesAfterCommit() noexcept {
  try {
    CleanupFiles();
  } catch (...) {
    // The committed profile remains authoritative. Opening it again retries file cleanup.
  }
}

void PluginManager::Impl::PruneExitedWorkers() {
  std::erase_if(retired_workers, [](const auto& entry) {
    try {
      entry.second.worker->WaitForExit(std::chrono::milliseconds::zero());
      return true;
    } catch (const Error&) {
      return false;
    }
  });
}

bool PluginManager::Impl::IsPluginBusy(std::string_view id, const Installed& installed) const {
  if (const auto change = changes.find(id); change != changes.end() && change->second == 0) return true;
  const auto pending = profile_state.upgrades.find(id);
  if (pending != profile_state.upgrades.end() && pending->second.applying) return true;
  if (installed.approval.mode != ExecutionMode::InProcess) return false;
  if (InProcessVersionRequiresRestart(profile, id, installed.info.digest)) return true;
  return pending != profile_state.upgrades.end() && pending->second.process != HostProcessToken();
}

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
    case ErrorCode::Ambiguous: return "ambiguous";
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
  for (;;) {
    {
      std::lock_guard lock(impl->mutex);
      if (impl->changes.empty()) break;
    }
    co_await Delay(std::chrono::milliseconds(1));
  }
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

void PluginManager::Impl::ValidatePackage(const PackageInfo& info, InstallOptions& options) const {
  if (std::ranges::find(info.execution_modes, options.mode) == info.execution_modes.end() ||
      (options.mode == ExecutionMode::WorkerProcess && !worker_executable)) {
    throw Error(ErrorCode::UnsupportedMode, "The package or host cannot use the selected execution mode");
  }
  for (const auto& extension : info.extensions) {
    const auto point = points.find(extension.point);
    if (point == points.end() || ProvidedContract(info, extension.contract).Info() != point->second.Info()) {
      throw Error(ErrorCode::ContractMismatch, "The package does not match extension point " + extension.point);
    }
    if (options.mode == ExecutionMode::WorkerProcess && !point->second.remote) {
      throw Error(ErrorCode::UnsupportedMode, "The extension point cannot use a worker: " + extension.point);
    }
  }
  if (options.mode == ExecutionMode::InProcess && info.cpp_abi != CppAbi) {
    throw Error(ErrorCode::ContractMismatch, "The package requires a different in-process SDK or C++ ABI");
  }
  std::ranges::sort(options.services);
  if (options.services.size() > 64 || std::ranges::adjacent_find(options.services) != options.services.end() ||
      !std::ranges::all_of(options.services, [](const auto& service) { return IsId(service); })) {
    throw Error(ErrorCode::InvalidArgument, "Invalid approved service list");
  }
  for (const auto& service : options.services) {
    const auto found = services.find(service);
    if (found == services.end() || (options.mode == ExecutionMode::WorkerProcess && !found->second.dispatch)) {
      throw Error(ErrorCode::InvalidArgument, "The approved service is unavailable in the selected mode: " + service);
    }
  }
}

void PluginManager::Impl::StorePackage(const Package& package) {
  const auto destination = PackageDirectory(package.info.digest);
  RequireManagedDirectory(destination.parent_path());
  RequireManagedDirectory(destination);
  try {
    if (ReadPackage(destination).info.digest == package.info.digest) return;
  } catch (const Error&) {
    // Restore the approved bytes if a previous write left an incomplete snapshot.
  }
  if (IsInProcessModuleMapped(destination / ModuleName)) {
    throw Error(ErrorCode::Busy, "Restart the host before replacing a mapped plugin library");
  }
  fs::create_directories(destination);
  // Both files must be in place before the profile can reference the package.
  WriteAtomicFile(destination / "plugin.json", package.manifest_bytes);
  WriteAtomicFile(destination / ModuleName, package.module_bytes);
}

void PluginManager::InstallPackage(const File& directory, std::string_view approved_digest, InstallOptions options) {
  RejectServiceReentry();
  auto package = ReadPackage(NativePath(directory));
  if (package.info.digest != approved_digest) {
    throw Error(ErrorCode::ApprovalMismatch, "The package contents differ from the approved digest");
  }
  package.module_file.reset();
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  impl_->RequireIdle(package.info.id);
  impl_->PruneExitedWorkers();
  impl_->ValidatePackage(package.info, options);
  const auto& installed = impl_->profile_state.plugins;
  const auto current = installed.find(package.info.id);
  if (current != installed.end() && current->second.info.digest != package.info.digest) {
    throw Error(ErrorCode::AlreadyInstalled, "A different package is installed for this plugin ID");
  }
  if (current != installed.end() &&
      (current->second.approval.mode != options.mode || current->second.approval.services != options.services)) {
    throw Error(ErrorCode::AlreadyInstalled, "The installed package already has a different approval");
  }
  const auto destination = impl_->PackageDirectory(package.info.digest);
  if (current != installed.end()) {
    try {
      if (ReadPackage(destination).info.digest == approved_digest) return;
    } catch (const Error&) {
      // Reinstall the approved bytes if the stored copy is unreadable.
    }
    if (impl_->workers.contains(package.info.id) || impl_->local_plugins.contains(package.info.id) ||
        impl_->retired_workers.contains(package.info.digest)) {
      throw Error(ErrorCode::Busy, "Disable the plugin before repairing its package");
    }
  }

  impl_->StorePackage(package);
  if (current == installed.end()) {
    auto candidate = impl_->profile_state;
    const auto id = package.info.id;
    auto retained = candidate.retained_data.extract(id);
    auto data = retained.empty() ? DefaultDataName(id) : std::move(retained.mapped());
    if (retained.empty() && candidate.discarded_data.contains(data)) {
      // A previous delete can still be awaiting cleanup. A new installation gets fresh data.
      data += "." + NewToken();
    }
    candidate.plugins.emplace(id, Installed{std::move(package.info), std::move(package.manifest), true,
                                           std::move(options), std::move(data)});
    impl_->Commit(std::move(candidate));
    impl_->CleanupFilesAfterCommit();
  }
}

Task<UpgradeStatus> PluginManager::UpgradePackage(File directory, UpgradeApproval approval) {
  return RunManagement<UpgradeStatus>(impl_, [directory = std::move(directory), approval = std::move(approval)]
                                       (Impl& impl) mutable {
    return impl.Upgrade(std::move(directory), std::move(approval));
  });
}

Task<UninstallStatus> PluginManager::UninstallPackage(std::string_view id, std::string_view expected_digest,
                                                      PluginDataPolicy data) {
  return RunManagement<UninstallStatus>(impl_, [id = std::string(id), digest = std::string(expected_digest), data]
                                         (Impl& impl) mutable {
    return impl.Uninstall(std::move(id), std::move(digest), data);
  });
}

Task<std::vector<PendingChangeResult>> PluginManager::ApplyPendingChanges() {
  return RunManagement<std::vector<PendingChangeResult>>(impl_, [](Impl& impl) { return impl.ApplyPending(); });
}

std::vector<std::shared_ptr<Worker>> PluginManager::Impl::ActiveAndRetiredWorkers(std::string_view id) const {
  std::vector<std::shared_ptr<Worker>> result;
  if (const auto active = workers.find(id); active != workers.end()) result.push_back(active->second);
  for (const auto& [digest, retired] : retired_workers) {
    if (retired.id == id) result.push_back(retired.worker);
  }
  return result;
}

Task<void> PluginManager::Impl::RetirePlugin(const std::string& id) {
  if (const auto running = local_plugins.find(id); running != local_plugins.end()) {
    const auto local = running->second;
    BeginLocalShutdown(local);
    co_await FinishLocalShutdown(local);
    local_plugins.erase(id);
  }

  std::vector<std::shared_ptr<Worker>> retiring;
  {
    std::lock_guard lock(mutex);
    if (const auto admission = admissions.find(id); admission != admissions.end()) {
      admission->second.request_stop();
      admissions.erase(admission);
    }
    retiring = ActiveAndRetiredWorkers(id);
    for (const auto& worker : retiring) worker->BeginShutdown();
  }
  if (retiring.empty()) co_return;
  co_await RunTransport([retiring = std::move(retiring)](std::stop_token) {
    for (const auto& worker : retiring) {
      worker->Shutdown();
      worker->WaitForExit();
    }
  });
  std::lock_guard lock(mutex);
  workers.erase(id);
  std::erase_if(retired_workers, [&](const auto& entry) { return entry.second.id == id; });
}

Task<UninstallStatus> PluginManager::Impl::Uninstall(std::string id, std::string expected_digest,
                                                    PluginDataPolicy data) {
  if (data != PluginDataPolicy::Keep && data != PluginDataPolicy::Delete) {
    throw Error(ErrorCode::InvalidArgument, "Invalid plugin data policy");
  }
  std::optional<Change> change;
  {
    std::lock_guard lock(mutex);
    const auto& current = RequireInstalled(id);
    if (current.info.digest != expected_digest) throw Error(ErrorCode::ApprovalMismatch, "The installed digest changed");
    change.emplace(*this, id);
  }
  co_await RetirePlugin(id);
  std::lock_guard lock(mutex);
  (void)RequireInstalled(id);
  auto next = profile_state;
  const auto current = next.plugins.extract(id);
  const auto pending = next.upgrades.extract(id);
  const auto package_path = PackageDirectory(current.mapped().info.digest);
  const auto data_path = DataDirectory(current.mapped().data);
  fs::path pending_package;
  fs::path pending_data;
  if (!pending.empty()) pending_package = PackageDirectory(pending.mapped().target.info.digest);
  if (!pending.empty() && pending.mapped().applying) {
    next.discarded_data.insert(pending.mapped().target.data);
    pending_data = DataDirectory(pending.mapped().target.data);
  }
  if (data == PluginDataPolicy::Keep) next.retained_data.emplace(id, current.mapped().data);
  else next.discarded_data.insert(current.mapped().data);
  Commit(std::move(next));
  CleanupFilesAfterCommit();
  std::error_code error;
  const auto remains = [&](const fs::path& path) {
    const auto status = fs::symlink_status(path, error);
    return status.type() != fs::file_type::not_found;
  };
  const auto cleanup_pending = remains(package_path) || (!pending_package.empty() && remains(pending_package)) ||
      (!pending_data.empty() && remains(pending_data)) || (data == PluginDataPolicy::Delete && remains(data_path));
  co_return cleanup_pending ? UninstallStatus::CleanupPending : UninstallStatus::Removed;
}

Task<UpgradeStatus> PluginManager::Impl::Upgrade(File directory, UpgradeApproval approval) {
  auto source = co_await RunTransport([directory = std::move(directory)](std::stop_token) {
    return ReadPackage(NativePath(directory));
  });
  auto id = source.info.id;
  co_return co_await ApplyUpgrade(std::move(id), std::move(source), std::move(approval));
}

Task<UpgradeStatus> PluginManager::Impl::ApplyUpgrade(std::string id, std::optional<Package> source,
                                                      UpgradeApproval approval) {
  std::optional<Change> change;
  std::optional<Installed> replacement;
  std::string current_data;
  std::string candidate_data;
  {
    std::lock_guard lock(mutex);
    const auto& current = RequireInstalled(id);
    change.emplace(*this, id);
    PruneExitedWorkers();
    if (source) {
      if (current.info.digest != approval.expected_digest || source->info.digest != approval.approved_digest) {
        throw Error(ErrorCode::ApprovalMismatch, "The installed or replacement package digest changed");
      }
      if (current.info.version == source->info.version) {
        throw Error(ErrorCode::InvalidArgument, "An upgrade requires a different plugin release version");
      }
      if (const auto existing = profile_state.upgrades.find(id); existing != profile_state.upgrades.end()) {
        throw Error(ErrorCode::Busy, existing->second.applying
            ? "Reopen the profile to recover the interrupted upgrade"
            : "This plugin already has an approved upgrade");
      }
      InstallOptions options{current.approval.mode, std::move(approval.services)};
      ValidatePackage(source->info, options);
      if (retired_workers.contains(source->info.digest)) {
        throw Error(ErrorCode::Busy, "A previous candidate worker is still retiring");
      }
      source->module_file.reset();
      StorePackage(*source);
      replacement.emplace(Installed{std::move(source->info), std::move(source->manifest), current.enabled,
                                     std::move(options), current.data});
    } else {
      const auto found = profile_state.upgrades.find(id);
      if (found == profile_state.upgrades.end() || found->second.process == HostProcessToken() || found->second.applying) {
        throw Error(ErrorCode::Busy, "This pending upgrade is not ready to apply");
      }
      replacement.emplace(found->second.target);
    }

    current_data = current.data;
    if (source) replacement->data = DefaultDataName(id) + "." + NewToken();
    candidate_data = replacement->data;
    replacement->enabled = current.enabled;
    auto next = profile_state;
    if (source && current.approval.mode == ExecutionMode::InProcess) {
      next.upgrades.emplace(id, PendingUpgrade{std::move(*replacement), current.info.digest, HostProcessToken(), false});
      Commit(std::move(next));
      co_return UpgradeStatus::RestartRequired;
    }

    // Publish the validation record before starting the candidate so a fresh host can recover the old snapshot.
    next.upgrades.insert_or_assign(id, PendingUpgrade{*replacement, current.info.digest, HostProcessToken(), true});
    Commit(std::move(next));
  }

  std::shared_ptr<Worker> worker;
  std::shared_ptr<LocalPlugin> local;
  std::exception_ptr failure;
  try {
    std::vector<Worker::Service> granted;
    {
      std::lock_guard lock(mutex);
      ValidatePackage(replacement->info, replacement->approval);
      if (replacement->approval.mode == ExecutionMode::WorkerProcess) granted = WorkerServices(*replacement);
    }
    co_await RetirePlugin(id);
    const auto prepare = [self = shared_from_this(), target = *replacement, source_data = DataDirectory(current_data),
                          target_data = DataDirectory(candidate_data)](std::stop_token) {
      auto package = self->VerifyPackage(target);
      RequireManagedDirectory(target_data.parent_path());
      fs::create_directories(target_data.parent_path());
      CopyPluginData(source_data, target_data);
      return package;
    };
    auto package = co_await RunTransport(prepare);
    if (replacement->enabled && replacement->approval.mode == ExecutionMode::WorkerProcess) {
      worker = MakeWorker(*replacement, package, std::move(granted));
      co_await RunTransport([worker](std::stop_token stop) { worker->ValidateFactories(stop); });
    } else if (replacement->enabled) {
      local = MakeLocal(*replacement);
      ValidateLocalPlugin(local);
    }

    std::lock_guard lock(mutex);
    (void)RequireInstalled(id);
    auto next = profile_state;
    next.plugins.at(id) = std::move(*replacement);
    next.upgrades.erase(id);
    next.discarded_data.insert(current_data);
    if (worker) workers.emplace(id, worker);
    if (local) local_plugins.emplace(id, local);
    Commit(std::move(next));
    CleanupFilesAfterCommit();
  } catch (const fs::filesystem_error& error) {
    failure = std::make_exception_ptr(Error(ErrorCode::Io, error.what()));
  } catch (...) {
    failure = std::current_exception();
  }
  if (!failure) co_return UpgradeStatus::Applied;

  if (worker) {
    try {
      co_await RunTransport([worker](std::stop_token) {
        worker->Shutdown();
        worker->WaitForExit();
      });
    } catch (...) {
      std::lock_guard lock(mutex);
      const auto& pending = profile_state.upgrades.at(id);
      // Retain files while an unconfirmed worker exit can still leave them open.
      retired_workers.emplace(pending.target.info.digest, RetiredWorker{id, worker, candidate_data});
    }
  }
  if (local) {
    BeginLocalShutdown(local);
    co_await FinishLocalShutdown(local);
    local_plugins.erase(id);
  }
  {
    std::lock_guard lock(mutex);
    if (worker) {
      if (const auto running = workers.find(id); running != workers.end() && running->second == worker) {
        workers.erase(running);
      }
    }
    auto next = profile_state;
    next.upgrades.erase(id);
    next.discarded_data.insert(candidate_data);
    Commit(std::move(next));
    CleanupFilesAfterCommit();
  }
  std::rethrow_exception(failure);
}

Task<std::vector<PendingChangeResult>> PluginManager::Impl::ApplyPending() {
  std::vector<std::string> pending;
  {
    std::lock_guard lock(mutex);
    if (closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
    for (const auto& [id, upgrade] : profile_state.upgrades) {
      if (!upgrade.applying && upgrade.process != HostProcessToken()) pending.push_back(id);
    }
  }

  std::vector<PendingChangeResult> results;
  for (const auto& id : pending) {
    PendingChangeResult result{id, std::nullopt, {}, false};
    try {
      co_await ApplyUpgrade(id, std::nullopt);
    } catch (const Error& error) {
      result.error = error.Code();
      result.message = error.what();
    } catch (const std::exception& error) {
      result.error = ErrorCode::Io;
      result.message = error.what();
    }
    if (result.error) {
      std::lock_guard lock(mutex);
      const auto& installed = profile_state.plugins;
      if (const auto current = installed.find(id); current != installed.end()) {
        result.restart_required = InProcessVersionRequiresRestart(profile, id, current->second.info.digest);
      }
    }
    results.push_back(std::move(result));
  }
  co_return results;
}

Task<void> PluginManager::SetEnabled(std::string_view id, bool enabled) {
  return RunManagement<void>(impl_, [id = std::string(id), enabled](Impl& impl) mutable {
    return impl.SetEnabled(std::move(id), enabled);
  });
}

Task<void> PluginManager::Impl::SetEnabled(std::string id, bool enabled) {
  ExecutionMode mode;
  std::optional<Change> change;
  {
    std::lock_guard lock(mutex);
    mode = RequireInstalled(id).approval.mode;
    change.emplace(*this, id, false);
  }
  if (mode == ExecutionMode::InProcess) {
    co_await ChangeLocalEnabled(id, enabled);
  } else {
    co_await RunTransport([self = shared_from_this(), id, enabled](std::stop_token stop) {
      self->ChangeWorkerEnabled(id, enabled, stop);
    });
  }
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
  worker->WaitForExit();
  std::lock_guard lock(mutex);
  const auto running = workers.find(id);
  if (running != workers.end() && running->second == worker) workers.erase(running);
}

std::vector<PluginInfo> PluginManager::ListPlugins() const {
  // Read the snapshot without entering the worker's request queue, including during a host callback.
  std::lock_guard lock(impl_->mutex);
  std::vector<PluginInfo> result;
  for (const auto& [id, plugin] : impl_->profile_state.plugins) {
    const auto worker = impl_->workers.find(id);
    const auto upgrade = impl_->profile_state.upgrades.find(id);
    result.push_back({plugin.info, plugin.enabled, plugin.approval.mode,
                      worker == impl_->workers.end() ? std::nullopt : worker->second->Pid(),
                      upgrade == impl_->profile_state.upgrades.end()
                          ? std::nullopt : std::optional(upgrade->second.target.info)});
  }
  return result;
}

std::vector<ExtensionInfo> PluginManager::List(std::string_view point, ContractInfo contract,
                                               std::optional<std::string_view> plugin_id) const {
  std::lock_guard lock(impl_->mutex);
  impl_->RequirePoint(point, contract);
  std::vector<ExtensionInfo> result;
  for (const auto& [id, plugin] : impl_->profile_state.plugins) {
    if ((plugin_id && id != *plugin_id) || !plugin.enabled || impl_->IsPluginBusy(id, plugin)) continue;
    std::uint32_t ordinal = 0;
    for (const auto& extension : plugin.info.extensions) {
      if (extension.point != point) continue;
      if (ProvidedContract(plugin.info, extension.contract).Info() == contract) {
        result.push_back(ExtensionInfo(id, extension, plugin.info.digest, ordinal));
      }
      ++ordinal;
    }
  }
  return result;
}

std::shared_ptr<ExtensionTarget> PluginManager::Create(std::string_view point, ContractInfo contract,
    std::string_view plugin_id, const ExtensionInfo* candidate, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24)) {
    throw Error(ErrorCode::InvalidArgument, "Extension timeout must be between 1 ms and 24 hours");
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->closed) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  const auto& registered = impl_->RequirePoint(point, contract);
  const auto& plugins = impl_->profile_state.plugins;
  const auto plugin = plugins.find(plugin_id);
  if (plugin == plugins.end()) throw Error(ErrorCode::NotFound, "The plugin is not installed");
  const auto& installed = plugin->second;
  // An ordinal only identifies the same factory within one approved package snapshot.
  if (candidate && candidate->digest_ != installed.info.digest) {
    throw Error(ErrorCode::PackageChanged, "Query the extension candidates again after a package change");
  }
  if (!installed.enabled) throw Error(ErrorCode::Disabled, "The plugin is disabled");
  if (impl_->IsPluginBusy(plugin_id, installed)) {
    throw Error(ErrorCode::Busy, "The plugin has a pending version change, restart, or uninstall");
  }
  const auto& extensions = installed.info.extensions;
  if (candidate && candidate->point_ != point) {
    throw Error(ErrorCode::NotFound, "The candidate belongs to a different extension point");
  }
  bool selected = false;
  std::uint32_t ordinal = 0;
  for (const auto& extension : extensions) {
    if (extension.point != point) continue;
    if (candidate && ordinal != candidate->ordinal_) {
      ++ordinal;
      continue;
    }
    if (selected) throw Error(ErrorCode::Ambiguous, "The plugin provides multiple factories at this extension point");
    if (ProvidedContract(installed.info, extension.contract).Info() != contract) {
      throw Error(ErrorCode::ContractMismatch, "The installed contract differs from the requested contract");
    }
    selected = true;
    if (candidate) break;
  }
  if (!selected) throw Error(ErrorCode::NotFound, "The plugin has no such factory at this extension point");

  if (installed.approval.mode == ExecutionMode::WorkerProcess) {
    if (!registered.remote || !impl_->worker_executable) {
      throw Error(ErrorCode::UnsupportedMode, "The extension cannot use a worker");
    }
    const auto admission = impl_->admissions.try_emplace(plugin->first).first->second.get_token();
    return MakeWorkerExtension([impl = impl_, id = plugin->first, admission](Deadline deadline, std::stop_token stop) {
      return impl->StartWorker(id, admission, deadline, stop);
    }, timeout, admission, ExtensionPayload(point, ordinal));
  }

  const auto running = impl_->local_plugins.find(plugin_id);
  auto local = running == impl_->local_plugins.end() ? impl_->MakeLocal(installed) : running->second;
  if (running == impl_->local_plugins.end()) impl_->local_plugins.emplace(plugin->first, local);
  return MakeLocalExtension(std::move(local), std::string(point), ordinal, timeout);
}

std::shared_ptr<LocalPlugin> PluginManager::Impl::MakeLocal(const Installed& installed) {
  return CreateLocalPlugin(callbacks, [manager = weak_from_this(), installed]() -> const hpk_plugin_v1& {
    const auto impl = manager.lock();
    if (!impl) throw Error(ErrorCode::Closed, "The plugin manager is closed");
    if (installed.info.cpp_abi != CppAbi) {
      throw Error(ErrorCode::ContractMismatch, "The package requires a different in-process SDK or C++ ABI");
    }
    auto package = impl->VerifyPackage(installed);
    const auto module = impl->PackageDirectory(package.info.digest) / ModuleName;
    const auto* binding = &LoadInProcessModule(module, std::move(package.module_file), impl->profile,
                                               installed.info.id, installed.info.digest);
    const auto* runtime = RuntimeIdentity();
    if (!runtime || !binding->cpp_abi || installed.info.cpp_abi != binding->cpp_abi || !binding->runtime_identity ||
        binding->runtime_identity() != runtime) {
      throw Error(ErrorCode::ContractMismatch, "The plugin does not share the host's C++ ABI and HuxerUI Runtime");
    }
    return *binding;
  }, Startup(installed), ApprovedServices(installed));
}

Package PluginManager::Impl::VerifyPackage(const Installed& installed) const {
  try {
    const auto directory = PackageDirectory(installed.info.digest);
    RequireManagedDirectory(directory.parent_path());
    RequireManagedDirectory(directory);
    auto package = ReadPackage(directory);
    if (package.info.digest == installed.info.digest) return package;
  } catch (const Error& error) {
    throw Error(ErrorCode::PackageChanged, "Cannot verify the installed package: " + std::string(error.what()));
  }
  throw Error(ErrorCode::PackageChanged, "The installed package differs from the approved package");
}

PluginStartup PluginManager::Impl::Startup(const Installed& installed) const {
  const auto& info = installed.info;
  const auto directory = PackageDirectory(info.digest);
  const auto data = DataDirectory(installed.data);
  PluginStartup startup{info.id, info.version, info.digest, PathText(directory), PathText(data), {}};
  startup.extensions.reserve(info.extensions.size());
  for (const auto& extension : info.extensions) {
    startup.extensions.push_back({extension.point, ProvidedContract(info, extension.contract)});
  }
  return startup;
}

std::vector<Worker::Service> PluginManager::Impl::WorkerServices(const Installed& installed) const {
  std::vector<Worker::Service> granted;
  for (auto& service : ApprovedServices(installed)) {
    if (service.dispatch) granted.push_back({std::move(service.id), service.version, std::move(service.fingerprint),
                                             std::move(service.dispatch)});
  }
  return granted;
}

std::shared_ptr<Worker> PluginManager::Impl::MakeWorker(const Installed& installed, const Package& package,
                                                        std::vector<Worker::Service> granted) {
  const auto& info = installed.info;
  return std::make_shared<Worker>(*worker_executable, PackageDirectory(info.digest) / ModuleName, package.module_file,
      std::move(granted), callbacks, Startup(installed));
}

std::shared_ptr<Worker> PluginManager::Impl::StartWorker(const std::string& id, const std::stop_token& admission,
                                                        Deadline deadline, std::stop_token stop) {
  std::unique_lock lock(mutex);
  if (closed || admission.stop_requested()) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
  const auto installed = profile_state.plugins.find(id);
  if (installed == profile_state.plugins.end() || !installed->second.enabled) {
    throw Error(ErrorCode::Closed, "The plugin is disabled");
  }

  if (const auto running = workers.find(id); running != workers.end()) {
    if (running->second->Available() && running->second->Pid()) return running->second;
    running->second->Stop();
    running->second->WaitForExit();
    workers.erase(running);
  }

  const auto package = VerifyPackage(installed->second);
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
  if (std::chrono::steady_clock::now() >= deadline) {
    throw Error(ErrorCode::DeadlineExceeded, "The plugin call exceeded its deadline before starting the worker");
  }
  auto worker = MakeWorker(installed->second, package, WorkerServices(installed->second));
  workers.emplace(id, worker);
  return worker;
}

} // namespace huxerui::pluginkit
