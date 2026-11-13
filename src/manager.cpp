#include "digest.h"
#include "worker_process.h"

#include <nlohmann/json.hpp>

#include <map>

namespace pluginkit {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace detail;

constexpr std::size_t MaxManifest = 64 * 1024;
constexpr std::size_t MaxModule = 64 * 1024 * 1024;
constexpr std::size_t MaxProfile = 1024 * 1024;
constexpr int MaxManifestDepth = 16;

Json ParseJson(ByteView bytes, int max_depth, ErrorCode error) {
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
  Impl(const fs::path& directory, const fs::path& executable)
      : profile(fs::absolute(directory)), worker_executable(fs::absolute(executable)) {
    fs::create_directories(profile);
    ownership = LockProfile(profile);
    catalog = ReadCatalog(profile / "profile.json");
  }

  ~Impl() {
    for (const auto& [id, worker] : workers) worker->Stop();
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
  Handle ownership;
  mutable std::mutex mutex;
  Catalog catalog;
  std::map<std::string, RegisteredContract, std::less<>> points;
  std::map<std::string, std::shared_ptr<Worker>, std::less<>> workers;
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
  }
  return "unknown";
}

PluginManager::PluginManager(const fs::path& profile, const fs::path& executable)
    : impl_(std::make_unique<Impl>(profile, executable)) {}

PluginManager::~PluginManager() = default;

void PluginManager::Register(std::string_view point, ContractInfo contract) {
  if (!IsId(point) || !IsId(contract.id) || !contract.version || !IsDigest(contract.fingerprint)) {
    throw Error(ErrorCode::InvalidArgument, "Invalid extension point or contract identity");
  }
  std::lock_guard lock(impl_->mutex);
  if (!impl_->points.try_emplace(std::string(point), std::string(contract.id), contract.version,
                                 std::string(contract.fingerprint)).second) {
    throw Error(ErrorCode::InvalidArgument, "The extension point is already registered");
  }
}

PackageInfo PluginManager::InspectPackage(const fs::path& directory) { return ReadPackage(directory).info; }

void PluginManager::InstallPackage(const fs::path& directory, std::string_view approved_digest) {
  auto package = ReadPackage(directory);
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
      running->second->Stop();
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

void PluginManager::SetEnabled(std::string_view id, bool enabled) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->catalog.find(id);
  if (found == impl_->catalog.end()) throw Error(ErrorCode::NotFound, "The plugin is not installed");
  if (found->second.enabled == enabled) return;
  auto candidate = impl_->catalog;
  candidate.find(id)->second.enabled = enabled;
  impl_->Commit(std::move(candidate));
  if (!enabled) {
    if (const auto running = impl_->workers.find(id); running != impl_->workers.end()) {
      running->second->Stop();
      impl_->workers.erase(running);
    }
  }
}

std::vector<PluginInfo> PluginManager::ListPlugins() const {
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

ExtensionInstance PluginManager::Open(std::string_view point, ContractInfo contract, const ExtensionId& id,
                                      std::chrono::milliseconds timeout) {
  std::lock_guard lock(impl_->mutex);
  impl_->RequirePoint(point, contract);
  const auto plugin = impl_->catalog.find(id.plugin);
  if (plugin == impl_->catalog.end() || plugin->second.info.point != point ||
      plugin->second.info.contribution != id.contribution) {
    throw Error(ErrorCode::NotFound, "The extension is not installed at this point");
  }
  const auto& installed = plugin->second;
  if (!installed.enabled) throw Error(ErrorCode::Disabled, "The plugin is disabled");
  if (!Matches(installed.info, contract)) throw Error(ErrorCode::ContractMismatch, "The installed contract differs");

  const auto running = impl_->workers.find(id.plugin);
  if (running != impl_->workers.end()) {
    if (running->second->Pid()) {
      const auto reply = running->second->Request({static_cast<std::uint32_t>(Command::Open), 0, 0, {}}, timeout);
      return ExtensionInstance(running->second, reply.instance);
    }
    running->second->Stop();
    impl_->workers.erase(running);
  }

  const auto directory = impl_->PackageDirectory(installed.info.digest);
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
  // The open file lets the Linux worker load the same library that was verified above.
  auto worker = std::make_shared<Worker>(impl_->worker_executable, directory / ModuleName, package.module_file, contract);
  const auto reply = worker->Request({static_cast<std::uint32_t>(Command::Open), 0, 0, {}}, timeout);
  // Publish a generation only after its first instance opens successfully.
  impl_->workers.emplace(id.plugin, worker);
  return ExtensionInstance(std::move(worker), reply.instance);
}

} // namespace pluginkit
