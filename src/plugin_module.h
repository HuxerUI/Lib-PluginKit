#pragma once

#include "platform_files.h"
#include "transport.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace huxerui::pluginkit::detail {

inline void CheckPluginStatus(std::int32_t status) {
  switch (status) {
    case HPK_OK: return;
    case HPK_INVALID_REQUEST: throw Error(ErrorCode::Protocol, "The plugin rejected the request payload");
    case HPK_PAYLOAD_TOO_LARGE: throw Error(ErrorCode::PayloadTooLarge, "The plugin response exceeds the payload limit");
    case HPK_PLUGIN_FAILED: throw Error(ErrorCode::PluginFailed, "The plugin implementation failed");
    case HPK_SERVICE_DENIED: throw Error(ErrorCode::ServiceDenied, "The package cannot call this host service");
    case HPK_SERVICE_UNAVAILABLE: throw Error(ErrorCode::ServiceUnavailable, "The host service is unavailable");
    case HPK_REENTRANT_CALL: throw Error(ErrorCode::ReentrantCall, "A host callback attempted to reenter PluginKit");
    case HPK_RECEIVER_CLOSED: throw Error(ErrorCode::ReceiverClosed, "The receiver is outside its owning invocation");
    case HPK_CONTRACT_MISMATCH: throw Error(ErrorCode::ContractMismatch, "The callback contract does not match");
    case HPK_CLOSED: throw Error(ErrorCode::Closed, "The plugin generation is closing");
    case HPK_CALLBACK_UNAVAILABLE: throw Error(ErrorCode::CallbackUnavailable, "The call cannot use a host callback");
    case HPK_BUSY: throw Error(ErrorCode::Busy, "The plugin has an active invocation");
    default: throw Error(ErrorCode::Protocol, "The plugin returned an invalid status");
  }
}

inline const hpk_extension_v1* FindExtension(const hpk_plugin_v1& plugin, std::string_view point,
                                             std::uint32_t ordinal) noexcept {
  for (const auto& entry : std::span(plugin.extensions, plugin.extension_count)) {
    if (point != std::string_view(entry.point, entry.point_size)) continue;
    if (ordinal == 0) return &entry;
    --ordinal;
  }
  return nullptr;
}

inline void ValidatePluginDescriptor(const hpk_plugin_v1& binding, std::span<const ExpectedExtension> expected,
                                     std::uint32_t mode) {
  if (!(binding.execution_modes & mode) ||
      (mode == HPK_IN_PROCESS && (!binding.set_invocation || !binding.attach_invocation || !binding.detach_invocation))) {
    throw Error(ErrorCode::UnsupportedMode, "The module does not support the approved execution mode");
  }
  if (binding.extension_count != expected.size()) {
    throw Error(ErrorCode::ContractMismatch, "The module factory count differs from the approved package");
  }
  // Each declared slot must select a distinct export, including when the worker decodes the slots from IPC.
  std::array<bool, HPK_MAX_EXTENSIONS> matched{};
  for (const auto& extension : expected) {
    const auto* found = FindExtension(binding, extension.point, extension.ordinal);
    if (!found || extension.Info() !=
        ContractInfo{found->contract_id, found->contract_version, found->contract_fingerprint}) {
      throw Error(ErrorCode::ContractMismatch, "The module factory does not match the host extension point " + extension.point);
    }
    if (std::exchange(matched[found - binding.extensions], true)) {
      throw Error(ErrorCode::ContractMismatch, "Several declarations select the same module factory");
    }
    if (mode == HPK_WORKER_PROCESS && !found->invoke) {
      throw Error(ErrorCode::UnsupportedMode, "The module has a factory without worker dispatch");
    }
  }
}

