#pragma once

/// @file
/// @brief Host API for plugin profiles, package approval, and typed extension instances.
///
/// Link HuxerUI::PluginKit and include the generated host header for each HPK_REMOTE interface.
/// Supply the platform's hpk-worker executable to enable worker packages. In-process packages
/// use the same C++ SDK and active HuxerUI application as the host.

#include <huxerui/pluginkit/detail/extension.h>

#include <huxerui/file.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace huxerui::pluginkit {

/// @brief Default time budget for a managed extension call, including time spent queued.
inline constexpr auto DefaultTimeout = std::chrono::milliseconds(5000);

/// @brief Host approval for the exact installed package snapshot.
///
/// InstallPackage() persists the execution mode and service grants together with the approved digest.
struct InstallOptions {
  /// Approved execution mode, which must be declared by the package. Defaults to WorkerProcess.
  ExecutionMode mode = ExecutionMode::WorkerProcess;
  /// Up to 64 distinct registered service contract IDs. An empty list grants no service access.
  /// Each service granted to a worker package must have generated bindings.
  std::vector<std::string> services;
};

/// @brief Host approval to replace the installed snapshot with a specific local package.
struct UpgradeApproval {
  /// Digest of the installed snapshot shown to the user.
  std::string expected_digest;
  /// Digest from InspectPackage() for the approved replacement.
  std::string approved_digest;
  /// Complete list of service grants for the replacement. An empty list grants no services.
  std::vector<std::string> services;
};

/// @brief Whether an approved package upgrade has taken effect.
enum class UpgradeStatus {
  /// The replacement is committed in this host process.
  Applied,
  /// The replacement is staged for ApplyPendingChanges() in a new host process.
  RestartRequired,
};

/// @brief Result of applying one pending in-process upgrade at startup.
struct PendingChangeResult {
  /// Identifier of the plugin affected by the upgrade.
  std::string plugin_id;
  /// Failure category, if the previous snapshot was kept.
  std::optional<ErrorCode> error;
  /// Diagnostic for a failed upgrade.
  std::string message;
  /// True if this process must exit before it can load the selected snapshot.
  bool restart_required = false;
};

/// @brief What to do with the current plugin data when uninstalling a package.
enum class PluginDataPolicy {
  /// Reuse the current data on a later installation with the same plugin identifier.
  Keep,
  /// Delete the current data after the plugin has been uninstalled.
  Delete,
};

/// @brief Outcome after the installed package has been removed from the profile.
enum class UninstallStatus {
  /// The uninstall and requested file cleanup completed.
  Removed,
  /// Files remain in use or could not be removed. Opening the profile again retries cleanup.
  CleanupPending,
};

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
  /// Fingerprint declared in the manifest and checked against the host's registered contract.
  std::string contract_fingerprint;
  /// Extension point to which the plugin contributes.
  std::string point;
  /// Identifier of the contribution within the plugin.
  std::string contribution;
  /// Advertised file suffixes, including their leading dots.
  std::vector<std::string> extensions;
  /// Execution modes advertised by the package for this target. InstallPackage() approves one of them.
  std::vector<ExecutionMode> execution_modes;
  /// Shared SDK and C++ build identity checked for in-process execution. Ignored for worker execution.
  std::string cpp_abi;
};

/// @brief Snapshot of an installed plugin and its activation status.
struct PluginInfo {
  /// Metadata for the approved package snapshot.
  PackageInfo package;
  /// Persisted enablement flag.
  bool enabled;
  /// Execution mode selected at installation.
  ExecutionMode mode;
  /// Process ID of the active worker. Empty for an in-process package or while no worker is running.
  std::optional<std::uint32_t> worker_pid;
  /// Approved replacement waiting for a new host process, if any.
  std::optional<PackageInfo> pending_upgrade;
};

