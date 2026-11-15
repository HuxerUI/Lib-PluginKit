#pragma once

/// @file
/// @brief Host API for plugin profiles, package approval, and typed extension instances.
///
/// Link HuxerUI::PluginKit and include the generated host header for each HPK_REMOTE interface.
/// Supply the platform's hpk-worker executable to enable worker packages. In-process packages
/// use the same C++ SDK and active HuxerUI application as the host.

#include <huxerui/pluginkit/detail/extension.h>

#include <huxerui/file.h>
#include <huxerui/resource.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

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

/// @brief What to do with the plugin's data and saved settings when uninstalling a package.
enum class PluginDataPolicy {
  /// Retain data and reuse compatible settings on a later installation with the same plugin identifier.
  Keep,
  /// Discard saved settings and delete the data directory after uninstall commits.
  Delete,
};

/// @brief Outcome after the installed package has been removed from the profile.
enum class UninstallStatus {
  /// The uninstall and requested file cleanup completed.
  Removed,
  /// Files remain in use or could not be removed. Opening the profile again retries cleanup.
  CleanupPending,
};

namespace detail {
struct ManifestParser;
}

/// @brief Describes one factory declared by a specific package snapshot.
///
/// Package inspection and catalog queries return this information before activation. The entry can select
/// the factory once its exact package snapshot is installed and enabled.
/// @see PluginManager::InspectPackage(), PluginManager::ListExtensions(), PluginManager::CreateExtension()
class ExtensionInfo {
public:
  /// @brief Returns the identifier of the declaring package.
  /// @return Reference valid for the lifetime of this entry.
  const std::string& PluginId() const noexcept { return plugin_id_; }

  /// @brief Returns the name of the extension point implemented by this factory.
  /// @return Reference valid for the lifetime of this entry.
  const std::string& Point() const noexcept { return point_; }

  /// Validated UTF-8 JSON object text. Formatting can differ from the manifest.
  /// An omitted metadata field yields std::nullopt. An empty object yields "{}". The host interprets the object.
  std::optional<std::string> metadata_json;

private:
  friend class PluginManager;
  friend struct detail::ManifestParser;
  ExtensionInfo(std::string_view plugin_id, std::string point, std::optional<std::string> metadata,
                std::string_view digest, std::uint32_t ordinal)
      : metadata_json(std::move(metadata)), plugin_id_(plugin_id), point_(std::move(point)), digest_(digest),
        ordinal_(ordinal) {}

  std::string plugin_id_;
  std::string point_;
  std::string digest_;
  std::uint32_t ordinal_;
};

/// @brief Setting editor and value type declared in the package manifest.
enum class SettingType {
  Boolean,     ///< Toggle backed by bool.
  Integer,     ///< Signed 64-bit integer with optional inclusive bounds.
  String,      ///< Free-form UTF-8 text.
  Enumeration, ///< Choice from a declared list of UTF-8 values.
};

/// @brief A literal label or a string resource in the declaring package.
using SettingText = std::variant<std::string, StringResource>;

/// @brief One choice in an enumeration setting.
struct SettingOption {
  /// Stable value used in the profile and in plugin code.
  std::string value;
  /// Label shown in the host editor.
  SettingText title;
};

/// @brief A validated manifest declaration used to build a setting editor.
struct SettingDefinition {
  /// Stable key used to address the setting. It follows the manifest identifier syntax.
  std::string key;
  /// Determines the editor, value type, and applicable constraints.
  SettingType type;
  /// Label shown in the host editor.
  SettingText title;
  /// Additional text supplied by the plugin author, if any.
  std::optional<SettingText> description;
  /// Effective value when the host has saved no override for this key.
  SettingValue default_value;
  /// Inclusive lower bound for an integer, if declared.
  std::optional<std::int64_t> minimum;
  /// Inclusive upper bound for an integer, if declared.
  std::optional<std::int64_t> maximum;
  /// Choices in manifest order for an enumeration. Empty for other types.
  std::vector<SettingOption> options;
};

