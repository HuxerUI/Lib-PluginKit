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

The application declares a stable contract ID and version for a C++ interface in its build configuration. A contract header uses ordinary C++ syntax.

```cpp
#include <string>

struct ImportRequest {
  std::string source;
};

struct ImportResult {
  std::string plain_text;
};

class DocumentImporter {
public:
  virtual ~DocumentImporter() = default;
  virtual ImportResult Import(const ImportRequest& request) = 0;
};
```

`hpk gen` parses the selected interface with Clang using its compilation options. It generates the schema, the typed application-side call interface, the implementation binding, and the ABI lock. The published SDK includes the original declarations and generated files. A plugin author uses that SDK and binds a derived implementation with `Bind<Interface, Implementation>()`. Unsupported declarations receive source-located diagnostics. [Clang LibTooling](https://clang.llvm.org/docs/LibTooling.html)

Portable signatures use fixed-width scalars, enums, UTF-8 strings, byte sequences, lists, optional values, records composed of those types, and Kit-managed handles. A const reference parameter is encoded as a value and has call-scoped lifetime in the implementation. The generated binding constructs and destroys C++ objects on their owning side. Host services use the same contract model in the reverse direction. [Windows DLL memory ownership guidance](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries?view=msvc-170)

A plugin shared library exports the generated `pluginkit_query_v1` C entry point. It provides a versioned descriptor and the operations needed to create an instance, invoke an operation, and close the instance. The calling convention, struct sizes, buffer ownership, and descriptor version are part of the loader ABI. The binding catches C++ exceptions at its boundary and reports structured errors. Built-in plugins expose distinct registration entries referenced by the application. Both paths use the same contract and dispatch model.

Operation and record field numbers are stable protocol identifiers. The generated ABI lock records them together with the schema for published revisions. Messages use the same numbered schema across in-process, worker, and Wasm transports. CBOR carries encoded values. Ordinary DTOs are decoded locally, and large payloads use scoped streams. [CBOR specification](https://www.rfc-editor.org/rfc/rfc8949.html)

The Kit protocol version, business contract version, and plugin release version evolve independently. An additive optional field or detectable operation can advance a contract minor version. Changes to existing signatures or meaning require a new major version. At open time, both sides choose the highest mutually supported revision with the same schema fingerprint. The application can detect operations added after the negotiated revision. A single contract ID and revision describe the same portable schema on every target.

## Domain model and public API

| Name | Meaning |
| --- | --- |
| `PluginId` | Stable logical identity of a plugin across releases. |
| `PluginPackage` and `PluginManifest` | The installable artifacts and the declarations that describe them. |
| `PluginInfo` | A catalog snapshot containing the selected and available versions, enabled state, and execution status. |
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
| Configuration | `RegisterExtensionPoint(point)`, `ProvideService<T>()`, `RegisterBuiltinPlugin(entry)` | Declare typed extension points, supply host services, and explicitly register built-ins. |
| Manager lifetime | `StartAsync()`, `ShutdownAsync()` | Recover the profile before publishing the catalog. On shutdown, stop new work and release owned runtime resources. |
| Discovery | `RefreshCatalogAsync()`, `ListPlugins()`, `ListExtensions(point)` | Refresh configured sources and read catalog snapshots. Listing contributions uses manifest metadata. |
| Invocation | `CreateExtension(point, id)` | Instantiate a selected contribution and return a typed handle. The selection design also allows matching by typed metadata and user preference. |
| Changes | `PlanChangesAsync(changes)`, `ApplyPlanAsync(plan, approval)` | Coordinate installation, enablement, disablement, updates, removal, pinning, provider selection, grant changes, and rollback. |
| Settings | `GetSettings(plugin_id)`, `SetSettingsAsync(plugin_id, values)` | Read a snapshot and validate or persist changes against the plugin settings schema. |
| Observability | `SubscribeEvents()`, `Diagnostics()` | Report catalog revisions, state transitions, operation progress, and structured failures. Subscription handles release their callbacks. |

Explicit IDs and stored user preferences take precedence over automatic matching. A unique eligible match can be opened directly. An ambiguous query returns the candidates for selection. Disabled contributions leave the available extension list while their package and diagnostic records remain visible in management views.

The generated `ExtensionHandle<DocumentImporter>` exposes an asynchronous `ImportAsync` call. The completion result distinguishes the business value from a Kit failure such as an incompatible contract, deadline expiry, or worker exit. Explicit `CloseAsync()` waits for instance release when the caller needs that boundary. Releasing the handle also schedules cleanup.

A plugin activation owns a single `PluginContext`. The SDK passes it by const reference to optional `Activate` and `Deactivate` hooks and to each extension implementation that accepts it in its constructor. `Activate` runs before the first instance is created. Normal shutdown destroys the instances before calling `Deactivate`. The host derives the bundle directory from the approved package snapshot and the writable data directory from the plugin ID in the profile. Both are exposed as `huxerui::File`.

During an invocation, `huxerui::pluginkit::GetService<T>()` creates a typed proxy. The worker checks service access against the package digest authorized by the host. The caller can supply a `Receiver<T>` for a task operation and the plugin can await its methods while that invocation is active. The current desktop SDK shares one deadline across the invocation and its callbacks. See the [CSV example](../README.md#sdk-and-runtime) for the implemented API.

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

The manager verifies local packages against application trust policy. Remote sources use authenticated update metadata with expiry and rollback checks. Installed and previously verified packages can remain available offline under the active policy. An explicit rollback can select a retained trusted package and still applies current grants and trust decisions. [The Update Framework specification](https://theupdateframework.github.io/specification/latest/)

## Execution, permissions, and instance lifetime

Plugin origin and execution backend are separate. `Builtin` and `ExternalPackage` describe how the code is supplied. `InProcess`, `WorkerProcess`, and `Wasm` select an execution backend.

| Backend | Policy and lifetime |
| --- | --- |
| `InProcess` | Default for built-in plugins. The application can explicitly approve trusted external artifacts for this backend. External plugin libraries stay mapped until process exit, and an update takes effect at a subsequent startup. |
| `WorkerProcess` | Default for separately installed desktop plugins. Each active plugin generation has its own `hpk-worker` with a private IPC channel. The worker starts on demand and loads the selected artifact. |
| `Wasm` | Executes an independent module with configured imports and resource limits. Availability follows the target runtime and distribution policy. |

Process separation contains a plugin process failure. The application selects a sandbox policy for filesystem, network, and other operating system access. Kit checks declared and granted capabilities when dispatching host services and uses an eligible sandboxed backend where policy requires one. Scoped file and object grants carry the plugin identity, permitted operations, and lifetime. Package trust identifies what code is approved to run. The sandbox determines its access. [Windows AppContainer isolation](https://learn.microsoft.com/en-us/windows/win32/secauthz/appcontainer-isolation)

The manager starts a plugin generation once, after resolving its dependencies and grants. Concurrent opens share that activation. The manager publishes callable bindings after successful initialization and releases provisional resources when initialization fails. Each open creates an extension instance in that generation. Calls on one instance are serialized by default. The transport drives I/O independently of business execution, and host callbacks run on the configured executor. Call options supply a deadline and cancellation, and the generated call context exposes progress and cancellation to the implementation. Large streams have bounded buffers and backpressure. [Cancellation](https://grpc.io/docs/guides/cancellation/) and [flow control](https://grpc.io/docs/guides/flow-control/)

A handle identifies a concrete instance and generation. Disabling or replacing a generation withdraws its contributions and closes admission to new calls immediately. The manager cancels queued work, drains or cancels work already in progress under its shutdown policy, and releases the instances. A successfully started generation receives its stop hook before the manager releases the context. An application can retain a closed handle and receive a closed result. It opens a fresh handle by extension ID after a version change.

Worker exit completes pending calls with a failure and marks the generation faulted. A request delivered before a crash can have an unknown business outcome. The application can retry a safe operation explicitly, and Kit preserves the distinction between an undelivered request and a potentially executed request. Restart policy is bounded and exposed in diagnostics. A deadline ends the caller's wait and requests cancellation. For a worker that exceeds its stop deadline, the policy can terminate the worker. In-process execution keeps its resources until the relevant calls finish or the process exits. [Retry safety guidance](https://google.aip.dev/194)

## Management transactions and persisted state

A profile records the selected releases, dependency resolution, enabled flags, provider preferences, grants, and settings. One manager owns write access to that profile. Readers consume committed snapshots. Other writers receive a busy result and can use a separate profile or retry after the owner releases it.

`PlanChangesAsync` computes a proposed change against a specific profile revision and package digests. The plan lists installs and removals, affected dependents, version selections, required approvals, and restart requirements. An approval binds to the exact plan and its requested grants. `ApplyPlanAsync` rechecks the revision, package identity, and trust policy before changing state. A stale plan requires planning again.

The manager stages verified package files and candidate profile data, then commits a durable revision. Startup recovers an interrupted operation before exposing the catalog. Candidate activation and settings migration use staged Kit-owned state. A failed change preserves the last committed version selection, settings, and dependency resolution. Business data migration and recovery follow the application's contracts.

An enabled worker or Wasm plugin can be upgraded by validating the candidate generation and its resolved dependencies, committing the new selection, and draining the old generation. New calls go to newly opened handles. Updates to in-process plugin libraries are recorded as pending restart and validated at startup. Built-in code versions follow the application release. Old packages are retained while a running generation, a pin, or a rollback record references them.

Settings are versioned, validated against the declared schema, and persisted by plugin ID within the profile. The manager stages settings migrations with the candidate release. Private plugin data has its own lifetime and follows an explicit retention choice on removal. Grants are tied to plugin and publisher identity. A change that expands access requires approval before the new grant takes effect. Revocation invalidates the corresponding scoped handles. The manager restarts affected workers when the new policy requires a different sandbox.

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
- Interrupt an update and fail a candidate activation. Verify that startup recovers the last committed profile and that explicit rollback restores a retained release subject to current policy.
