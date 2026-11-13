#pragma once

/// @file
/// @brief SDK helpers for exporting the generated plugin contract from a shared library.

#include <pluginkit/contract.h>
#include <pluginkit/wire.h>

#include <memory>

namespace pluginkit {

/// @brief Builds the ABI descriptor for a generated contract and its implementation.
///
/// The descriptor's callbacks allocate and release instances and response buffers inside the library.
/// @tparam Interface Interface with a generated Contract specialization.
/// @tparam Implementation Publicly derived, default-constructible implementation of Interface.
/// @return Reference to a descriptor that remains valid while the plugin library is loaded.
template<class Interface, class Implementation>
  requires std::derived_from<Implementation, Interface> && std::default_initializable<Implementation>
const hpk_plugin_v1& Bind() noexcept {
  static const hpk_plugin_v1 binding{
      sizeof(hpk_plugin_v1), HPK_ABI_VERSION, Contract<Interface>::Info.id.data(),
      Contract<Interface>::Info.version, Contract<Interface>::Info.fingerprint.data(),
      [](void** instance) noexcept -> std::int32_t {
        *instance = nullptr;
        try {
          *instance = new Implementation;
          return HPK_OK;
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      [](void* instance, std::uint32_t operation, const std::uint8_t* data, std::uint32_t size,
         hpk_buffer_v1* result) noexcept -> std::int32_t {
        *result = {};
        try {
          auto bytes = std::make_unique<Bytes>(
              Contract<Interface>::Dispatch(*static_cast<Implementation*>(instance), operation, {data, size}));
          *result = {bytes->data(), static_cast<std::uint32_t>(bytes->size()), bytes.get()};
          bytes.release();
          return HPK_OK;
        } catch (const Error& error) {
          if (error.Code() == ErrorCode::PayloadTooLarge) return HPK_PAYLOAD_TOO_LARGE;
          if (error.Code() == ErrorCode::Protocol) return HPK_INVALID_REQUEST;
          return HPK_PLUGIN_FAILED;
        } catch (...) {
          return HPK_PLUGIN_FAILED;
        }
      },
      [](hpk_buffer_v1 buffer) noexcept { delete static_cast<Bytes*>(buffer.owner); },
      [](void* instance) noexcept { delete static_cast<Implementation*>(instance); },
  };
  return binding;
}

} // namespace pluginkit

/// @brief Platform export annotation for the plugin entry point.
#if defined(_WIN32)
#define HPK_EXPORT __declspec(dllexport)
#else
#define HPK_EXPORT __attribute__((visibility("default")))
#endif

/// @brief Defines pluginkit_query_v1. Use once at global scope in the plugin library.
/// @param Interface Interface declared by the published SDK.
/// @param Implementation Default-constructible type implementing Interface.
/// @code{.cpp}
/// HPK_EXPORT_PLUGIN(csv_import::TableImporter, csv_import::CsvImporter)
/// @endcode
#define HPK_EXPORT_PLUGIN(Interface, Implementation)                                              \
  extern "C" HPK_EXPORT const hpk_plugin_v1* HPK_CALL pluginkit_query_v1() noexcept {             \
    return &::pluginkit::Bind<Interface, Implementation>();                                       \
  }