/// @brief An owned view of the committed settings and the current activation's revision.
struct SettingsSnapshot {
  /// Declarations in manifest order. All display text holds resolved strings for the requested locale.
  std::vector<SettingDefinition> schema;
  /// One effective value per declared key, with saved overrides applied to the defaults.
  SettingValues effective;
  /// Explicitly saved values. Remove a key from this map to use its default on the next save.
  SettingValues overrides;
  /// Opaque revision to pass to SetSettingsAsync() when saving an edit to this snapshot.
  std::string revision;
  /// Revision of the running activation, or std::nullopt while the plugin is inactive or transitioning.
  std::optional<std::string> active_revision;
};

/// @brief Reason a proposed override failed validation.
enum class SettingIssueKind {
  UnknownKey,    ///< No setting with this key is declared.
  TypeMismatch,  ///< The value does not have the declared type.
  OutOfRange,    ///< An integer exceeds one of the declared bounds.
  UnknownOption, ///< The string does not match a declared choice value.
  InvalidText,   ///< The string is not valid UTF-8.
};

/// @brief Validation failure associated with a single setting key.
struct SettingIssue {
  /// Key of the rejected override.
  std::string key;
  /// Constraint the override violated.
  SettingIssueKind kind;
};

/// @brief InvalidArgument error containing the rejected settings from a single save attempt.
class SettingsValidationError : public Error {
public:
  /// @brief Creates a validation error with the collected field issues.
  /// @param issues Failures identified before committing the proposed overrides.
  explicit SettingsValidationError(std::vector<SettingIssue> issues)
      : Error(ErrorCode::InvalidArgument, "Plugin settings failed validation"), issues_(std::move(issues)) {}

  /// @brief Returns the rejected fields and their reasons in key order.
  /// @return Reference valid for the lifetime of this error.
  const std::vector<SettingIssue>& Issues() const noexcept { return issues_; }

private:
  std::vector<SettingIssue> issues_;
};