inline const hpk_plugin_v1& LoadPluginModule(const std::filesystem::path& path) {
  // Keep modules mapped for SDK operations and C++ objects that can outlive a particular activation.
#if defined(_WIN32)
  const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  if (!module) WindowsError(ErrorCode::InvalidPackage, "Cannot load plugin.dll");
  const auto address = GetProcAddress(module, "pluginkit_query_v1");
#else
  int flags = RTLD_NOW | RTLD_LOCAL;
#if defined(__APPLE__)
  flags |= RTLD_FIRST;
#endif
  const auto module = ::dlopen(path.c_str(), flags);
  if (!module) {
    auto message = std::string("Cannot load ") + ModuleName;
    if (const auto* error = ::dlerror(); error && IsUtf8(error)) message += ": " + std::string(error);
    throw Error(ErrorCode::InvalidPackage, message);
  }
  const auto address = ::dlsym(module, "pluginkit_query_v1");
#endif
  if (!address) throw Error(ErrorCode::ContractMismatch, "Missing pluginkit_query_v1 export");
  const auto* binding = reinterpret_cast<hpk_query_v1>(address)();
  if (!binding || binding->struct_size != sizeof(hpk_plugin_v1) || binding->abi_version != HPK_ABI_VERSION ||
      !binding->extensions || !binding->extension_count || binding->extension_count > HPK_MAX_EXTENSIONS ||
      !binding->activate || !binding->deactivate) {
    throw Error(ErrorCode::ContractMismatch, "The module has an invalid plugin descriptor");
  }
  for (const auto& entry : std::span(binding->extensions, binding->extension_count)) {
    if (!entry.point || !entry.point_size || !entry.contract_id || !entry.contract_version || !entry.contract_fingerprint ||
        !entry.create || !entry.close) {
      throw Error(ErrorCode::ContractMismatch, "The module has an invalid extension factory");
    }
  }
  return *binding;
}

struct InProcessModules {
  struct Module {
    const hpk_plugin_v1* binding = nullptr;
#if defined(__linux__)
    Handle file;
#endif
  };

  std::map<std::filesystem::path, std::shared_ptr<Module>> modules;
  std::map<std::filesystem::path, std::string> selected_digests;
  std::mutex mutex;
};

inline InProcessModules& LoadedModules() {
  static InProcessModules modules;
  return modules;
}

inline bool IsInProcessModuleMapped(const std::filesystem::path& path) {
  auto& state = LoadedModules();
  std::lock_guard lock(state.mutex);
  return state.modules.contains(path);
}

inline bool InProcessVersionRequiresRestart(const std::filesystem::path& profile, std::string_view id,
                                            std::string_view digest) {
  auto& state = LoadedModules();
  std::lock_guard lock(state.mutex);
  const auto found = state.selected_digests.find(profile / std::string(id));
  return found != state.selected_digests.end() && found->second != digest;
}

inline const hpk_plugin_v1& LoadInProcessModule(const std::filesystem::path& path, [[maybe_unused]] Handle verified,
                                               const std::filesystem::path& profile,
                                               std::string_view id, std::string_view digest) {
  // A mapped module can own deferred SDK work. Cache its descriptor for the lifetime of this process.
  auto& state = LoadedModules();
  std::shared_ptr<InProcessModules::Module> entry;
  {
    std::lock_guard lock(state.mutex);
    const auto [selected, inserted] = state.selected_digests.try_emplace(profile / std::string(id), digest);
    if (!inserted && selected->second != digest) {
      throw Error(ErrorCode::Busy, "Restart the host before loading a different in-process plugin version");
    }
    auto& cached = state.modules[path];
    if (cached && cached->binding) return *cached->binding;
    if (!cached) {
      cached = std::make_shared<InProcessModules::Module>();
#if defined(__linux__)
      // Keep the descriptor open so the loader's /proc path cannot be reused for another module.
      cached->file = std::move(verified);
#endif
    }
    entry = cached;
  }
#if defined(__linux__)
  const auto load_path = std::filesystem::path("/proc/self/fd") / std::to_string(entry->file.get());
#else
  const auto& load_path = path;
#endif
  const auto& binding = LoadPluginModule(load_path);
  {
    std::lock_guard lock(state.mutex);
    entry->binding = &binding;
  }
  return binding;
}

inline void* ActivatePlugin(const hpk_plugin_v1& binding, const PluginStartup& startup, std::uint32_t mode) {
  ValidatePluginDescriptor(binding, startup.extensions, mode);
  const auto& data = startup.data_directory;
  const auto path = std::filesystem::path(std::u8string(data.begin(), data.end()));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) throw Error(ErrorCode::Io, "Cannot create the plugin data directory: " + error.message());

  const auto& identity = startup.identity;
  const hpk_plugin_context_v1 context{identity.id.c_str(), identity.version.c_str(), identity.digest.c_str(),
                                     startup.bundle_directory.c_str(), startup.data_directory.c_str()};
  void* activation = nullptr;
  CheckPluginStatus(binding.activate(&context, &activation));
  if (!activation) throw Error(ErrorCode::Protocol, "The plugin returned an empty activation");
  return activation;
}

} // namespace huxerui::pluginkit::detail
