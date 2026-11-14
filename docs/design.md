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

`hpk gen` parses the marked interface with Clang using its compilation options. It generates the schema, typed forwarding methods, and worker dispatch. The published SDK includes the original declarations and generated files. A plugin author implements the interfaces, binds each factory to a point with `BindExtension`, and exports the table with `HPK_EXPORT_PLUGIN`. Unsupported declarations receive source-located diagnostics. [Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html)

Portable signatures use fixed-width scalars, enums, UTF-8 strings, byte sequences, lists, optional values, records composed of those types, and Kit-managed handles. A const reference parameter is encoded as a value and has call-scoped lifetime in the implementation. The generated binding constructs and destroys C++ objects on their owning side. Host services use the same contract model in the reverse direction. [Windows DLL memory ownership guidance](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)

A plugin shared library exports `pluginkit_query_v1`. Its descriptor supplies package lifecycle callbacks and one factory entry for each manifest extension. Each entry names its point and exact contract and supplies callbacks to create and destroy the typed interface and dispatch worker calls. An in-process host and the plugin use matching C++ and HuxerUI SDK builds. The library that creates the instance also destroys it.

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
| `ExtensionInfo` | A factory declaration tied to an extension point and a package digest, returned by both package inspection and discovery. |
| `ExtensionHandle<T>` | A typed, revocable handle to an opened extension instance. |
| `PluginContext` | Shared activation context containing the approved plugin identity, installed bundle directory, optional HuxerUI resources, and writable data directory. |
| `Task<T>` | Lazy operation whose coroutine runs on its owning thread. |
| `Receiver<T>` | Typed proxy for a caller-owned callback interface, scoped to one invocation. |

The application owns one `huxerui::pluginkit::PluginManager` for each plugin profile it manages. `PluginManagerOptions` supplies the profile directory, trusted sources, execution policy, and callback executor. Registration of application extension points and host services completes before startup.

| Responsibility | API | Behavior |
| --- | --- | --- |
| Configuration | `RegisterExtensionPoint(point)`, `ProvideService<T>()` | Declare typed extension points and register one implementation per host service. |
| Manager lifetime | `PluginManager`, `ApplyPendingChanges()`, `Shutdown()` | Recover the profile at construction, apply pending upgrades after registration, and release runtime resources on shutdown. |
| Discovery | `RefreshCatalogAsync()`, `ListPlugins()`, `ListExtensions(point)` | Refresh configured sources and read catalog snapshots. Listing contributions uses manifest metadata. |
| Invocation | `CreateExtension(point, candidate)`, `CreateExtension(point, plugin_id)`, `CreateExtensions(point, [plugin_id])` | Synchronously return typed handles. The first managed method creates its instance. |
| Local package changes | `InstallPackage()`, `SetEnabled()`, `UpgradePackage()`, `UninstallPackage()` | Install an approved snapshot, persist enablement, upgrade to another local release, and uninstall with a data retention policy. |
| Settings | `GetSettings(plugin_id)`, `SetSettingsAsync(plugin_id, values)` | Read a snapshot and validate or persist changes against the plugin settings schema. |
| Observability | `SubscribeEvents()`, `Diagnostics()` | Report catalog revisions, state transitions, operation progress, and structured failures. Subscription handles release their callbacks. |

`ListExtensions` returns candidates with their plugin ID and optional JSON object text in `metadata_json`. The host can parse the text using its own metadata conventions to pick a candidate and pass it to `CreateExtension`. A plugin ID selects a factory when it is the only one at that point and yields `Ambiguous` when several match. `CreateExtensions` returns one handle per available factory, ordered by plugin ID and then by declaration order within each package. Disabled packages disappear from extension discovery while their package records remain visible. After an upgrade, a candidate for the old digest yields `PackageChanged`.

`ExtensionHandle<DocumentImporter>` exposes the published interface through `operator->`. The host calls `importer->Format()` or awaits `importer->Import(request)` using the original signatures. Scope exit releases the handle and retires the instance after active work leaves it.

A package activation owns a single `PluginContext` for all factories and their instances. The SDK passes it by const reference to optional `OnActivate` and `OnDeactivate` hooks and to each extension implementation that accepts it in its constructor. `OnActivate` runs once after validating the complete factory table and before the first instance is created. Orderly teardown destroys the instances before calling `OnDeactivate`. The bundle directory identifies the approved snapshot and the writable data directory is selected by the committed profile. Both are exposed as `huxerui::File`.

The activation opens the installed `resources/` directory once with `huxerui::Resources::OpenPackage()`, before `OnActivate()`. `PluginContext::Resources()` exposes this independent package in both execution modes. Its owner survives through `OnDeactivate()`. Resource indices, keys, lazy payload reads, and resolved asset lifetimes follow the HuxerUI SDK.