/// @brief Manifest metadata for an enabled contribution that matches a registered contract.
struct ExtensionInfo {
  /// Identifier to pass to CreateExtension().
  ExtensionId id;
  /// Advertised file suffixes, including their leading dots.
  std::vector<std::string> extensions;
};

/// @brief Move-only owner of one extension instance in the package's approved execution mode.
///
/// Call published methods through operator->() on the owning application thread. Generated bindings keep the
/// interface's synchronous and Task signatures. In-process calls invoke the implementation through C++ and
/// worker calls use IPC.
///
/// Destroying the handle releases the instance automatically. A managed call already in progress can retain
/// the instance until it finishes. A lazy call started after the handle is released fails with Closed.
/// For an interface published only in process, keep the handle alive until all uses of the borrowed object,
/// including Tasks that access it, have finished. Release these handles before awaiting disable or shutdown.
/// @tparam Interface Published C++ interface of the instance.
/// @see PluginManager::CreateExtension()
template<class Interface>
class ExtensionHandle {
public:
  /// @brief Transfers ownership from another handle, leaving the source empty.
  ExtensionHandle(ExtensionHandle&&) noexcept = default;

  /// @brief Releases this instance and takes ownership from another handle.
  /// @param other Source handle, left empty after the transfer unless it is this handle.
  /// @return This handle.
  ExtensionHandle& operator=(ExtensionHandle&& other) noexcept {
    if (this != &other) {
      if (target_) target_->Release();
      adapter_ = std::move(other.adapter_);
      target_ = std::move(other.target_);
    }
    return *this;
  }

  /// @brief Releases the instance when the handle leaves scope.
  ~ExtensionHandle() { if (target_) target_->Release(); }

  /// @brief Borrows the interface for normal C++ method calls.
  /// @return Pointer to the generated interface adapter or to the in-process object for an unmarked contract.
  /// Use it while this handle owns the instance and the plugin is active.
  /// @throws Error With Closed if the handle has moved or its instance has been retired.
  Interface* operator->() const {
    if (!target_) throw Error(ErrorCode::Closed, "The extension handle is closed");
    target_->Check();
    return adapter_ ? adapter_.get() : static_cast<Interface*>(target_->BorrowLocal());
  }

private:
  friend class PluginManager;
  explicit ExtensionHandle(std::shared_ptr<detail::ExtensionTarget> target) : target_(std::move(target)) {
    if constexpr (requires { typename Contract<Interface>::Extension; }) {
      try {
        adapter_ = std::make_unique<typename Contract<Interface>::Extension>(target_);
      } catch (...) {
        target_->Release();
        throw;
      }
    } else {
      // A native-only interface exposes the real object, so acquire it with the handle.
      try {
        (void)target_->BorrowLocal();
      } catch (...) {
        target_->Release();
        throw;
      }
    }
  }

  std::shared_ptr<detail::ExtensionTarget> target_;
  std::unique_ptr<Interface> adapter_;
};

