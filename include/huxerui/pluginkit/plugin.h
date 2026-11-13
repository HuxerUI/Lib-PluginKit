#pragma once

/// @file
/// @brief Plugin context, host services, and export bindings for plugin implementations.

#include <huxerui/file.h>
#include <huxerui/pluginkit/detail/invocation.h>

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
/// The binding passes the same object to lifecycle hooks and to constructors that accept it.
/// It remains valid through Deactivate(). Copy individual values if they must outlive the activation.
class PluginContext {
public:
  /// @brief Context identity is shared by reference within its activation.
  PluginContext(const PluginContext&) = delete;
  PluginContext& operator=(const PluginContext&) = delete;

  /// @brief Returns the identity of the approved plugin package.
  const PluginIdentity& Info() const noexcept { return info_; }

  /// @brief Returns the absolute directory of the installed package snapshot.
  /// Treat its contents as read-only.
  const File& BundleDirectory() const noexcept { return bundle_; }

  /// @brief Returns the absolute writable directory reserved for this plugin in the host profile.
  /// The host creates it before activation and reuses it across worker generations.
  const File& DataDirectory() const noexcept { return data_; }

private:
  friend struct detail::PluginContextAccess;
  PluginContext(PluginIdentity info, File bundle, File data)
      : info_(std::move(info)), bundle_(std::move(bundle)), data_(std::move(data)) {}

  PluginIdentity info_;
  File bundle_;
  File data_;
};

/// @brief Obtains a typed host service proxy for the current plugin invocation.
///
/// Await proxy methods from the invocation's task or a child task awaited by it. The host checks authorization
/// when the proxy method runs.
/// @tparam Interface Host service interface with generated bindings.
/// @return Proxy valid while the current invocation is active.
/// @throws Error With ServiceUnavailable if the calling thread has no active plugin invocation.
template<class Interface>
ServiceHandle<Interface> GetService() {
  if (!detail::current_invocation) {
    throw Error(ErrorCode::ServiceUnavailable, "Host services require an active plugin invocation");
  }
  return ServiceHandle<Interface>(detail::HostEndpoint(*detail::current_invocation));
}

namespace detail {

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
    DispatchContext context{{host, invocation, co_await CurrentTaskExecution{}}, receiver};
    // The worker admits only one plugin invocation. Proxies also check the awaiting task's identity.
    InvocationScope scope(context.invocation);
    auto bytes = co_await Contract<Interface>::Dispatch(instance, operation, std::move(request), context);
    complete(user, HPK_OK, {reinterpret_cast<const std::uint8_t*>(bytes.data()),
                            static_cast<std::uint32_t>(bytes.size())});
  } catch (const Error& error) {
    complete(user, static_cast<std::uint32_t>(status(error)), {});
  } catch (...) {
    complete(user, HPK_PLUGIN_FAILED, {});
  }
}

} // namespace detail

/// @brief Builds the ABI descriptor for a generated contract and its implementation.
///
/// The descriptor creates and destroys instances inside the library and lends response buffers to completion callbacks.
/// @tparam Interface Interface with a generated Contract specialization.
/// @tparam Implementation Public implementation constructible from const PluginContext& or by default.
/// @tparam Activate Optional plugin-wide initialization hook. A thrown exception fails activation.
/// @tparam Deactivate Optional cleanup hook called after the worker has destroyed all instances.
/// @note The hooks run on the worker's HuxerUI application thread. Host service proxies require an active invocation.
/// @return Reference to a descriptor that remains valid while the plugin library is loaded.
template<class Interface, class Implementation,
         void (*Activate)(const PluginContext&) = nullptr, void (*Deactivate)(const PluginContext&) = nullptr>
  requires std::derived_from<Implementation, Interface> &&
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
      default: return HPK_PLUGIN_FAILED;
    }
  };
  static const hpk_plugin_v1 binding{
      sizeof(hpk_plugin_v1), HPK_ABI_VERSION, Contract<Interface>::Info.id.data(),
      Contract<Interface>::Info.version, Contract<Interface>::Info.fingerprint.data(),
      [](const hpk_plugin_context_v1* input, void** activation) noexcept -> std::int32_t {
        *activation = nullptr;
        try {
          auto context = detail::PluginContextAccess::Create(*input);
          if constexpr (Activate != nullptr) Activate(*context);
          // Transfer context ownership to the worker after the activation hook succeeds.
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
            *instance = new Implementation(*static_cast<const PluginContext*>(activation));
          } else {
            *instance = new Implementation;
          }
          return HPK_OK;
        } catch (const Error& error) {
          return status(error);
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      [](void* instance, const hpk_host_v1* host, std::uint64_t invocation,
         std::uint32_t receiver, std::uint32_t operation, const std::uint8_t* data, std::uint32_t size,
         hpk_complete_v1 complete, void* user) noexcept -> std::int32_t {
        try {
          if (!instance || !host || !invocation || (size && !data)) return HPK_INVALID_REQUEST;
          if (size > MaxPayload) return HPK_PAYLOAD_TOO_LARGE;
          Bytes request;
          if (size) {
            const auto bytes = std::as_bytes(std::span(data, size));
            request.assign(bytes.begin(), bytes.end());
          }
          (void)UseApplicationTaskScope().Launch(detail::DispatchPlugin<Interface>(
              *static_cast<Implementation*>(instance), host, invocation, receiver, operation, std::move(request),
              complete, user, status));
          return HPK_OK;
        } catch (const Error& error) {
          return status(error);
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      [](void* instance) noexcept { delete static_cast<Implementation*>(instance); },
      [](void* activation) noexcept -> std::int32_t {
        const std::unique_ptr<PluginContext> context(static_cast<PluginContext*>(activation));
        try {
          if constexpr (Deactivate != nullptr) Deactivate(*context);
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

/// @brief Platform export annotation for the plugin entry point.
#if defined(_WIN32)
#define HPK_EXPORT __declspec(dllexport)
#else
#define HPK_EXPORT __attribute__((visibility("default")))
#endif

/// @brief Defines pluginkit_query_v1. Use once at global scope in the plugin library.
/// @param ... Published interface and implementation type, followed by optional Activate and Deactivate
/// function pointers. The implementation may accept const PluginContext&. Each hook takes const
/// PluginContext& and returns void.
/// @code{.cpp}
/// HPK_EXPORT_PLUGIN(csv_import::TableImporter, csv_import::CsvImporter)
/// @endcode
#define HPK_EXPORT_PLUGIN(...)                                                                  \
  extern "C" HPK_EXPORT const hpk_plugin_v1* HPK_CALL pluginkit_query_v1() noexcept {             \
    return &::huxerui::pluginkit::Bind<__VA_ARGS__>();                                                     \
  }
