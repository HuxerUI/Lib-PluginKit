# Lib-PluginKit Design

This document specifies the intended API and behavior of Lib-PluginKit. It is a C++20 library for applications that publish typed extension interfaces and accept independently built plugins. The HuxerUI project exposes `HuxerUI::PluginKit`. The application owns the plugin manager and its lifetime.

## Responsibilities

| Role | Responsibility |
| --- | --- |
| Application | Define ordinary C++ interfaces and extension points. Supply host services, trusted sources, authorization decisions, and an executor for callbacks. |
| Plugin author | Implement the published interfaces and declare the plugin, its contributions, its dependencies, and the capabilities it needs. |
| Lib-PluginKit | Generate SDK bindings, verify packages, resolve dependencies, manage installed versions and settings, dispatch typed calls, and recover from failures. |

The developer CLI is `hpk` (HuxerUI Plugin Kit). The target application can ship `hpk-worker` for out-of-process execution. These tools belong to the plugin SDK distribution.

## Contracts and generated SDK

The application declares a stable contract ID and version for a C++ interface in its build configuration. The interface uses ordinary C++ signatures and carries `HPK_REMOTE` when it supports worker execution.

```cpp
#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>
#include <string>

struct ImportRequest {
  std::string source;
};

struct ImportResult {
  std::string plain_text;
};

class HPK_REMOTE DocumentImporter {
public:
  virtual ~DocumentImporter() = default;
  virtual std::string Format() const = 0;
  virtual huxerui::Task<ImportResult> Import(ImportRequest request) = 0;
};
```

`hpk gen` parses the marked interface with Clang using its compilation options. It generates the schema, typed forwarding methods, and worker dispatch. The published SDK includes the original declarations and generated files. A plugin author implements the interface and exports its factory with `HPK_EXPORT_PLUGIN`. Unsupported declarations receive source-located diagnostics. [Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html)

Portable signatures use fixed-width scalars, enums, UTF-8 strings, byte sequences, lists, optional values, records composed of those types, and Kit-managed handles. A const reference parameter is encoded as a value and has call-scoped lifetime in the implementation. The generated binding constructs and destroys C++ objects on their owning side. Host services use the same contract model in the reverse direction. [Windows DLL memory ownership guidance](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)

A plugin shared library exports `pluginkit_query_v1`. Its descriptor provides the published interface pointer, paired creation and destruction callbacks, and worker dispatch for remote-capable contracts. An in-process host and the plugin use matching C++ and HuxerUI SDK builds. The library that creates the instance also destroys it.

