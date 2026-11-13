#pragma once

/// @file
/// @brief C ABI shared by the plugin library and the worker that loads it.
///
/// Export pluginkit_query_v1 using this header. The worker checks the descriptor size, ABI version, and exact
/// contract identity before calling into the implementation.

#include <stdint.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/// @brief Calling convention used by the exported query and all ABI callbacks.
#if defined(_WIN32) && defined(_M_X64)
#define HPK_CALL __cdecl
#elif defined(__linux__) && defined(__x86_64__) && !defined(__ILP32__)
#define HPK_CALL
#elif defined(__APPLE__) && TARGET_OS_OSX && defined(__aarch64__)
#define HPK_CALL
#else
#error PluginKit ABI v1 requires Windows x64, Linux x86_64, or macOS arm64.
#endif

/// @brief Descriptor version expected by the worker.
#define HPK_ABI_VERSION 1u

/// @brief Maximum size in bytes of an encoded request or response.
#define HPK_MAX_PAYLOAD (8u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Status returned by the instance creation and operation callbacks.
enum hpk_status_v1 {
  /// The callback completed successfully.
  HPK_OK = 0,
  /// The operation or encoded request did not match the contract.
  HPK_INVALID_REQUEST = 1,
  /// Plugin code failed during creation or invocation.
  HPK_PLUGIN_FAILED = 2,
  /// The encoded response exceeds the ABI limit.
  HPK_PAYLOAD_TOO_LARGE = 3,
};

/// @brief Response bytes owned by the plugin until its release callback is called.
typedef struct hpk_buffer_v1 {
  /// Read-only view of the encoded response.
  const uint8_t* data;
  /// Number of bytes at data.
  uint32_t size;
  /// Opaque allocation token passed back to the plugin.
  void* owner;
} hpk_buffer_v1;

/// @brief Descriptor and callbacks for one contract.
///
/// Keep the descriptor and its strings valid while the library is loaded. The worker calls these operations
/// serially. Contain C++ exceptions within the library and report callback failures with hpk_status_v1.
typedef struct hpk_plugin_v1 {
  /// Size of this descriptor in bytes.
  uint32_t struct_size;
  /// PluginKit ABI version.
  uint32_t abi_version;
  /// Null-terminated contract identifier.
  const char* contract_id;
  /// Declared contract version.
  uint32_t contract_version;
  /// Null-terminated lowercase SHA-256 of the generated schema.
  const char* contract_fingerprint;

  /// @brief Creates a plugin-owned instance.
  /// @param[out] instance Opaque instance pointer on success. Initialize the output to null on failure.
  /// @return HPK_OK with a valid instance, or HPK_PLUGIN_FAILED.
  int32_t (HPK_CALL *create)(void** instance);

  /// @brief Runs a generated operation and encodes its result.
  /// @param instance Instance created by create.
  /// @param operation Operation number from the generated contract.
  /// @param data Encoded request bytes borrowed for the duration of the call.
  /// @param size Length of the request in bytes.
  /// @param[out] result Plugin-owned response on success. Leave the buffer empty on failure.
  /// @return HPK_OK with an encoded response, or an error from hpk_status_v1.
  int32_t (HPK_CALL *invoke)(void* instance, uint32_t operation, const uint8_t* data, uint32_t size,
                            hpk_buffer_v1* result);

  /// @brief Releases a response allocated by this library.
  /// @param buffer Buffer returned by invoke, including its opaque owner token.
  void (HPK_CALL *release)(hpk_buffer_v1 buffer);

  /// @brief Destroys an instance allocated by this library.
  /// @param instance Instance returned by create.
  void (HPK_CALL *close)(void* instance);
} hpk_plugin_v1;

/// @brief Signature of the pluginkit_query_v1 symbol exported by the plugin library.
/// @return Borrowed descriptor valid until the library is unloaded.
typedef const hpk_plugin_v1* (HPK_CALL *hpk_query_v1)(void);

#ifdef __cplusplus
}
#endif