The host records service grants and execution mode with the approved package digest. During a call, `GetService<T>()` synchronously returns a typed handle. The service implementation resolves the grant from the invocation. The caller supplies a `Receiver<T>` as a shared C++ callback argument, with access scoped to that invocation. See the [CSV example](../README.md#sdk-and-runtime).

The broader lifecycle design includes settings, logging, and lifetime cancellation. Kit tracks the tasks, subscriptions, streams, and grants acquired through the lifecycle API.

## Packages, discovery, and resolution

A local directory package contains `plugin.json`, one library for the selected target, and optionally the HuxerUI resource compiler's output under `resources/`. The manifest identifies the plugin and release, available execution modes, C++ ABI identity, and extension factories. The manager enumerates the resource files and hashes their relative paths and exact contents alongside the manifest and library. Installation verifies those files in a staging directory before publishing the approved snapshot. Archive extraction belongs to the caller. The broader package design includes publisher signatures, target variants, dependencies, requested capabilities, and settings schemas.

The CSV example publishes two importers and one exporter in the same package.

```json
{
  "schema_version": 1,
  "id": "example.csv",
  "version": "0.1.0",
  "target": "windows-x86_64-msvc",
  "execution_modes": ["in_process", "worker_process"],
  "cpp_abi": "release-0.4.0:msvc=145:iterators=0:dynamic-crt",
  "extensions": [
    {"point": "example.table-importers", "metadata": {"extensions": [".csv"]}},
    {"point": "example.table-importers", "metadata": {"extensions": [".ssv"]}},
    {"point": "example.table-exporters", "metadata": {"extensions": [".csv"]}}
  ]
}
```

The manifest parser assigns each factory a zero-based ordinal within its point. Discovery, instance creation, and factory validation use the same point and ordinal. The manifest may interleave points differently from the export table. A host can select an inspected entry once the exact package is installed and enabled. Optional metadata must be a JSON object. Package inspection and candidate discovery expose its UTF-8 JSON text and preserve its values after the profile is reopened. An omitted object yields no text. An empty object yields `"{}"`. The host parses the text and defines the meanings of its keys, such as the CSV preview's file suffix array.

At installation the manager checks that every point is registered and supports the selected mode. Before running `OnActivate`, the loader checks the full table of declared factories against the plugin's exports and compares each exported contract ID, version, and fingerprint with the host point's registered contract. The SDK generates this identity for remote interfaces.

Published content is immutable for a release and target. The manager checks manifest and library sizes before installing content into its managed store. The planned `hpk pack` tool supports target selection and publisher signatures.

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

The manager starts one activation per package generation. All instances of all points in a worker package share the same process and context. Managed calls are serialized across the package. The transport drives I/O independently of business execution, and host callbacks run on the owning application scope. Call options supply a deadline and cancellation. The broader design includes large streams with bounded buffers and backpressure. [Cancellation](https://grpc.io/docs/guides/cancellation/) and [flow control](https://grpc.io/docs/guides/flow-control/)

The worker receives the package context and expected factory table in `Activate`. `Create` sends the point and its ordinal and returns an instance ID. The worker stores that instance with its factory so `Invoke` uses the corresponding contract's operation table. `Close` destroys one instance. Package shutdown closes all remaining instances before invoking `OnDeactivate`.

A handle is scoped to a factory and generation. Disabling or replacing a package withdraws all its factories and closes admission to new calls. The manager drains or cancels work under its shutdown policy and releases the instances. An application can retain a closed handle and receive a `Closed` result. It queries the current candidates or creates a new handle by plugin ID after a version change.

Worker exit completes pending calls with a failure and marks the generation faulted. A request delivered before a crash can have an unknown business outcome. The application can retry a safe operation explicitly, and Kit preserves the distinction between an undelivered request and a potentially executed request. Restart policy is bounded and exposed in diagnostics. A deadline ends the caller's wait and requests cancellation. For a worker that exceeds its stop deadline, the policy can terminate the worker. In-process execution keeps its resources until the relevant calls finish or the process exits. [Retry safety guidance](https://google.aip.dev/194)

## Management transactions and persisted state

A profile records the selected local package, enabled flag, granted services, and data directory per installed plugin. A new host process reopens the profile and uses the managed snapshot without reinstalling. Register its current extension points and service objects before using the installed plugins. One manager owns write access to that profile. Readers consume committed snapshots. Other writers receive a busy result and can use a separate profile or retry after the owner releases it.

`UpgradePackage` checks the installed and candidate digests and the complete approved grant list. The package keeps the plugin identifier, declares a different release version, and matches the registered points and installed execution mode. `UninstallPackage` checks the installed digest and removes its catalog entry, factories, grants, and pending upgrade. Current plugin data is retained by default and reused on reinstall. `PluginDataPolicy::Delete` records that the data can be removed after uninstall commits.

The manager stages the verified package and records when validation begins. It stops the old generation before copying the current data. The plugin owns data format migration in `OnActivate` and can write to the candidate copy. One atomic profile replacement selects the package, the grants, and the migrated data. Opening the profile discards interrupted candidates and retries cleanup of files discarded by a committed change. Host side effects and files outside the plugin data directory follow the application's contracts.

An enabled worker upgrade waits for the old worker to exit and validates the candidate in a new worker before committing. Validation runs `OnActivate` and creates and destroys a probe instance from every factory on the shared activation. The host obtains new handles after commit. In-process upgrades run the same factory checks when `ApplyPendingChanges` runs in a new host process after registration. If a failed candidate was mapped, the selected old version can be loaded in a subsequent process. Disabled packages keep their enablement state and activate on first use. The old package and data can be cleaned up once their references are released.

The broader management design can use `PlanChangesAsync` and `ApplyPlanAsync` for coordinated package and dependency changes against a profile revision. It includes versioned settings validated against a schema, provider preferences, and scoped grant revocation. Local upgrades commit explicit grants for the approved target digest.

## Toolchain and HuxerUI integration

| Tool | Output or purpose |
| --- | --- |
| `hpk gen` and `hpk check` | Generate the contract SDK and check ABI lock and version changes. |
| `hpk new` | Scaffold a plugin project from a published application SDK. |
| `hpk pack` and `hpk verify` | Stage and verify target artifacts and their HuxerUI resources for distribution. |
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