The worker protocol uses numbered operations and CBOR values. The in-process path passes typed values to the actual virtual method. Both paths validate the exact published contract fingerprint. [CBOR specification](https://www.rfc-editor.org/rfc/rfc8949.html)

The Kit ABI and protocol, application contract version, and plugin release version are independent. The current ABI and protocol use v1. Applications rebuild the host, worker, and plugins together against the published SDK.

## Domain model and public API

| Name | Meaning |
| --- | --- |
| `PluginId` | Stable logical identity of a plugin across releases. |
| `PluginPackage` and `PluginManifest` | The installable artifacts and the declarations that describe them. |
| `PluginInfo` | A catalog snapshot containing the installed package, any pending upgrade, enabled state, and execution status. |
| `Contract` | The versioned callable schema derived from an interface. |
| `ExtensionPoint<T>` | A named application extension point for a contract. Several points can use the same interface with different metadata and selection rules. |
| `ExtensionId` | The plugin ID and a stable contribution ID within that plugin. |
| `ExtensionInfo<T>` | Catalog metadata for one contribution, available before the implementation starts. |
| `ExtensionHandle<T>` | A typed, revocable handle to an opened extension instance. |
| `PluginContext` | Shared activation context containing the approved plugin identity, installed bundle directory, and writable data directory. |
| `Task<T>` | Lazy operation whose coroutine runs on its owning thread. |
| `Receiver<T>` | Typed proxy for a caller-owned callback interface, scoped to one invocation. |

The application owns one `huxerui::pluginkit::PluginManager` for each plugin profile it manages. `PluginManagerOptions` supplies the profile directory, trusted sources, execution policy, and callback executor. Registration of application extension points and host services completes before startup.

| Responsibility | API | Behavior |
| --- | --- | --- |
| Configuration | `RegisterExtensionPoint(point)`, `ProvideService<T>()` | Declare typed extension points and register one implementation per host service. |
| Manager lifetime | `PluginManager`, `ApplyPendingChanges()`, `Shutdown()` | Recover the profile at construction, apply pending upgrades after registration, and release runtime resources on shutdown. |
| Discovery | `RefreshCatalogAsync()`, `ListPlugins()`, `ListExtensions(point)` | Refresh configured sources and read catalog snapshots. Listing contributions uses manifest metadata. |
| Invocation | `CreateExtension(point, id)` | Synchronously return a typed handle. The first managed method creates the instance. |
| Local package changes | `InstallPackage()`, `SetEnabled()`, `UpgradePackage()`, `UninstallPackage()` | Install an approved snapshot, persist enablement, upgrade to another local release, and uninstall with a data retention policy. |
| Settings | `GetSettings(plugin_id)`, `SetSettingsAsync(plugin_id, values)` | Read a snapshot and validate or persist changes against the plugin settings schema. |
| Observability | `SubscribeEvents()`, `Diagnostics()` | Report catalog revisions, state transitions, operation progress, and structured failures. Subscription handles release their callbacks. |

Explicit IDs and stored user preferences take precedence over automatic matching. A unique eligible match can be opened directly. An ambiguous query returns the candidates for selection. Disabled contributions leave the available extension list while their package and diagnostic records remain visible in management views.

`ExtensionHandle<DocumentImporter>` exposes the published interface through `operator->`. The host calls `importer->Format()` or awaits `importer->Import(request)` using the original signatures. Scope exit releases the handle and retires the instance after active work leaves it.

A plugin activation owns a single `PluginContext`. The SDK passes it by const reference to optional `OnActivate` and `OnDeactivate` hooks and to each extension implementation that accepts it in its constructor. `OnActivate` runs before the first instance is created. Orderly teardown destroys the instances before calling `OnDeactivate`. The bundle directory identifies the approved snapshot and the writable data directory is selected by the committed profile. Both are exposed as `huxerui::File`.

The host records service grants and execution mode with the approved package digest. During a call, `GetService<T>()` synchronously returns a typed handle. The service implementation resolves the grant from the invocation. The caller supplies a `Receiver<T>` as a shared C++ callback argument, with access scoped to that invocation. See the [CSV example](../README.md#sdk-and-runtime).

The broader lifecycle design includes settings, read-only bundle resources, logging, and lifetime cancellation. Kit tracks the tasks, subscriptions, streams, and grants acquired through the lifecycle API.

## Packages, discovery, and resolution

A package contains `plugin.json`, target artifacts, and resources. Its signed metadata binds the publisher, manifest, and file inventory to their digests. The manifest identifies the plugin and release, its target variants, provided contract revisions, required plugin versions and service contracts, contributions, requested capabilities, and settings schema. The generated descriptors carry the corresponding contract fingerprints.

An author manifest can describe a Markdown importer as follows.

```json
{
  "schema_version": 1,
  "id": "example.markdown",
  "version": "1.4.0",
  "targets": [
    {
      "os": "windows",
      "arch": "x86_64",
      "abi": "msvc",
      "entry": "bin/windows/x86_64/markdown.dll"
    }
  ],
  "provides": [
    {
      "contract": "richeditor.document-importer",
      "versions": ["1.0", "1.1"]
    }
  ],
  "contributes": {
    "extensions": [
      {
        "id": "markdown-importer",
        "point": "richeditor.importers",
        "contract": "richeditor.document-importer",
        "metadata": { "extensions": [".md"] }
      }
    ]
  },
  "settings_schema": "settings.json"
}
```

`hpk pack` adds the generated schema fingerprints and signed inventory for the published artifact. Each selected target resolves to a verified entry inside the package. Published content is immutable for a release and target. The manager validates archive boundaries and file sizes before installing content into its managed store.

Discovery reads manifests from explicit local sources and configured repositories. Package selection checks the operating system, architecture, artifact ABI, supported contracts, execution policy, and permissions before activation. The loader checks the plugin descriptor against the verified package. A contribution can advertise a command or file type before its implementation runs.

The resolver selects one release per plugin ID in a profile. It honors version pins and required version ranges, verifies required host services, rejects required dependency cycles, and reports the dependency chain for conflicts. Optional dependencies become available features when a compatible provider exists. Required dependencies activate before dependents, and teardown follows the reverse order. The committed resolution is persisted for reproducible startup.

The manager verifies local packages against application trust policy. Remote sources use authenticated update metadata with expiry and rollback checks. Installed and previously verified packages can remain available offline under the active policy. [The Update Framework specification](https://theupdateframework.github.io/specification/latest/)

## Execution, permissions, and instance lifetime

One desktop profile can contain packages installed for either `InProcess` or `WorkerProcess`. The application chooses the mode during installation, and the package declares its supported modes.

| Backend | Policy and lifetime |
| --- | --- |
| `InProcess` | The host loads an approved library into its own process and calls its C++ interface directly. The library stays mapped until host exit. |
| `WorkerProcess` | Each active generation has its own `hpk-worker` with a private IPC channel. The worker starts on demand and loads the selected library. |

Process separation contains a plugin process failure. The application selects a sandbox policy for filesystem, network, and other operating system access. Kit checks declared and granted capabilities when dispatching host services and uses an eligible sandboxed backend where policy requires one. Scoped file and object grants carry the plugin identity, permitted operations, and lifetime. Package trust identifies what code is approved to run. The sandbox determines its access. [Windows AppContainer isolation](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)

The manager starts a plugin generation once, after resolving its dependencies and grants. Concurrent opens share that activation. The manager publishes callable bindings after successful initialization and releases provisional resources when initialization fails. Each open creates an extension instance in that generation. Calls on one instance are serialized by default. The transport drives I/O independently of business execution, and host callbacks run on the configured executor. Call options supply a deadline and cancellation, and the generated call context exposes progress and cancellation to the implementation. Large streams have bounded buffers and backpressure. [Cancellation](https://grpc.io/docs/guides/cancellation/) and [flow control](https://grpc.io/docs/guides/flow-control/)

A handle identifies a concrete instance and generation. Disabling or replacing a generation withdraws its contributions and closes admission to new calls immediately. The manager cancels queued work, drains or cancels work already in progress under its shutdown policy, and releases the instances. A successfully started generation receives its stop hook before the manager releases the context. An application can retain a closed handle and receive a closed result. It opens a fresh handle by extension ID after a version change.

Worker exit completes pending calls with a failure and marks the generation faulted. A request delivered before a crash can have an unknown business outcome. The application can retry a safe operation explicitly, and Kit preserves the distinction between an undelivered request and a potentially executed request. Restart policy is bounded and exposed in diagnostics. A deadline ends the caller's wait and requests cancellation. For a worker that exceeds its stop deadline, the policy can terminate the worker. In-process execution keeps its resources until the relevant calls finish or the process exits. [Retry safety guidance](https://google.aip.dev/194)

## Management transactions and persisted state

A profile records the selected local package, enabled flag, granted services, and data directory per installed plugin. One manager owns write access to that profile. Readers consume committed snapshots. Other writers receive a busy result and can use a separate profile or retry after the owner releases it.

`UpgradePackage` checks the installed and candidate digests and the complete approved grant list. The package keeps the plugin identifier, declares a different release version, and matches the registered contract and installed execution mode. `UninstallPackage` checks the installed digest and removes its catalog entry, grants, and pending upgrade. Current plugin data is retained by default and reused on reinstall. `PluginDataPolicy::Delete` records that the data can be removed after uninstall commits.

The manager stages the verified package and records when validation begins. It stops the old generation before copying the current data. The plugin owns data format migration in `OnActivate` and can write to the candidate copy. One atomic profile replacement selects the package, the grants, and the migrated data. Opening the profile discards interrupted candidates and retries cleanup of files discarded by a committed change. Host side effects and files outside the plugin data directory follow the application's contracts.

An enabled worker upgrade waits for the old worker to exit and validates the candidate in a new worker before committing. The host obtains new handles after commit. In-process upgrades wait for a new host process and are applied by `ApplyPendingChanges` after registration. If a failed candidate was mapped, the selected old version can be loaded in a subsequent process. Disabled packages keep their enablement state and activate on first use. The old package and data can be cleaned up once their references are released.

The broader management design can use `PlanChangesAsync` and `ApplyPlanAsync` for coordinated package and dependency changes against a profile revision. It includes versioned settings validated against a schema, provider preferences, and scoped grant revocation. Local upgrades commit explicit grants for the approved target digest.

## Toolchain and HuxerUI integration

| Tool | Output or purpose |
| --- | --- |
| `hpk gen` and `hpk check` | Generate the contract SDK and check ABI lock and version changes. |
| `hpk new` | Scaffold a plugin project from a published application SDK. |
| `hpk pack` and `hpk verify` | Package target artifacts and resources, sign the inventory, and verify the result. |
| `hpk index` | Produce repository metadata for publishing packages. |
| `hpk test` and `hpk doctor` | Exercise contract and lifecycle behavior. Explain compatibility, dependency, permission, and activation failures. |

CMake, mcpp, and xmake integrate through the same host tool and artifact specifications. Build integrations pass compilation options to `hpk` and use the executable supplied by the SDK. Generated SDKs are build artifacts published with the contract metadata. The target runtime consumes the generated bindings and ships the matching `hpk-worker` where required.

An optional HuxerUI adapter provides `PluginCenter(manager)`, schema-driven settings, and contribution mounting points for menus and commands. The UI uses the same plan and apply operations as other integrations. The application can pass the manager directly or register it as an application service through its existing hooks. Plugin contributions describe UI data, and the application builds the corresponding HuxerUI views.

## Acceptance scenarios

- Publish an editor SDK and install an independently built Markdown importer into an existing editor. The importer appears through manifest metadata, starts on first use, and supplies a typed implementation.
- Open a large document through a granted stream. Verify bounded buffering, cancellation, and delivery of progress without blocking the application UI.
- Crash or stall one worker during a call. Verify that unrelated plugins remain available, that the pending call reports a useful error, and that recovery can open a new generation.
- Disable an extension while an application retains its handle and another call is in flight. Verify that new calls are refused and that resources are released after draining.
- Exercise a compatible contract minor revision, an incompatible contract, a dependency conflict, a denied grant, and a changed package digest. Verify that each diagnostic identifies the failing condition.
- Interrupt a local upgrade and fail a candidate activation after it writes to its data copy. Verify that startup selects the last committed package, grants, and data and finishes pending file cleanup.
- Upgrade a worker in the running host and an in-process plugin after restart. Uninstall with each data policy and verify that reinstall only reuses retained data.