/// @brief Owns a plugin profile and activates approved packages in their selected execution modes.
///
/// Register extension points and provide host services before installing or creating their contributions.
/// Installed packages, enablement, and approvals persist in the profile. Register points and provide services
/// again when opening a new manager.
///
/// Each profile has a single owning manager. Coordinate its destruction with callers.
/// Use the owning HuxerUI application thread for extension handles and manager operations. Catalog queries remain
/// available during host service and receiver callbacks. Managed calls on one plugin are serialized. A
/// synchronous call can report Busy while the plugin is handling another call.
/// Workers run under the host's user account and provide process fault isolation for trusted plugins.
///
/// The CSV example assumes the point is registered and its package is approved and installed.
/// @code{.cpp}
/// #include "csv_contract.host.h"
///
/// huxerui::Task<csv_import::ImportResult> ReadTable(huxerui::pluginkit::PluginManager& plugins,
///                                                const huxerui::pluginkit::ExtensionId& id,
///                                                std::shared_ptr<csv_import::ImportProgress> progress,
///                                                const std::string& csv) {
///   constexpr huxerui::pluginkit::ExtensionPoint<csv_import::TableImporter> importers{"example.table-importers"};
///   auto importer = plugins.CreateExtension(importers, id);
///   co_return co_await importer->Import({csv}, std::move(progress));
/// }
/// @endcode
/// @see ExtensionPoint, ExtensionHandle
class PluginManager {
public:
  /// @brief Creates or opens the profile and takes its exclusive lock.
  /// @param profile_directory Directory for the catalog and approved package snapshots.
  /// @param worker_executable Path to the platform's hpk-worker executable, started on demand. Pass
  /// std::nullopt to allow only in-process packages.
  /// @param callbacks Owning HuxerUI application task scope used for managed in-process calls and for host
  /// service and receiver callbacks. Keep it running until Shutdown() completes.
  /// @throws Error With ProfileBusy if another manager holds the lock, or Io if the profile cannot be read.
  /// @throws std::filesystem::filesystem_error If path resolution or profile directory creation fails.
  PluginManager(const File& profile_directory, const std::optional<File>& worker_executable, TaskScope callbacks);

  /// @brief Each profile lock belongs to one manager.
  PluginManager(const PluginManager&) = delete;
  /// @brief A manager retains ownership of the profile it opened.
  PluginManager& operator=(const PluginManager&) = delete;

  /// @brief Stops plugin admission and closes extension handles.
  /// Await Shutdown() before destruction to drain in-process work while the application scope can still run.
  /// Pending tasks can retain the profile lock until their transport work finishes.
  ~PluginManager();

  /// @brief Stops admission and waits for active plugin generations to finish teardown.
  /// Release handles to unmarked in-process interfaces before awaiting shutdown. Teardown waits for managed
  /// in-process work to finish and gives each worker a bounded shutdown budget.
  /// @return Lazy task to await before retiring the application task scope.
  /// @throws Error With ReentrantCall when awaited from a host callback.
  Task<void> Shutdown();

  /// @brief Registers a point and copies the generated contract identity into this manager.
  /// @tparam Interface Published interface with a Contract specialization. Worker packages require HPK_REMOTE bindings.
  /// @param point Identifier that contributions use in their manifests.
  /// @throws Error With InvalidArgument for invalid contract metadata or an invalid or duplicate point identifier.
  /// @throws Error With Closed after shutdown or ReentrantCall while a host callback is active on this thread.
  template<class Interface>
  void RegisterExtensionPoint(ExtensionPoint<Interface> point) {
    Register(point.id, Contract<Interface>::Info, requires { typename Contract<Interface>::Extension; });
  }

  /// @brief Inspects a package before any plugin code runs.
  /// @param directory Directory containing plugin.json and the platform's plugin library.
  /// @return Package metadata and the digest of the exact bytes to approve.
  /// @throws Error With InvalidPackage for invalid manifest metadata or an empty library, Io for unreadable files,
  /// or PayloadTooLarge if the manifest exceeds 64 KiB or the library exceeds 64 MiB.
  static PackageInfo InspectPackage(const File& directory);

  /// @brief Registers a host service once. InstallOptions authorizes its use per package.
  ///
  /// Register before approving a new package or creating handles for an installed package. A plugin activation
  /// uses the registrations selected from its saved service grants. Registrations last for this manager.
  ///
  /// Managed Task service calls run in the manager's application task scope and may interleave for different
  /// plugins. Synchronous service calls from synchronous in-process invocations run on the caller's stack.
  /// ListPlugins() and ListExtensions() can be called inside a handler. Management and managed invocations
  /// are subject to reentry checks. A change applied by the handler persists if the outer call fails.
  /// @tparam Interface Service interface with a Contract specialization. Worker service contracts require
  /// HPK_REMOTE bindings and accept no receiver parameters.
  /// @param implementation Service object retained by the manager and by activations that can call it.
  /// @throws Error With InvalidArgument for an empty implementation, invalid metadata, or duplicate registration.
  /// @throws Error With Closed after shutdown or ReentrantCall while a host callback is active on this thread.
  /// @see InstallOptions, GetService()
  template<class Interface>
  void ProvideService(std::shared_ptr<Interface> implementation) {
    if constexpr (requires { typename Contract<Interface>::Extension; }) {
      static_assert(requires { Contract<Interface>::HostService; },
                    "A remote host service cannot take a receiver parameter");
    }
    auto dispatch = detail::BindCallback(implementation);
    Provide(Contract<Interface>::Info, std::move(implementation), std::move(dispatch));
  }

