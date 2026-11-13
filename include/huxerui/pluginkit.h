#pragma once

/// @file
/// @brief Host API for plugin profiles, package approval, and typed worker instances.
///
/// Link HuxerUI::PluginKit and supply the platform's hpk-worker executable when opening a profile.
/// Include the generated host header for each interface used with PluginManager.

#include <huxerui/pluginkit/detail/invocation.h>

#include <huxerui/file.h>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

namespace huxerui::pluginkit {

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
  /// Fingerprint declared in the manifest and checked against the host's registered contract.
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
  /// Identifier to pass to CreateExtension().
  ExtensionId id;
  /// Advertised file suffixes, including their leading dots.
  std::vector<std::string> extensions;
};

class PluginManager;

namespace detail {

class Worker;
using ServiceDispatch = std::function<Task<Bytes>(std::uint32_t, Bytes)>;

struct ReceiverRegistration {
  ContractInfo contract;
  ServiceDispatch dispatch;
};

template<class Interface>
ServiceDispatch BindCallback(std::shared_ptr<Interface> implementation) {
  if (!implementation) throw Error(ErrorCode::InvalidArgument, "The callback implementation is empty");
  return [implementation = std::move(implementation)](std::uint32_t operation, Bytes request) {
    return Contract<Interface>::Dispatch(*implementation, operation, std::move(request), {});
  };
}

template<class Interface>
ReceiverRegistration BindReceiver(std::shared_ptr<Interface> implementation) {
  return {Contract<Interface>::Info, BindCallback(std::move(implementation))};
}

/// @brief Owned invocation target captured before returning a lazy host task.
class ExtensionCall {
public:
  Task<Bytes> Invoke(std::uint32_t operation, Bytes request, std::chrono::milliseconds timeout,
                     std::optional<ReceiverRegistration> receiver = {}) const;

private:
  friend class ExtensionInstance;
  ExtensionCall(std::shared_ptr<Worker> worker, std::uint32_t instance, std::stop_token lifetime)
      : worker_(std::move(worker)), instance_(instance), lifetime_(lifetime) {}
  std::shared_ptr<Worker> worker_;
  std::uint32_t instance_;
  std::stop_token lifetime_;
};

template<class Result>
Task<Result> InvokeExtension(ExtensionCall call, std::uint32_t operation, Bytes request,
                             std::chrono::milliseconds timeout, std::optional<ReceiverRegistration> receiver = {}) {
  co_return DecodeResult<Result>(co_await call.Invoke(operation, std::move(request), timeout, std::move(receiver)));
}

/// @brief Owns a worker instance on behalf of a generated extension handle.
class ExtensionInstance {
public:
  ExtensionInstance(ExtensionInstance&& other) noexcept;
  ExtensionInstance& operator=(ExtensionInstance&& other) noexcept;
  ~ExtensionInstance();

  ExtensionCall Call() const;
  Task<void> Close();

private:
  friend class huxerui::pluginkit::PluginManager;
  ExtensionInstance(std::shared_ptr<Worker> worker, std::uint32_t instance, std::stop_source lifetime);
  void Release() noexcept;

  std::shared_ptr<Worker> worker_;
  std::uint32_t instance_;
  std::stop_source lifetime_;
};

} // namespace detail

/// @brief Owns a plugin profile and starts a worker on demand for each active plugin.
///
/// Register each extension point before installing or opening its contributions. Installed packages and enablement
/// persist in the profile. Register the points again when opening a new manager.
///
/// Each profile has a single owning manager. Coordinate its destruction with callers.
/// Catalog queries remain available during host service and receiver callbacks. Calls on one plugin worker
/// are serialized even when they use different extension handles.
/// Workers run under the host's user account and provide process fault isolation for trusted plugins.
///
/// The CSV example assumes the point is registered and its package is approved and installed.
/// @code{.cpp}
/// #include "csv_contract.host.h"
///
/// huxerui::Task<csv_import::ImportResult> ReadTable(huxerui::pluginkit::PluginManager& plugins,
///                                    const huxerui::pluginkit::ExtensionId& id,
///                                    std::shared_ptr<csv_import::ImportProgress> progress,
///                                    const std::string& csv) {
///   constexpr huxerui::pluginkit::ExtensionPoint<csv_import::TableImporter> importers{"example.table-importers"};
///   auto importer = co_await plugins.CreateExtension(importers, id);
///   co_return co_await importer.Import({csv}, std::move(progress));
/// }
/// @endcode
/// @see ExtensionPoint, ExtensionHandle
class PluginManager {
public:
  /// @brief Creates or opens the profile and takes its exclusive lock.
  /// @param profile_directory Directory for the catalog and approved package snapshots.
  /// @param worker_executable Path to the platform's hpk-worker executable. Workers start on first instance creation.
  /// @param callbacks Application task scope used to deliver reverse RPC and receiver callbacks.
  /// @throws Error With ProfileBusy if another manager holds the lock, or Io if the profile cannot be read.
  /// @throws std::filesystem::filesystem_error If path resolution or profile directory creation fails.
  PluginManager(const File& profile_directory, const File& worker_executable, TaskScope callbacks);

  PluginManager(const PluginManager&) = delete;
  PluginManager& operator=(const PluginManager&) = delete;

  /// @brief Shuts down active plugins and closes their extension handles.
  /// Await Shutdown() first to keep the application thread responsive during teardown.
  /// Pending tasks can retain the profile lock until their transport work finishes.
  ~PluginManager();

  /// @brief Stops worker admission and awaits bounded teardown on the transport thread.
  /// @return Lazy task to await before retiring the application task scope.
  /// @throws Error With ReentrantCall when awaited from a host callback.
  Task<void> Shutdown();

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
  static PackageInfo InspectPackage(const File& directory);