/// @brief Metadata, settings schema, and SHA-256 digest of the installable files in a directory package.
///
/// The digest covers plugin.json, the platform's plugin library, and every file under resources, if present.
/// It hashes records sorted by relative UTF-8 path. Each record contains the path with forward slashes,
/// one NUL byte, and the 64 lowercase hexadecimal SHA-256 digits of the file's exact bytes.
struct PackageInfo {
  /// Package identifier used as the catalog key.
  std::string id;
  /// Package version declared in the manifest.
  std::string version;
  /// Package digest to present for approval and pass to InstallPackage().
  std::string digest;
  /// Factories in manifest order, up to HPK_MAX_EXTENSIONS. Several factories may implement the same point.
  std::vector<ExtensionInfo> extensions;
  /// Execution modes advertised by the package for this target. InstallPackage() approves one of them.
  std::vector<ExecutionMode> execution_modes;
  /// Shared SDK and C++ build identity checked for in-process execution. Ignored for worker execution.
  std::string cpp_abi;
  /// Setting declarations in manifest order, retaining their literal labels or package string resource identities.
  std::vector<SettingDefinition> settings_schema;
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

/// @brief Catalog or runtime change reported by a plugin manager.
enum class PluginEventKind {
  CatalogChanged,   ///< A catalog change committed. See PluginEvent::change.
  Activated,        ///< The selected package completed activation.
  ActivationFailed, ///< The selected package could not activate.
  Stopped,          ///< A runtime generation finished retiring. See PluginEvent::stop_reason.
  WorkerExited,     ///< A worker exited unexpectedly, including while idle.
  UpgradeFailed,    ///< An accepted replacement failed. See PluginEvent::package for the candidate.
};

/// @brief Change to the committed catalog.
enum class PluginCatalogChange {
  Installed,      ///< A newly installed package became available.
  Enabled,        ///< The installed package was enabled.
  Disabled,       ///< The package was disabled and admission to new calls closed.
  UpgradePending, ///< A replacement was approved for application after host restart.
  Upgraded,       ///< The replacement became the installed package.
  Uninstalled,    ///< The package was removed from the catalog.
  SettingsChanged, ///< A new settings revision committed. It takes effect on the next activation.
};

/// @brief Runtime state of an installed package, independent of its enablement.
enum class PluginRuntimeState {
  Inactive,   ///< No activation is running.
  Activating, ///< The package is being verified, loaded, or activated.
  Active,     ///< The package completed activation and is available for calls.
  Stopping,   ///< The existing runtime is being retired.
  Faulted,    ///< The most recent runtime attempt failed or terminated unexpectedly.
};

/// @brief Reason for retiring a runtime generation.
enum class PluginStopReason {
  Disabled,         ///< The host disabled the package.
  Upgrading,        ///< The host is validating or applying a replacement.
  Uninstalled,      ///< The host is uninstalling the package.
  Shutdown,         ///< The manager or a temporary activation is shutting down.
  Cancelled,        ///< Cancelling an active exchange retired its worker.
  DeadlineExceeded, ///< An exchange timed out and its worker was retired.
  TransportError,   ///< A failed exchange required retiring the worker.
};

/// @brief Operation stage in which PluginKit detected a failure.
enum class PluginFailureStage {
  VerifyPackage,     ///< Checking the selected package and its approval.
  StagePackage,      ///< Staging the approved package snapshot.
  RetirePrevious,    ///< Retiring the installed runtime before a change.
  PrepareData,       ///< Preparing the candidate package and its data copy.
  StartWorker,       ///< Starting the worker process.
  Activate,          ///< Loading the module, validating its descriptor, or running its activation hook.
  ValidateFactories, ///< Probing the replacement's factories.
  Commit,            ///< Writing the resulting profile state.
  Exchange,          ///< Exchanging an IPC request or callback.
  WorkerExit,        ///< Observing an unexpected process exit.
};

/// @brief Structured failure details for an event or a diagnostic snapshot.
struct PluginFailure {
  /// Stage at which the failure was observed.
  PluginFailureStage stage;
  /// Stable category for host decisions.
  ErrorCode code;
  /// UTF-8 diagnostic summary, at most 1 KiB.
  std::string message;
  /// Native exit code when the process exited with a status and one was available.
  std::optional<std::uint32_t> exit_code;
  /// Native signal when the process was terminated by a signal and one was available.
  std::optional<int> signal;
};

/// @brief Owned snapshot of one catalog or runtime change.
struct PluginEvent {
  /// The reason for the notification.
  PluginEventKind kind;
  /// Increasing event sequence within this manager session.
  std::uint64_t sequence;
  /// Wall clock time when the library recorded the event.
  std::chrono::system_clock::time_point time;
  /// Affected snapshot. Upgrade events identify the replacement and uninstall identifies the removed package.
  PluginIdentity package;
  /// Manager-local activation number, when the event refers to a runtime generation.
  std::optional<std::uint64_t> generation;
  /// Process ID of the affected worker, including after exit, when one was started.
  std::optional<std::uint32_t> worker_pid;
  /// Committed catalog action for CatalogChanged.
  std::optional<PluginCatalogChange> change;
  /// Retirement reason for Stopped.
  std::optional<PluginStopReason> stop_reason;
  /// Failure details when the event records a management or runtime failure.
  std::optional<PluginFailure> failure;
};

/// @brief Installed and runtime state of a plugin, with its latest failure in this manager session.
struct PluginDiagnostics {
  /// Current installed package, enablement, mode, available worker PID, and any pending upgrade.
  PluginInfo plugin;
  /// Current runtime state of the installed package.
  PluginRuntimeState state = PluginRuntimeState::Inactive;
  /// Generation of the activating, active, or stopping runtime, when present.
  std::optional<std::uint64_t> generation;
  /// True when a new host process is needed to load the currently installed in-process snapshot.
  bool restart_required = false;
  /// Most recent recorded failure, which may refer to a previous generation or a rejected upgrade candidate.
  std::optional<PluginEvent> last_failure;
};

namespace detail {
struct PluginEventSubscriber;
}

/// @brief Move-only subscription to plugin manager events.
///
/// Keep the handle on the owning application thread. Its destruction cancels pending deliveries to
/// this subscriber. A callback already running can return normally, including when it releases its
/// own subscription. A handle can outlive the manager.
class PluginEventSubscription {
public:
  /// Creates an empty subscription.
  PluginEventSubscription() noexcept = default;
  /// Transfers the subscription and leaves the original handle empty.
  PluginEventSubscription(PluginEventSubscription&&) noexcept = default;
  /// Releases the current subscription and takes ownership of the supplied subscription.
  PluginEventSubscription& operator=(PluginEventSubscription&& other) noexcept;
  PluginEventSubscription(const PluginEventSubscription&) = delete;
  PluginEventSubscription& operator=(const PluginEventSubscription&) = delete;
  /// Cancels delivery to this subscriber.
  ~PluginEventSubscription();

private:
  friend class PluginManager;
  explicit PluginEventSubscription(std::weak_ptr<detail::PluginEventSubscriber> subscriber);
  void Release() noexcept;
  std::weak_ptr<detail::PluginEventSubscriber> subscriber_;
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
///                                                const huxerui::pluginkit::ExtensionInfo& candidate,
///                                                std::shared_ptr<csv_import::ImportProgress> progress,
///                                                huxerui::AsyncInputStream input) {
///   auto importer = plugins.CreateExtension(csv_import::Importers, candidate);
///   co_return co_await importer->Import(std::move(input), std::move(progress));
/// }
/// @endcode
/// @see ExtensionPoint, ExtensionHandle
class PluginManager {
public:
  /// @brief Creates or opens the profile and takes its exclusive lock.
  /// Reopening the profile restores installed packages, approvals, enablement, and saved settings.
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

