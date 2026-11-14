#pragma once

/// @file
/// @brief Contract declarations and errors shared by the host and independently built plugins.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

/// @brief Marks an interface for hpk gen to publish in both execution modes.
///
/// Place it on the class definition, as in `class HPK_REMOTE Importer`. Declare a public virtual
/// destructor and public pure virtual operations with unique names. Operations may return a synchronous
/// value, void, or HuxerUI Task<T>. Use contract-supported types and pass Task requests by value.
/// A Task operation can take a Receiver as its second argument. Leave operations able to throw so
/// the binding can report failures. The macro expands to an annotation only when hpk gen parses the header.
#if defined(HPK_GENERATOR)
#define HPK_REMOTE [[clang::annotate("hpk.remote")]]
#else
#define HPK_REMOTE
#endif

namespace huxerui::pluginkit {

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

/// @brief Contract identity owned by a package metadata snapshot.
struct PackageContract {
  /// Stable identifier published by the host SDK.
  std::string id;
  /// Exact contract version implemented by the plugin.
  std::uint32_t version;
  /// Lowercase SHA-256 fingerprint of the implemented contract.
  std::string fingerprint;

  /// @brief Borrows the identity for comparison with a registered contract.
  /// @return Views valid for the lifetime of these strings.
  ContractInfo Info() const noexcept { return {id, version, fingerprint}; }
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

/// @brief Optional host callback object supplied to one managed Task invocation.
///
/// Pass an empty pointer to omit callbacks. When supplied, the host object is retained for the call and the
/// plugin receives an invocation-scoped implementation. Await its methods from the owning invocation or an
/// awaited child Task. A retained proxy fails with ReceiverClosed after completion, cancellation, or when
/// called from a different execution.
/// @tparam Interface HPK_REMOTE callback interface declared in its own header, with Task operations and
/// at most one request per operation.
template<class Interface>
using Receiver = std::shared_ptr<Interface>;

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
