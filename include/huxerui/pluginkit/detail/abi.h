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

/// @brief Status returned by plugin lifecycle and operation callbacks.
enum hpk_status_v1 {
  /// The callback completed successfully.
  HPK_OK = 0,
  /// The operation or encoded request did not match the contract.
  HPK_INVALID_REQUEST = 1,
  /// Plugin code failed during a lifecycle callback, instance creation, or invocation.
  HPK_PLUGIN_FAILED = 2,
  /// The encoded response exceeds the ABI limit.
  HPK_PAYLOAD_TOO_LARGE = 3,
  /// A plugin request for a host service was denied.
  HPK_SERVICE_DENIED = 4,
  /// A host service is unavailable in the current call context.
  HPK_SERVICE_UNAVAILABLE = 5,
  /// A host callback attempted to reenter PluginKit.
  HPK_REENTRANT_CALL = 6,
  /// The receiver no longer belongs to the current call.
  HPK_RECEIVER_CLOSED = 7,
  /// The host and plugin callback contracts differ.
  HPK_CONTRACT_MISMATCH = 8,
  /// The worker is closing the plugin generation.
  HPK_CLOSED = 9,
};

/// @brief Borrowed response bytes owned by the side delivering the completion.
typedef struct hpk_buffer_v1 {
  /// Read-only view of the encoded response.
  const uint8_t* data;
  /// Number of bytes at data.
  uint32_t size;
} hpk_buffer_v1;

/// @brief Completion with a buffer borrowed for the duration of the callback.
typedef void (HPK_CALL *hpk_complete_v1)(void* user, uint32_t status, hpk_buffer_v1 result);

/// @brief Worker callbacks exposed to the plugin during an invocation.
typedef struct hpk_host_v1 {
  /// @brief Queues a host service or receiver call on the application thread.
  /// @param invocation ID passed to invoke for the current call.
  /// @param receiver Receiver slot passed to invoke, or zero to use a registered service.
  /// @param service_id Null-terminated callback contract ID.
  /// @param version Callback contract version.
  /// @param fingerprint Null-terminated callback contract fingerprint.
  /// @param operation Operation number in the callback contract.
  /// @param data Borrowed encoded request.
  /// @param size Request length in bytes.
  /// @param complete Receives the response or optional diagnostic. It runs on the transport thread.
  /// @param user Completion state transferred on success.
  /// @return Zero on admission or a PluginKit error code. Accepted calls complete once.
  uint32_t (HPK_CALL *call)(uint64_t invocation, uint32_t receiver, const char* service_id, uint32_t version,
                           const char* fingerprint, uint32_t operation, const uint8_t* data,
                           uint32_t size, hpk_complete_v1 complete, void* user);
} hpk_host_v1;

/// @brief Host-verified plugin identity and directories borrowed for the activation callback.
typedef struct hpk_plugin_context_v1 {
  /// Null-terminated plugin ID from the approved manifest.
  const char* id;
  /// Null-terminated plugin release version from the approved manifest.
  const char* version;
  /// Null-terminated lowercase SHA-256 of the approved package snapshot.
  const char* digest;
  /// Null-terminated UTF-8 path of the installed package directory.
  const char* bundle_directory;
  /// Null-terminated UTF-8 path of the writable plugin data directory.
  const char* data_directory;
} hpk_plugin_context_v1;

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

  /// @brief Initializes one plugin activation before creating any instances.
  /// @param context Host-supplied values valid for the duration of the callback.
  /// @param[out] activation Plugin-owned token shared by all instance creations. Set it to null on failure.
  /// @return HPK_OK with a valid token or an error from hpk_status_v1.
  int32_t (HPK_CALL *activate)(const hpk_plugin_context_v1* context, void** activation);

  /// @brief Creates a plugin-owned instance.
  /// @param activation Token from the successful activate callback.
  /// @param[out] instance Opaque instance pointer on success. Initialize the output to null on failure.
  /// @return HPK_OK with a valid instance or an error from hpk_status_v1.
  int32_t (HPK_CALL *create)(const void* activation, void** instance);

  /// @brief Starts a generated operation on the worker's application thread.
  /// @param instance Instance created by create.
  /// @param host Borrowed worker callbacks for this invocation.
  /// @param invocation Worker-assigned ID that identifies this call.
  /// @param receiver Slot for the caller's receiver, or zero when there is none.
  /// @param operation Operation number from the generated contract.
  /// @param data Encoded request bytes borrowed until this function returns.
  /// @param size Length of the request in bytes.
  /// @param complete Reports the final result on the application thread.
  /// @param user Completion state retained by the worker.
  /// @return HPK_OK on admission or an error from hpk_status_v1.
  int32_t (HPK_CALL *invoke)(void* instance, const hpk_host_v1* host, uint64_t invocation,
                            uint32_t receiver, uint32_t operation, const uint8_t* data, uint32_t size,
                            hpk_complete_v1 complete, void* user);

  /// @brief Destroys an instance allocated by this library.
  /// @param instance Instance returned by create.
  void (HPK_CALL *close)(void* instance);

  /// @brief Releases the activation after all instances have been destroyed.
  /// @param activation Token returned by activate. Release it even if cleanup reports failure.
  /// @return HPK_OK or an error from hpk_status_v1.
  int32_t (HPK_CALL *deactivate)(void* activation);
} hpk_plugin_v1;

/// @brief Signature of the pluginkit_query_v1 symbol exported by the plugin library.
/// @return Borrowed descriptor valid until the library is unloaded.
typedef const hpk_plugin_v1* (HPK_CALL *hpk_query_v1)(void);

#ifdef __cplusplus
}
#endif