  /// @brief Inspects a package before any plugin code runs.
  /// @param directory Directory containing plugin.json, the platform's plugin library, and an optional resources
  /// directory generated by HuxerUI.
  /// @return Package metadata and the digest of the exact bytes to approve.
  /// @throws Error With InvalidPackage for invalid manifest metadata, an empty library, an invalid resource tree,
  /// or an unresolved setting label. Io reports unreadable files. PayloadTooLarge reports a manifest larger than
  /// 64 KiB or a library larger than 64 MiB.
  static PackageInfo InspectPackage(const File& directory);

  /// @brief Installs the approved package snapshot with its extension factories and optional HuxerUI resources.
  ///
  /// A new installation starts enabled. The mode and service grants apply to future activations of this snapshot.
  /// Every declared extension point must be registered and support the selected mode. The loader checks the
  /// library's factory contracts against those registrations before activation.
  /// Reinstalling with the same digest and approval can repair the stored files and preserves enablement.
  /// @param directory Source directory containing the package files.
  /// @param approved_digest Digest from InspectPackage() that the caller approved.
  /// @param options Execution mode and host services included in the approval.
  /// @throws Error With ApprovalMismatch if the inspected or staged digest differs, ContractMismatch for an
  /// unregistered point or a different in-process SDK identity, or UnsupportedMode if the selected mode is unavailable.
  /// @throws Error With InvalidArgument if a service grant is invalid or unavailable in the selected mode.
  /// @throws Error With AlreadyInstalled if the installed digest or approval differs, or Busy if the active
  /// plugin's snapshot needs repair. Disable the plugin before repairing it.
  /// @throws Error With Closed after shutdown or ReentrantCall while a host callback is active on this thread.
  /// @see InspectPackage(), ProvideService()
  void InstallPackage(const File& directory, std::string_view approved_digest, InstallOptions options = {});

  /// @brief Replaces an installed plugin with an approved package from a local directory.
  ///
  /// Use the same plugin identifier, a different release version, and the installed execution mode.
  /// The current generation is retired before validating an enabled candidate on a copy of the plugin data.
  /// Validation runs OnActivate once and constructs an instance from each factory. The package, grants,
  /// data, and compatible setting overrides are selected together at commit. Disabled plugins stay disabled and
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
  /// can remain mapped until host exit. The default policy preserves data and saved settings for a later install.
  /// @param plugin_id Installed plugin identifier.
  /// @param expected_digest Digest of the installed snapshot being removed.
  /// @param data Policy for the plugin's current data directory and saved settings.
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
  /// @param enabled True to make all of the package's factories available, false to disable them.
  /// @return Lazy task that completes after persisting the change and finishing any teardown.
  /// @throws Error With NotFound if the plugin is absent, Closed after shutdown, or ReentrantCall from a host callback.
  Task<void> SetEnabled(std::string_view plugin_id, bool enabled);

  /// @brief Reads the installed catalog without activating plugins.
  /// @return A snapshot of installed packages, enablement flags, and available worker process IDs.
  /// @see ListExtensions()
  std::vector<PluginInfo> ListPlugins() const;

