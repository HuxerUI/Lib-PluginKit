#pragma once

/// @file
/// @brief Host API for plugin profiles, package approval, and typed worker instances.

#include <pluginkit/contract.h>
#include <pluginkit/wire.h>

#include <chrono>
#include <filesystem>
#include <memory>

namespace pluginkit {

/// @brief Default time budget for a worker request, including time spent queued.
inline constexpr auto DefaultTimeout = std::chrono::milliseconds(5000);

/// @brief Identifies a contribution within an installed plugin.
struct ExtensionId {
  /// Plugin identifier from the manifest.
  std::string plugin;
  /// Contribution identifier from the manifest.
  std::string contribution;
};

/// @brief Metadata from a directory package and the digest of its exact contents.
///
/// The digest is SHA-256 of the two lowercase hexadecimal digests concatenated in manifest-then-library order.
/// The two input digests cover the raw file bytes.
struct PackageInfo {
  /// Package identifier used as the catalog key.
  std::string id;
  /// Package version declared in the manifest.
  std::string version;
  /// Package digest to present for approval and pass to InstallPackage().
  std::string digest;
  /// Identifier of the contract implemented by the contribution.
  std::string contract_id;
  /// Declared version of that contract.
  std::uint32_t contract_version;
  /// Fingerprint required by the host's registered contract.
  std::string contract_fingerprint;
  /// Extension point to which the plugin contributes.
  std::string point;
  /// Identifier of the contribution within the plugin.
  std::string contribution;
  /// Advertised file suffixes, including their leading dots.
  std::vector<std::string> extensions;
};

/// @brief Snapshot of an installed plugin and its activation status.
struct PluginInfo {
  /// Metadata for the approved package snapshot.
  PackageInfo package;
  /// Persisted enablement flag.
  bool enabled;
  /// Process ID of the active worker, if present.
  std::optional<std::uint32_t> worker_pid;
};

/// @brief Manifest metadata for an enabled contribution that matches a registered contract.
struct ExtensionInfo {
  /// Identifier to pass to OpenExtension.
  ExtensionId id;
  /// Advertised file suffixes, including their leading dots.
  std::vector<std::string> extensions;
};

class PluginManager;

namespace detail {

class Worker;

/// @brief Owns a worker instance on behalf of a generated extension handle.
class ExtensionInstance {
public:
  ExtensionInstance(ExtensionInstance&& other) noexcept;
  ExtensionInstance& operator=(ExtensionInstance&& other) noexcept;
  ~ExtensionInstance();

  Bytes Invoke(std::uint32_t operation, Bytes request, std::chrono::milliseconds timeout) const;
  void Close();

private:
  friend class pluginkit::PluginManager;
  ExtensionInstance(std::shared_ptr<Worker> worker, std::uint32_t instance) noexcept;
  void Release() noexcept;

  std::shared_ptr<Worker> worker_;
  std::uint32_t instance_;
};

} // namespace detail

/// @brief Owns a plugin profile and starts a worker on demand for each active plugin.
///
/// Register each extension point before installing or opening its contributions. Installed packages and enablement
/// persist in the profile. Register the points again when opening a new manager.
///
/// Manager calls serialize access to the profile and worker map. Coordinate destruction with callers.
/// Calls on one plugin worker are serialized even when they use different extension handles.
/// Workers run under the host's user account and provide process fault isolation for trusted plugins.
///
/// The CSV example assumes the point is registered and its package is approved and installed.
/// @code{.cpp}
/// #include "csv_contract.host.h"
///
/// csv_import::ImportResult ReadTable(pluginkit::PluginManager& plugins, const pluginkit::ExtensionId& id,
///                                    const std::string& csv) {
///   constexpr pluginkit::ExtensionPoint<csv_import::TableImporter> importers{"example.table-importers"};
///   auto importer = plugins.OpenExtension(importers, id);
///   auto result = importer.Import({csv});
///   importer.Close();
///   return result;
/// }
/// @endcode
/// @see ExtensionPoint, ExtensionHandle
class PluginManager {
public:
  /// @brief Creates or opens the profile and holds its exclusive lock until destruction.
  /// @param profile_directory Directory for the catalog and approved package snapshots.
  /// @param worker_executable Path to the platform's hpk-worker executable. Workers start on first open.
  /// @throws Error With ProfileBusy if another manager holds the lock, or Io if the profile cannot be read.
  PluginManager(const std::filesystem::path& profile_directory, const std::filesystem::path& worker_executable);