  /// @brief Installs the approved snapshot for a registered extension point.
  ///
  /// A new installation starts enabled. The mode and service grants apply to future activations of this snapshot.
  /// Reinstalling with the same digest and approval can repair the stored files and preserves enablement.
  /// @param directory Source directory containing the package files.
  /// @param approved_digest Digest from InspectPackage() that the caller approved.
  /// @param options Execution mode and host services included in the approval.
  /// @throws Error With ApprovalMismatch if the source digest changed, ContractMismatch if the contract or
  /// in-process SDK identity differs, or UnsupportedMode if the package or host cannot use the approved mode.
  /// @throws Error With InvalidArgument if a service grant is invalid or unavailable in the selected mode.
  /// @throws Error With AlreadyInstalled if the installed digest or approval differs, or Busy if the active
  /// plugin's snapshot needs repair. Disable the plugin before repairing it.
  /// @throws Error With Closed after shutdown or ReentrantCall while a host callback is active on this thread.
  /// @see InspectPackage(), ProvideService()
  void InstallPackage(const File& directory, std::string_view approved_digest, InstallOptions options = {});

  /// @brief Replaces an installed plugin with an approved package from a local directory.
  ///
  /// Use the same plugin identifier, a different release version, and the installed execution mode.
  /// An enabled worker is retired before the candidate is validated on a copy of the plugin data. The
  /// package, grants, and data are selected together at commit. Disabled plugins stay disabled and
  /// activate when next used. The data copy accepts regular files and directories. An in-process upgrade
  /// is applied after a host process restart.
  /// @param directory Local package directory. The Task reads it when it runs.
  /// @param approval Installed and replacement digests and the replacement's approved services.
  /// @return Lazy Task yielding the status of the approved upgrade. Cancelling the wait after admission
  /// lets the manager finish the transaction.
  /// @throws Error With ApprovalMismatch for a changed digest, NotFound for an uninstalled plugin,
  /// Busy for a concurrent change, or the validation or I/O failure that prevented commit.
  Task<UpgradeStatus> UpgradePackage(File directory, UpgradeApproval approval);

  /// @brief Removes an installed plugin, its service grants, and any pending upgrade.
  ///
  /// Release handles that borrow native interfaces before awaiting uninstall. Loaded in-process libraries
  /// can remain mapped until host exit. The default policy preserves the current data for a later install.
  /// @param plugin_id Installed plugin identifier.
  /// @param expected_digest Digest of the installed snapshot being removed.
  /// @param data Policy for the plugin's current data directory.
  /// @return Lazy Task that completes after uninstall and the first attempt to clean up files.
  /// @throws Error With ApprovalMismatch for a changed digest, NotFound for an uninstalled plugin,
  /// Busy for a concurrent change, or Io if the profile cannot be committed.
  Task<UninstallStatus> UninstallPackage(std::string_view plugin_id, std::string_view expected_digest,
                                         PluginDataPolicy data = PluginDataPolicy::Keep);