  /// @brief Provides a typed host service to one approved package.
  ///
  /// Register before activating the package. A worker keeps the services available at its startup.
  /// Service calls run in the manager's application task scope and may interleave for different workers.
  /// Keep handlers bounded. Use ListPlugins() or ListExtensions() for catalog reads in a handler.
  /// Other manager and worker operations are subject to reentry checks.
  /// Registrations last for this manager. Changes the handler applies remain in effect if the outer call fails.
  /// @tparam Interface Interface with generated contract bindings.
  /// @param implementation Service object retained by the manager and authorized worker generations.
  /// @param approved_digest Exact package digest authorized to use this service.
  /// @throws Error With InvalidArgument for an empty implementation, invalid metadata, or duplicate registration.
  template<class Interface>
  void ProvideService(std::shared_ptr<Interface> implementation, std::string_view approved_digest) {
    Provide(Contract<Interface>::Info, approved_digest, detail::BindCallback(std::move(implementation)));
  }

  /// @brief Installs the approved snapshot for a registered extension point.
  ///
  /// A new installation starts enabled. Reinstalling the same approved package preserves its enablement and
  /// can repair a changed or missing snapshot. A conflicting package under the same plugin ID is rejected.
  /// @param directory Source directory containing the package files.
  /// @param approved_digest Digest from InspectPackage() that the caller approved.
  /// @throws Error With ApprovalMismatch for a different digest, ContractMismatch for an unregistered
  /// or incompatible contract, or AlreadyInstalled for a conflicting package under the same ID.
  void InstallPackage(const File& directory, std::string_view approved_digest);

  /// @brief Persists enablement. Disabling a plugin stops new work on its active worker generation.
  /// Allows up to DefaultTimeout for the running exchange and plugin teardown before terminating the worker.
  /// @param plugin_id Installed plugin identifier.
  /// @param enabled True to make the contribution available for subsequent creation, false to disable it.
  /// @return Lazy task that completes after the change and any worker teardown.
  /// @throws Error With NotFound if the plugin identifier is absent from the profile.
  Task<void> SetEnabled(std::string_view plugin_id, bool enabled);

  /// @brief Reads the installed catalog without activating plugins.
  /// @return A snapshot of installed packages, enablement flags, and available worker process IDs.
  /// @see ListExtensions()
  std::vector<PluginInfo> ListPlugins() const;

  /// @brief Lists enabled contributions matching the point's exact registered contract.
  /// @tparam Interface Interface with a generated Contract specialization.
  /// @param point Previously registered extension point.
  /// @return Manifest metadata for matching contributions, including those whose workers have yet to start.
  /// @throws Error With NotFound for an unregistered point or ContractMismatch for a different contract.
  /// @see ListPlugins()
  template<class Interface>
  std::vector<ExtensionInfo> ListExtensions(ExtensionPoint<Interface> point) const {
    return List(point.id, Contract<Interface>::Info);
  }

  /// @brief Creates an extension instance, activating its plugin on first use.
  ///
  /// Starting a worker verifies the installed snapshot. The timeout starts with the worker request, after
  /// profile access, package verification, and process startup. Activation and creation share that deadline.
  /// @tparam Interface Interface with generated Contract and ExtensionHandle specializations.
  /// @param point Previously registered extension point.
  /// @param id Contribution to instantiate, as returned by ListExtensions().
  /// @param timeout Time budget for the worker request, from 1 ms through 24 hours.
  /// @return A move-only handle to the newly created instance.
  /// @throws Error With NotFound, Disabled, or ContractMismatch if the contribution is unavailable for this point.
  /// @throws Error With PackageChanged if the installed snapshot fails verification, DeadlineExceeded if the
  /// worker request expires, WorkerExited if the worker exits, or PluginFailed if activation or creation fails.
  template<class Interface>
  Task<ExtensionHandle<Interface>> CreateExtension(ExtensionPoint<Interface> point, ExtensionId id,
                                                    std::chrono::milliseconds timeout = DefaultTimeout) {
    return CreateTyped<Interface>(impl_, std::string(point.id), std::move(id), timeout);
  }

private:
  void Provide(ContractInfo contract, std::string_view approved_digest, detail::ServiceDispatch dispatch);
  void Register(std::string_view point, ContractInfo contract);
  std::vector<ExtensionInfo> List(std::string_view point, ContractInfo contract) const;
  struct Impl;
  static Task<void> ShutdownManager(std::shared_ptr<Impl> impl);
  static Task<void> ChangeEnabled(std::shared_ptr<Impl> impl, std::string id, bool enabled);
  static detail::ExtensionInstance CreateWorkerInstance(Impl& impl, std::string_view point, ContractInfo contract,
                                                        const ExtensionId& id, std::chrono::milliseconds timeout,
                                                        std::stop_token stop);
  static Task<detail::ExtensionInstance> Create(std::shared_ptr<Impl> impl, std::string point, ContractInfo contract,
                                               ExtensionId id, std::chrono::milliseconds timeout);

  template<class Interface>
  static Task<ExtensionHandle<Interface>> CreateTyped(std::shared_ptr<Impl> impl, std::string point, ExtensionId id,
                                                      std::chrono::milliseconds timeout) {
    co_return ExtensionHandle<Interface>(
        co_await Create(std::move(impl), std::move(point), Contract<Interface>::Info, std::move(id), timeout));
  }

  // A pending task can retain the implementation until its transport work finishes.
  std::shared_ptr<Impl> impl_;
};

} // namespace huxerui::pluginkit