  /// @brief Reads the installed settings and resolves their display text from the plugin package.
  ///
  /// Use the current UI locale when building an editor. The plugin can be disabled or inactive. This query
  /// uses the committed profile, can read local resource files, and does not start or call the plugin.
  /// @param plugin_id Installed plugin identifier.
  /// @param locale Locale for titles, descriptions, and choice labels. Defaults to Locale::Default(). Missing
  /// translations use SDK fallback.
  /// @return The schema, committed values, and revision of the current activation, if active.
  /// @throws Error With NotFound if the plugin is absent, Closed after shutdown, or InvalidPackage if
  /// a resource reference cannot be resolved from the installed package.
  /// @see SetSettingsAsync(), PluginContext::Settings()
  SettingsSnapshot GetSettings(std::string_view plugin_id, const Locale& locale = Locale::Default()) const;

  /// @brief Validates and atomically replaces a plugin's saved setting overrides.
  ///
  /// Pass the full override map, with any reset keys omitted. A successful change publishes SettingsChanged.
  /// Saving is available while the plugin is disabled or a restart upgrade is staged. Active plugin instances
  /// continue using the snapshot captured at activation. To apply saved values, disable and re-enable the plugin
  /// or reopen the profile in a new host session.
  /// @param plugin_id Installed plugin identifier.
  /// @param expected_revision Revision returned by GetSettings() for the snapshot being edited.
  /// @param overrides Complete set of explicit values to persist.
  /// @return Lazy Task that completes after the profile commits. Once admitted, the save finishes if the wait is cancelled.
  /// @throws SettingsValidationError If any override violates the committed schema. The entire edit is rejected.
  /// @throws Error With SettingsChanged for a stale revision, Busy while applying an upgrade or uninstalling,
  /// NotFound for an absent plugin, Closed after shutdown, ReentrantCall from a host callback, Io on a failed
  /// profile write, or PayloadTooLarge if the edit would exceed the profile byte limit.
  Task<void> SetSettingsAsync(std::string_view plugin_id, std::string_view expected_revision, SettingValues overrides);

  /// @brief Queues future catalog and runtime notifications on the manager's application thread.
  ///
  /// Call on the owning application thread. Callbacks run serially outside internal locks. They can
  /// query the current catalog and diagnostics. The event reference is valid during the callback.
  /// Take an initial snapshot separately. Events recorded before subscription are not replayed.
  /// @param callback A nonempty handler for each new event. An exception from a handler ends that delivery.
  /// @return A subscription that cancels delivery when released. Shutdown cancels all pending deliveries.
  /// @throws Error With InvalidArgument for an empty handler or Closed after shutdown begins.
  /// @see Diagnostics(), ListPlugins(), ListExtensions()
  PluginEventSubscription SubscribeEvents(std::function<void(const PluginEvent&)> callback);

  /// @brief Reads the installed and runtime state, including the most recent recorded failure.
  ///
  /// The failure survives successful recovery until replaced, uninstall, or manager shutdown. It is kept
  /// in memory for the current manager session. This query does not activate the plugin or enter its IPC queue.
  /// @param plugin_id The installed package identifier.
  /// @return An owned snapshot, or std::nullopt if the plugin is not installed.
  /// @throws Error With Closed after shutdown begins.
  std::optional<PluginDiagnostics> Diagnostics(std::string_view plugin_id) const;

  /// @brief Lists candidates declared at a registered extension point without activating packages.
  ///
  /// Discovery returns the optional JSON text of each candidate's metadata. The loader checks the complete
  /// factory table against the host contracts when the package is first used.
  /// @tparam Interface Published interface with a Contract specialization.
  /// @param point Previously registered extension point.
  /// @return Candidates in plugin identifier order and manifest order within each available package.
  /// @throws Error With NotFound for an unregistered point or ContractMismatch for a different contract.
  /// @see ListPlugins()
  template<class Interface>
  std::vector<ExtensionInfo> ListExtensions(ExtensionPoint<Interface> point) const {
    return List(point.id, Contract<Interface>::Info);
  }