  /// @brief Applies upgrades staged in an earlier host process.
  ///
  /// Await after registering extension points and host services and before creating extensions. A failed
  /// candidate leaves the previous snapshot selected. If the candidate was loaded in process, its
  /// library stays mapped and loading the previous snapshot requires another host process.
  /// @return Lazy Task yielding one result for each eligible pending change it attempted.
  /// @throws Error With Closed after shutdown or ReentrantCall from a host callback.
  Task<std::vector<PendingChangeResult>> ApplyPendingChanges();

  /// @brief Persists enablement and retires the active plugin when disabling it.
  ///
  /// Disabling stops admission. In-process teardown cancels managed calls, waits for tracked blocking work,
  /// and waits for handles that expose a native interface directly. Release those handles before disabling.
  /// Worker teardown allows up to DefaultTimeout for the running exchange and shutdown before termination.
  /// @param plugin_id Installed plugin identifier.
  /// @param enabled True to make the contribution available for subsequent creation, false to disable it.
  /// @return Lazy task that completes after persisting the change and finishing any teardown.
  /// @throws Error With NotFound if the plugin is absent, Closed after shutdown, or ReentrantCall from a host callback.
  Task<void> SetEnabled(std::string_view plugin_id, bool enabled);

  /// @brief Reads the installed catalog without activating plugins.
  /// @return A snapshot of installed packages, enablement flags, and available worker process IDs.
  /// @see ListExtensions()
  std::vector<PluginInfo> ListPlugins() const;

  /// @brief Lists enabled contributions matching the point's exact registered contract.
  /// @tparam Interface Published interface with a Contract specialization.
  /// @param point Previously registered extension point.
  /// @return Manifest metadata for matching contributions, including those that have yet to activate.
  /// @throws Error With NotFound for an unregistered point or ContractMismatch for a different contract.
  /// @see ListPlugins()
  template<class Interface>
  std::vector<ExtensionInfo> ListExtensions(ExtensionPoint<Interface> point) const {
    return List(point.id, Contract<Interface>::Info);
  }

  /// @brief Synchronously creates a handle to one contribution in its approved execution mode.
  ///
  /// With generated bindings, instance creation is deferred until the first synchronous method call or
  /// until the first Task method is awaited. For an unmarked in-process interface, this function creates
  /// the instance immediately so operator->() can expose the implementation directly.
  /// @tparam Interface Interface with contract metadata.
  /// @param point Previously registered extension point.
  /// @param id Contribution to instantiate, as returned by ListExtensions().
  /// @param timeout Time budget for each managed call, from 1 ms through 24 hours, including time in the queue.
  /// An unmarked in-process interface runs under the caller's ordinary C++ lifetime and timing rules.
  /// @return A move-only handle exposing Interface through operator->.
  /// @throws Error With NotFound, Disabled, or ContractMismatch if the contribution is unavailable for this point,
  /// InvalidArgument for an invalid timeout, Closed after shutdown, or UnsupportedMode if a worker is unavailable
  /// for the approved mode. Unmarked interfaces can also report activation or construction failures here.
  template<class Interface>
  ExtensionHandle<Interface> CreateExtension(ExtensionPoint<Interface> point, const ExtensionId& id,
                                             std::chrono::milliseconds timeout = DefaultTimeout) {
    return ExtensionHandle<Interface>(Create(point.id, Contract<Interface>::Info, id, timeout));
  }

private:
  void Provide(ContractInfo contract, std::shared_ptr<void> implementation, detail::ServiceDispatch dispatch);
  void Register(std::string_view point, ContractInfo contract, bool remote);
  std::vector<ExtensionInfo> List(std::string_view point, ContractInfo contract) const;
  struct Impl;
  static Task<void> ShutdownManager(std::shared_ptr<Impl> impl);
  std::shared_ptr<detail::ExtensionTarget> Create(std::string_view point, ContractInfo contract,
                                                  const ExtensionId& id, std::chrono::milliseconds timeout);

  // A pending task can retain the implementation until its transport work finishes.
  std::shared_ptr<Impl> impl_;
};

} // namespace huxerui::pluginkit
