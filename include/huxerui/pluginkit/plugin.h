#pragma once

/// @file
/// @brief Plugin context, host services, and export bindings for plugin implementations.
///
/// Include the generated bindings for the implemented HPK_REMOTE interface and export its implementation
/// with HPK_EXPORT_PLUGIN. In-process plugins use the host's HuxerUI application. Worker plugins run
/// in a separate windowless HuxerUI application.

#include <huxerui/file.h>
#include <huxerui/app.h>
#include <huxerui/pluginkit/detail/extension.h>

#include <concepts>
#include <memory>
#include <string>
#include <utility>

namespace huxerui::pluginkit {

namespace detail {
struct PluginContextAccess;
}

/// @brief Identity of the installed package selected by the host.
struct PluginIdentity {
  /// Plugin identifier from the manifest.
  std::string id;
  /// Plugin release version from the manifest.
  std::string version;
  /// SHA-256 digest of the approved package snapshot.
  std::string digest;
};

/// @brief Read-only context shared by every extension instance in one plugin activation.
///
/// The binding passes the same object to the optional activation and deactivation hooks and to constructors
/// that accept it. The object stays valid through instance destruction and the deactivation hook. Copy
/// individual values if they must outlive the activation. See GetService() for access to host services
/// during a managed invocation.
class PluginContext {
public:
  /// @brief Context identity is shared by reference within its activation.
  PluginContext(const PluginContext&) = delete;
  /// @brief A context retains the identity and paths of its activation.
  PluginContext& operator=(const PluginContext&) = delete;

  /// @brief Returns the identity of the approved plugin package.
  /// @return Reference valid for the lifetime of this context.
  const PluginIdentity& Info() const noexcept { return info_; }

  /// @brief Returns the absolute directory of the installed package snapshot.
  /// @return Reference to the directory, valid for the lifetime of this context. Treat its contents as read-only.
  const File& BundleDirectory() const noexcept { return bundle_; }

  /// @brief Returns the absolute writable directory reserved for this plugin in the host profile.
  /// PluginKit reuses this directory until an upgrade commits a replacement. During upgrade validation,
  /// OnActivate() receives a copy in which it can migrate the plugin's data format.
  /// @return Reference valid for the lifetime of this context.
  const File& DataDirectory() const noexcept { return data_; }

private:
  friend struct detail::PluginContextAccess;
  PluginContext(PluginIdentity info, File bundle, File data)
      : info_(std::move(info)), bundle_(std::move(bundle)), data_(std::move(data)) {}