  /// @brief Synchronously creates a handle to a factory declared by the installed package snapshot.
  ///
  /// With generated bindings, instance creation is deferred until the first synchronous method call or
  /// until the first Task method is awaited. For an unmarked in-process interface, this function creates
  /// the instance immediately so operator->() can expose the implementation directly.
  /// Before activation, the loader checks every exported factory against the registered extension points.
  /// @tparam Interface Interface with contract metadata.
  /// @param point Previously registered extension point.
  /// @param candidate Factory selected from package inspection or a catalog query for this point.
  /// @param timeout Time budget for each managed call, from 1 ms through 24 hours, including time in the queue.
  /// An unmarked in-process interface runs under the caller's ordinary C++ lifetime and timing rules.
  /// @return A move-only handle exposing Interface through operator->.
  /// @throws Error With PackageChanged if the package digest differs, or NotFound, Disabled, or ContractMismatch
  /// if the factory is unavailable for this point. Also reports Busy during a package change,
  /// InvalidArgument for an invalid timeout, Closed after shutdown, or UnsupportedMode if a worker is unavailable
  /// for the approved mode. Unmarked interfaces can also report activation or construction failures here.
  template<class Interface>
  ExtensionHandle<Interface> CreateExtension(ExtensionPoint<Interface> point, const ExtensionInfo& candidate,
                                             std::chrono::milliseconds timeout = DefaultTimeout) {
    return ExtensionHandle<Interface>(Create(point.id, Contract<Interface>::Info, candidate.PluginId(),
                                             &candidate, timeout));
  }

  /// @brief Synchronously creates a handle to the sole factory from a plugin at this extension point.
  /// @tparam Interface Published interface with contract metadata.
  /// @param point Previously registered extension point.
  /// @param plugin_id Installed plugin that provides the factory.
  /// @param timeout Time budget for each managed call, including time in the queue.
  /// @return Move-only handle exposing Interface through operator->.
  /// @throws Error With Ambiguous if the enabled plugin provides several factories at this point. Use ListExtensions()
  /// to select one or CreateExtensions() to obtain them all. Other failures follow the candidate overload.
  template<class Interface>
  ExtensionHandle<Interface> CreateExtension(ExtensionPoint<Interface> point, std::string_view plugin_id,
                                             std::chrono::milliseconds timeout = DefaultTimeout) {
    return ExtensionHandle<Interface>(Create(point.id, Contract<Interface>::Info, plugin_id, nullptr, timeout));
  }

  /// @brief Synchronously creates one handle per available factory at a registered extension point.
  /// @tparam Interface Published interface with contract metadata.
  /// @param point Previously registered extension point.
  /// @param plugin_id Optional installed plugin identifier to filter by.
  /// @param timeout Time budget for each managed call, including time in the queue.
  /// @return Handles in plugin identifier order and manifest order within each package. An empty match
  /// returns an empty vector. Instance creation follows the same rules as CreateExtension().
  /// @throws Error If querying candidates or creating any handle fails. Already created handles are released.
  template<class Interface>
  std::vector<ExtensionHandle<Interface>> CreateExtensions(ExtensionPoint<Interface> point,
      std::optional<std::string_view> plugin_id = std::nullopt, std::chrono::milliseconds timeout = DefaultTimeout) {
    const auto candidates = List(point.id, Contract<Interface>::Info, plugin_id);
    std::vector<ExtensionHandle<Interface>> handles;
    handles.reserve(candidates.size());
    for (const auto& candidate : candidates) handles.push_back(CreateExtension(point, candidate, timeout));
    return handles;
  }

private:
  void Register(std::string_view point, ContractInfo contract, bool remote);
  void Provide(ContractInfo contract, std::shared_ptr<void> implementation, detail::ServiceDispatch dispatch);
  std::vector<ExtensionInfo> List(std::string_view point, ContractInfo contract,
                                  std::optional<std::string_view> plugin_id = std::nullopt) const;
  struct Impl;
  static Task<void> ShutdownManager(std::shared_ptr<Impl> impl);
  std::shared_ptr<detail::ExtensionTarget> Create(std::string_view point, ContractInfo contract,
      std::string_view plugin_id, const ExtensionInfo* candidate, std::chrono::milliseconds timeout);

  // A pending task can retain the implementation until its transport work finishes.
  std::shared_ptr<Impl> impl_;
};

} // namespace huxerui::pluginkit