  /// @brief Stops active workers and releases the profile lock. Their extension handles become closed.
  ~PluginManager();

  /// @brief Registers a point and copies the generated contract identity into this manager.
  /// @tparam Interface Interface with a generated Contract specialization.
  /// @param point Identifier that contributions use in their manifests.
  /// @throws Error With InvalidArgument for invalid contract metadata or an invalid or duplicate point identifier.
  template<class Interface>
  void RegisterExtensionPoint(ExtensionPoint<Interface> point) {
    Register(point.id, Contract<Interface>::Info);
  }

  /// @brief Inspects a package before any plugin code runs.
  /// @param directory Directory containing plugin.json and the platform's plugin library.
  /// @return Package metadata and the digest of the exact bytes to approve.
  /// @throws Error With InvalidPackage for invalid manifest metadata or an empty library, Io for unreadable files,
  /// or PayloadTooLarge if the manifest exceeds 64 KiB or the library exceeds 64 MiB.
  static PackageInfo InspectPackage(const std::filesystem::path& directory);

  /// @brief Installs the approved snapshot for a registered extension point.
  ///
  /// A new installation starts enabled. Reinstalling the same approved package preserves its enablement and
  /// can repair a changed or missing snapshot. A conflicting package under the same plugin ID is rejected.
  /// @param directory Source directory containing the package files.
  /// @param approved_digest Digest from InspectPackage() that the caller approved.
  /// @throws Error With ApprovalMismatch for a different digest, ContractMismatch for an unregistered
  /// or incompatible contract, or AlreadyInstalled for a conflicting package under the same ID.
  void InstallPackage(const std::filesystem::path& directory, std::string_view approved_digest);

  /// @brief Persists enablement. Disabling a plugin also closes its active worker generation.
  /// @param plugin_id Installed plugin identifier.
  /// @param enabled True to make the contribution available for subsequent opens, false to disable it.
  /// @throws Error With NotFound if the plugin identifier is absent from the profile.
  void SetEnabled(std::string_view plugin_id, bool enabled);

  /// @brief Reads the installed catalog without activating plugins.
  /// @return A snapshot of installed packages, enablement flags, and available worker process IDs.
  std::vector<PluginInfo> ListPlugins() const;

  /// @brief Lists enabled contributions matching the point's exact registered contract.
  /// @tparam Interface Interface with a generated Contract specialization.
  /// @param point Previously registered extension point.
  /// @return Manifest metadata for matching contributions, including those whose workers have yet to start.
  /// @throws Error With NotFound for an unregistered point or ContractMismatch for a different contract.
  template<class Interface>
  std::vector<ExtensionInfo> ListExtensions(ExtensionPoint<Interface> point) const {
    return List(point.id, Contract<Interface>::Info);
  }

  /// @brief Opens an instance in the plugin worker, starting a worker on demand.
  ///
  /// Starting a worker verifies the installed snapshot. The timeout starts with the worker request, after
  /// profile access, package verification, and process startup. Reopening after a worker exit or in-flight
  /// timeout starts a new generation.
  /// @tparam Interface Interface with generated Contract and ExtensionHandle specializations.
  /// @param point Previously registered extension point.
  /// @param id Contribution to open, as returned by ListExtensions().
  /// @param timeout Time budget for queuing and exchanging the open request, from 1 ms through 24 hours.
  /// @return A move-only handle to the newly opened instance.
  /// @throws Error With NotFound, Disabled, or ContractMismatch if the contribution is unavailable for this point.
  /// @throws Error With PackageChanged if the installed snapshot fails verification, DeadlineExceeded if the
  /// open request expires, or WorkerExited if the worker exits before the reply.
  template<class Interface>
  ExtensionHandle<Interface> OpenExtension(ExtensionPoint<Interface> point, const ExtensionId& id,
                                           std::chrono::milliseconds timeout = DefaultTimeout) {
    return ExtensionHandle<Interface>(Open(point.id, Contract<Interface>::Info, id, timeout));
  }

private:
  void Register(std::string_view point, ContractInfo contract);
  std::vector<ExtensionInfo> List(std::string_view point, ContractInfo contract) const;
  detail::ExtensionInstance Open(std::string_view point, ContractInfo contract, const ExtensionId& id,
                                 std::chrono::milliseconds timeout);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pluginkit