  PluginIdentity info_;
  File bundle_;
  File data_;
};

/// @brief Synchronously obtains a typed handle for a host service.
///
/// Call service methods through operator->(). The binding resolves the current managed invocation and checks
/// the package's grant when the method runs. Await Task methods from that invocation or an awaited child Task.
/// Synchronous service methods require a synchronous managed in-process invocation.
/// @tparam Interface Host service interface with a Contract specialization. Worker calls require HPK_REMOTE bindings.
/// @return Typed handle that resolves the current invocation on use.
/// @see PluginManager::ProvideService(), InstallOptions
template<class Interface>
ServiceHandle<Interface> GetService() {
  return {};
}

/// @brief Reads the stop token for the current managed plugin invocation.
///
/// Await from the plugin Task or an awaited child Task. In-process cancellation, an active call's deadline,
/// or shutdown can request cooperative stop. A worker call has an empty token and the host can terminate
/// its worker on cancellation.
/// @return Task yielding the invocation's stop token.
/// @throws Error With ServiceUnavailable if the Task runs outside a managed plugin invocation.
/// @see RunInvocationWorker()
inline Task<std::stop_token> GetInvocationStopToken() {
  co_return detail::FindInvocation(co_await detail::CurrentTaskExecution{}).stop;
}

/// @brief Runs blocking work tied to a managed plugin Task.
///
/// Use this when a background callable borrows the in-process instance or its PluginContext. PluginKit
/// retains them until the callable finishes, even if the awaiting Task is cancelled. The callable should
/// check the stop token and exit promptly so teardown can finish. In a worker process the token is empty.
/// Keep host service and receiver calls on the application Task.
/// @tparam Work Callable accepting a leading std::stop_token followed by the supplied arguments.
/// @tparam Args Types of the arguments passed to the callable after the token.
/// @param work Callable owned by the returned Task.
/// @param args Arguments copied or moved into the returned Task.
/// @return Lazy Task yielding the callable's result.
/// @throws Error With ServiceUnavailable if run outside an invocation, or Closed if it is closing.
template<class Work, class... Args>
auto RunInvocationWorker(Work&& work, Args&&... args) {
  return detail::RunPluginWorker(std::forward<Work>(work), std::forward<Args>(args)...);
}

namespace detail {

// Host and plugins consume the same published build identifier for the shared SDK.
#ifndef HPK_SDK_ABI
#define HPK_SDK_ABI ""
#endif
#define HPK_STRINGIFY_VALUE(value) #value
#define HPK_STRINGIFY(value) HPK_STRINGIFY_VALUE(value)

inline constexpr char CppAbi[] = HPK_SDK_ABI
#if defined(_MSC_VER)
    ":msvc=" HPK_STRINGIFY(_MSVC_STL_VERSION) ":iterators=" HPK_STRINGIFY(_ITERATOR_DEBUG_LEVEL)
#if defined(_DLL)
    ":dynamic-crt"
#else
    ":static-crt"
#endif
#if defined(_DEBUG)
    ":debug"
#endif
#elif defined(_LIBCPP_VERSION)
    ":libc++=" HPK_STRINGIFY(_LIBCPP_ABI_VERSION)
#elif defined(__GLIBCXX__)
    ":libstdc++=" HPK_STRINGIFY(_GLIBCXX_USE_CXX11_ABI)
#if defined(_GLIBCXX_DEBUG)
    ":debug"
#endif
#endif
    ;

inline const void* RuntimeIdentity() noexcept {
  try {
    return &huxerui::detail::CurrentApplication();
  } catch (...) {
    return nullptr;
  }
}

#undef HPK_STRINGIFY
#undef HPK_STRINGIFY_VALUE

struct PluginContextAccess {
  static std::unique_ptr<PluginContext> Create(const hpk_plugin_context_v1& context) {
    return std::unique_ptr<PluginContext>(new PluginContext(
        {context.id, context.version, context.digest},
        File(context.bundle_directory), File(context.data_directory)));
  }
};

template<class Interface>
Task<void> DispatchPlugin(Interface& instance, const hpk_host_v1* host, std::uint64_t invocation,
                          std::uint32_t receiver, std::uint32_t operation, Bytes request,
                          hpk_complete_v1 complete, void* user, std::int32_t (*status)(const Error&) noexcept) {
  try {
    Bytes bytes;
    {
      const auto execution = co_await CurrentTaskExecution{};
      DispatchContext context{{host, invocation, execution}, receiver};
      const Invocation call{execution, HostEndpoint(context.invocation), nullptr, nullptr, {}};
      InvocationScope scope(call);
      bytes = co_await Contract<Interface>::Dispatch(instance, operation, std::move(request), context);
    }
    complete(user, HPK_OK, {reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            static_cast<std::uint32_t>(bytes.size())});
  } catch (const Error& error) {
    complete(user, static_cast<std::uint32_t>(status(error)), {});
  } catch (...) {
    complete(user, HPK_PLUGIN_FAILED, {});
  }
}

} // namespace detail

/// @brief Builds the ABI descriptor for a published contract and its implementation.
///
/// The descriptor creates and destroys instances inside the library. HPK_REMOTE bindings also publish
/// worker dispatch and lend encoded result buffers to completion callbacks. Both modes create a single
/// PluginContext for each activation. If both constructors are available, the context overload is used.
/// @tparam Interface Interface with a public virtual destructor and a Contract specialization.
/// @tparam Implementation Public implementation constructible from const PluginContext& or by default.
/// @tparam OnActivate Optional initialization hook called before creating any instances. An exception fails activation.
/// @tparam OnDeactivate Optional cleanup hook called during orderly teardown after all instances are destroyed.
/// @note Hooks run on the loading process's application thread. A terminated worker may skip teardown.
/// @return Reference to a descriptor that remains valid while the plugin library is loaded.
template<class Interface, class Implementation,
         void (*OnActivate)(const PluginContext&) = nullptr, void (*OnDeactivate)(const PluginContext&) = nullptr>
  requires std::derived_from<Implementation, Interface> &&
           std::has_virtual_destructor_v<Interface> &&
           (std::constructible_from<Implementation, const PluginContext&> || std::default_initializable<Implementation>)
const hpk_plugin_v1& Bind() noexcept {
  static constexpr auto status = [](const Error& error) noexcept -> std::int32_t {
    switch (error.Code()) {
      case ErrorCode::PayloadTooLarge: return HPK_PAYLOAD_TOO_LARGE;
      case ErrorCode::Protocol: return HPK_INVALID_REQUEST;
      case ErrorCode::ServiceDenied: return HPK_SERVICE_DENIED;
      case ErrorCode::ServiceUnavailable: return HPK_SERVICE_UNAVAILABLE;
      case ErrorCode::ReentrantCall: return HPK_REENTRANT_CALL;
      case ErrorCode::ReceiverClosed: return HPK_RECEIVER_CLOSED;
      case ErrorCode::ContractMismatch: return HPK_CONTRACT_MISMATCH;
      case ErrorCode::Closed: return HPK_CLOSED;
      case ErrorCode::CallbackUnavailable: return HPK_CALLBACK_UNAVAILABLE;
      case ErrorCode::Busy: return HPK_BUSY;
      default: return HPK_PLUGIN_FAILED;
    }
  };
  constexpr bool remote = requires(Interface& instance, Bytes request, detail::DispatchContext context) {
    Contract<Interface>::Dispatch(instance, std::uint32_t{}, std::move(request), context);
  };
  static const hpk_plugin_v1 binding{
      sizeof(hpk_plugin_v1), HPK_ABI_VERSION, Contract<Interface>::Info.id.data(),
      Contract<Interface>::Info.version, Contract<Interface>::Info.fingerprint.data(),
      HPK_IN_PROCESS | (remote ? HPK_WORKER_PROCESS : 0u), detail::CppAbi, detail::RuntimeIdentity,
      [](const hpk_plugin_context_v1* input, void** activation) noexcept -> std::int32_t {
        *activation = nullptr;
        try {
          auto context = detail::PluginContextAccess::Create(*input);
          if constexpr (OnActivate != nullptr) OnActivate(*context);
          // Publish the context only after the activation hook succeeds.
          *activation = context.release();
          return HPK_OK;
        } catch (const Error& error) {
          return status(error);
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      []([[maybe_unused]] const void* activation, void** instance) noexcept -> std::int32_t {
        *instance = nullptr;
        try {
          if constexpr (std::constructible_from<Implementation, const PluginContext&>) {
            *instance = static_cast<Interface*>(new Implementation(*static_cast<const PluginContext*>(activation)));
          } else {
            *instance = static_cast<Interface*>(new Implementation);
          }
          return HPK_OK;
        } catch (const Error& error) {
          return status(error);
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      remote ? +[](void* instance, const hpk_host_v1* host, std::uint64_t invocation,
         std::uint32_t receiver, std::uint32_t operation, const std::uint8_t* data, std::uint32_t size,
         hpk_complete_v1 complete, void* user) noexcept -> std::int32_t {
        if constexpr (remote) {
          try {
            if (!instance || !host || !invocation || !complete || (size && !data)) return HPK_INVALID_REQUEST;
            if (size > MaxPayload) return HPK_PAYLOAD_TOO_LARGE;
            Bytes request;
            if (size) {
              const auto bytes = std::as_bytes(std::span(data, size));
              request.assign(bytes.begin(), bytes.end());
            }
            (void)UseApplicationTaskScope().Launch(detail::DispatchPlugin<Interface>(
                *static_cast<Interface*>(instance), host, invocation, receiver, operation, std::move(request),
                complete, user, status));
            return HPK_OK;
          } catch (const Error& error) {
            return status(error);
          } catch (...) {
            return HPK_PLUGIN_FAILED;
          }
        }
        return HPK_INVALID_REQUEST;
      } : nullptr,
      [](const void* context) noexcept -> const void* {
        return std::exchange(detail::synchronous_invocation, static_cast<const detail::Invocation*>(context));
      },
      [](const void* context) -> void* {
        return new detail::InvocationScope(*static_cast<const detail::Invocation*>(context));
      },
      [](void* registration) noexcept { delete static_cast<detail::InvocationScope*>(registration); },
      [](void* instance) noexcept { delete static_cast<Interface*>(instance); },
      [](void* activation) noexcept -> std::int32_t {
        const std::unique_ptr<PluginContext> context(static_cast<PluginContext*>(activation));
        try {
          if constexpr (OnDeactivate != nullptr) OnDeactivate(*context);
          return HPK_OK;
        } catch (const Error& error) {
          return status(error);
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
  };
  return binding;
}

} // namespace huxerui::pluginkit

/// @brief Makes the plugin query symbol visible in the platform's dynamic library.
#if defined(_WIN32)
#define HPK_EXPORT __declspec(dllexport)
#else
#define HPK_EXPORT __attribute__((visibility("default")))
#endif

/// @brief Defines pluginkit_query_v1. Use once at global scope in the plugin library.
/// @param ... Published interface and implementation type, followed by optional activation and deactivation
/// function pointers. Each hook takes const PluginContext& and returns void.
/// @see Bind()
/// @code{.cpp}
/// HPK_EXPORT_PLUGIN(csv_import::TableImporter, csv_import::CsvImporter)
/// @endcode
#define HPK_EXPORT_PLUGIN(...)                                                                  \
  extern "C" HPK_EXPORT const hpk_plugin_v1* HPK_CALL pluginkit_query_v1() noexcept {             \
    return &::huxerui::pluginkit::Bind<__VA_ARGS__>();                                                     \
  }
