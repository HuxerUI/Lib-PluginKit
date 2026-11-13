#pragma once

/// @file
/// @brief Contract declarations and errors shared by the host and independently built plugins.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

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

/// @brief Specialized by the SDK generator with metadata and a dispatcher.
/// @tparam Interface C++ interface passed to hpk gen.
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

/// @brief Generated move-only handle that invokes operations on one worker instance.
/// @tparam Interface Interface exposed by the handle.
/// @see PluginManager::CreateExtension()
template<class Interface>
class ExtensionHandle;

/// @brief Generated proxy for calling a typed host service from a plugin.
/// @tparam Interface Service interface passed to hpk gen.
template<class Interface>
class ServiceHandle;

/// @brief Generated call-scoped proxy for an interface implemented by the caller.
/// @tparam Interface Callback interface passed to hpk gen.
template<class Interface>
class Receiver;

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
  /// A different package is installed under the same plugin identifier.
  AlreadyInstalled,
  /// The requested plugin, extension point, or contribution was not found.
  NotFound,
  /// The requested plugin is disabled.
  Disabled,
  /// The registered contract, manifest, or exported descriptor did not match.
  ContractMismatch,
  /// The extension instance or worker generation is closed.
  Closed,
  /// A request expired in the queue or during a worker exchange.
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
