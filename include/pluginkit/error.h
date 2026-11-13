#pragma once

/// @file
/// @brief Error codes and exceptions reported by PluginKit.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pluginkit {

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

} // namespace pluginkit
