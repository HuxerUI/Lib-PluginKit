#pragma once

#include "platform_files.h"
#include "transport.h"

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

inline const hpk_plugin_v1& LoadPluginModule(const std::filesystem::path& path, ContractInfo contract,
                                            std::uint32_t mode) {
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
      !binding->contract_id || !binding->contract_fingerprint || contract.id != binding->contract_id ||
      contract.version != binding->contract_version || contract.fingerprint != binding->contract_fingerprint ||
      !binding->activate || !binding->create || !binding->close || !binding->deactivate) {
    throw Error(ErrorCode::ContractMismatch, "The module descriptor does not match the installed contract");
  }
  if (!(binding->execution_modes & mode) || (mode == HPK_WORKER_PROCESS && !binding->invoke) ||
      (mode == HPK_IN_PROCESS && (!binding->set_invocation || !binding->attach_invocation || !binding->detach_invocation))) {
    throw Error(ErrorCode::UnsupportedMode, "The module does not support the approved execution mode");
  }
  return *binding;
}

inline const hpk_plugin_v1& LoadInProcessModule(const std::filesystem::path& path, [[maybe_unused]] Handle verified,
                                               ContractInfo contract) {
  // A mapped module can own deferred SDK work. Cache its descriptor for the lifetime of this process.
  struct Module {
    const hpk_plugin_v1* binding = nullptr;
#if defined(__linux__)
    Handle file;
#endif
  };
  static std::map<std::filesystem::path, std::shared_ptr<Module>> modules;
  static std::mutex mutex;
  std::shared_ptr<Module> entry;
  {
    std::lock_guard lock(mutex);
    auto& cached = modules[path];
    if (cached && cached->binding) return *cached->binding;
    if (!cached) {
      cached = std::make_shared<Module>();
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
  const auto& binding = LoadPluginModule(load_path, contract, HPK_IN_PROCESS);
  {
    std::lock_guard lock(mutex);
    entry->binding = &binding;
  }
  return binding;
}

inline void* ActivatePlugin(const hpk_plugin_v1& binding, const PluginStartup& startup) {
  const hpk_plugin_context_v1 context{startup.id.c_str(), startup.version.c_str(), startup.digest.c_str(),
                                     startup.bundle_directory.c_str(), startup.data_directory.c_str()};
  void* activation = nullptr;
  CheckPluginStatus(binding.activate(&context, &activation));
  if (!activation) throw Error(ErrorCode::Protocol, "The plugin returned an empty activation");
  return activation;
}

} // namespace huxerui::pluginkit::detail
