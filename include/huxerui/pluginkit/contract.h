#pragma once

/// @file
/// @brief Contract declarations and errors shared by the host and independently built plugins.

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

/// @brief Marks an interface for hpk gen to publish in both execution modes.
///
/// Place it on the class definition, as in `class HPK_REMOTE Importer`. Declare a public virtual
/// destructor and public pure virtual operations with unique names. Operations may return a synchronous
/// value, void, or HuxerUI Task<T>. Pass Task arguments by value. Synchronous methods can also borrow
/// supported ordinary values by const reference for the duration of the call.
/// Declare ordinary values and invocation endpoints in any order. Synchronous methods take InputStream
/// and OutputStream. Task methods take AsyncInputStream and AsyncOutputStream and can also accept
/// std::shared_ptr<T> to an HPK_REMOTE interface with Task operations. A null pointer omits that callback.
/// Each stream or callback is granted for the invocation that receives it. Await callbacks from that
/// invocation or an awaited child Task. Access expires at completion or cancellation. Successfully
/// close every supplied output before returning.
/// Leave operations able to throw so the binding can report failures. The macro expands to an annotation
/// only when hpk gen parses the header.
#if defined(HPK_GENERATOR)
#define HPK_REMOTE [[clang::annotate("hpk.remote")]]
#else
#define HPK_REMOTE
#endif

namespace huxerui::pluginkit {

/// @brief Identity of one plugin package snapshot.
struct PluginIdentity {
  /// Plugin identifier from the manifest.
  std::string id;
  /// Plugin release version from the manifest.
  std::string version;
  /// SHA-256 digest of the package snapshot.
  std::string digest;
};

/// @brief A plugin setting value. An enumeration uses its declared UTF-8 string value.
using SettingValue = std::variant<bool, std::int64_t, std::string>;

/// @brief Setting values indexed by their stable manifest keys.
using SettingValues = std::map<std::string, SettingValue, std::less<>>;

/// @brief Exact identity of a generated contract.
///
/// String views borrow their underlying storage. Generated bindings use static storage for both strings.
struct ContractInfo {
  /// Stable contract identifier.
  std::string_view id;
  /// Positive version supplied to hpk gen.
  std::uint32_t version;
  /// Lowercase SHA-256 of the generated contract schema.
  std::string_view fingerprint;

  /// @brief Compares contract identifiers, versions, and fingerprints.
  /// @return True when all three values match.
  bool operator==(const ContractInfo&) const = default;
};

/// @brief Describes the contract published by a C++ interface.
///
/// For an HPK_REMOTE interface, hpk gen supplies the exact identity, typed adapter, and worker dispatch.
/// For an interface used only in process, define a specialization with `static constexpr ContractInfo Info`.
/// @tparam Interface Published C++ interface.
template<class Interface>
struct Contract;

/// @brief Associates a typed contract with an extension point registered by the host.
/// @tparam Interface Interface accepted by the point's contributions.
template<class Interface>
struct ExtensionPoint {
  /// Extension point identifier used in plugin manifests. Use up to 128 lowercase ASCII letters, digits, dots,
  /// underscores, or hyphens, starting with a letter or digit.
  std::string_view id;
};

/// @brief Move-only handle to a plugin instance in either execution mode.
/// @tparam Interface Interface exposed by the handle.
/// @see PluginManager::CreateExtension()
template<class Interface>
class ExtensionHandle;

/// @brief Typed handle for calling a host service during a plugin invocation.
/// @tparam Interface Published service interface with a Contract specialization.
/// @see GetService()
template<class Interface>
class ServiceHandle;

/// @brief Maps an application value to a supported wire value for IPC.
/// @tparam T Owning value type used in a published contract.
///
/// Specialize in the shared contract SDK. Declare a WireType alias, static WireType Encode(const T&),
/// and static T Decode(WireType). Encode and Decode preserve the value's meaning. Decode constructs
/// an independent value and reports an invalid representation by throwing. PluginKit encodes the
/// representation with its bounded CBOR implementation. In-process calls use T directly.
template<class T>
struct WireCodec;

/// @brief Machine-readable reason for a failed PluginKit operation.
enum class ErrorCode : std::uint32_t {
  /// The caller supplied an invalid argument.
  InvalidArgument = 1,
  /// A filesystem or system operation failed.
  Io,
  /// Another manager owns the profile lock.
  ProfileBusy,
  /// The manifest or plugin library could not be accepted.
  InvalidPackage,
  /// The source package no longer matches the approved digest.
  ApprovalMismatch,
  /// The installed package could not be verified against its catalog entry.
  PackageChanged,
  /// A package is already installed under this identifier with a different digest or approval.
  AlreadyInstalled,
  /// The requested plugin, extension point, or contribution was not found.
  NotFound,
  /// The requested plugin is disabled.
  Disabled,
  /// The registered contract, manifest, or exported descriptor did not match.
  ContractMismatch,
  /// The manager, extension instance, or plugin activation is closed.
  Closed,
  /// A managed call exhausted its time budget, including time in the queue.
  DeadlineExceeded,
  /// The worker process or its transport became unavailable.
  WorkerExited,
  /// A frame or encoded payload violated the worker protocol or contract.
  Protocol,
  /// Plugin code reported a failure.
  PluginFailed,
  /// A byte or item limit was exceeded.
  PayloadTooLarge,
  /// The installed package was not authorized to call the requested host service.
  ServiceDenied,
  /// The host service cannot be used in the current plugin call context.
  ServiceUnavailable,
  /// An active host service or receiver callback blocked a manager or worker operation.
  ReentrantCall,
  /// The caller cancelled the operation.
  Cancelled,
  /// The receiver no longer belongs to an active call.
  ReceiverClosed,
  /// The selected execution mode is unavailable for this package or contract.
  UnsupportedMode,
  /// A synchronous call attempted a host callback that requires an asynchronous call or an in-process caller.
  CallbackUnavailable,
  /// A synchronous call could not enter a busy plugin, or a package repair requires an inactive plugin.
  Busy,
  /// The plugin provides several factories at the requested extension point.
  Ambiguous,
  /// The settings revision supplied by the host is no longer current.
  SettingsChanged,
};

/// @brief A PluginKit failure with an error code and a human-readable diagnostic.
///
/// Use Code() for application decisions and what() for diagnostics.
class Error : public std::runtime_error {
public:
  /// @brief Creates a PluginKit exception.
  /// @param code Category returned by Code().
  /// @param message Null-terminated diagnostic text copied into the exception.
  Error(ErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}
  /// @brief Creates a PluginKit exception from a string.
  /// @param code Category returned by Code().
  /// @param message Diagnostic text copied into the exception.
  Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}

  /// @brief Reads the failure category.
  /// @return The code supplied when constructing the exception.
  ErrorCode Code() const noexcept { return code_; }

private:
  ErrorCode code_;
};

/// @brief Resolves an error code to its diagnostic identifier.
/// @param code PluginKit error code.
/// @return A view of a static lowercase identifier, or "unknown" for an unrecognized value.
std::string_view ErrorName(ErrorCode code) noexcept;

} // namespace huxerui::pluginkit
