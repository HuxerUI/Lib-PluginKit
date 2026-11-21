# Lib-PluginKit

Lib-PluginKit lets a HuxerUI application install C++ plugin packages from local files. A host registers typed extension points and approves an execution mode for each installed plugin. It calls the same interface with `handle->Method()` in either mode. In-process plugins use native C++ calls. Worker plugins run in a separate, windowless HuxerUI application and use IPC for calls.

Worker execution provides process fault isolation for trusted plugins. Workers run with the application's user permissions. [Run the CSV example](#run-the-csv-example) to try both modes, streams, callbacks, and settings with independently built packages.

[Host SDK](#publish-an-extension-sdk) · [Plugin build](#build-a-plugin) · [Host calls](#install-and-call-from-the-host) · [Worker types](#worker-interface-types) · [Settings](#resources-and-settings) · [Package lifecycle](#manage-the-package-lifecycle)

## Requirements

| Platform | HuxerUI CLI `--abi` | PluginKit v1 manifest `target` | Plugin library |
| --- | --- | --- | --- |
| Windows | `x86_64` | `windows-x86_64-msvc` | `plugin.dll` |
| macOS | `arm64` | `macos-arm64` | `plugin.dylib` |
| macOS | `x86_64` | `macos-x86_64` | `plugin.dylib` |
| Linux | `x86_64` | `linux-x86_64-gnu` | `plugin.so` |
| Linux | `aarch64` | `linux-aarch64-gnu` | `plugin.so` |
| Android | `arm64-v8a` | `android-arm64-v8a` | `plugin.so` |
| Android | `x86_64` | `android-x86_64` | `plugin.so` |

HuxerUI can select both macOS slices for a universal app build from a source SDK and universal dependencies. Each PluginKit v1 package targets one ABI.

Build with CMake 3.24 or newer and a C++20 toolchain. On desktop, use a shared HuxerUI SDK that provides `Resources::OpenPackage()` and the callback-backed synchronous and asynchronous stream factories. Use a HuxerUI CLI with `--config` support for the CLI build and packaging commands. Build the host, worker, and plugins against the matching SDK and build configuration. Set `HUXERUI_HOME` to the installed SDK prefix or expose it through CMake's package search. In-process plugins also share the host's C++ ABI and active HuxerUI application. PluginKit's ABI and worker protocol are v1.

The `hpk` SDK generator needs Clang, libclang, and the `clang-c` headers. Set `HPK_LLVM_ROOT` to the LLVM prefix if CMake cannot find Clang on `PATH`. On Windows use the x64 Visual Studio developer shell for standalone CMake builds. If Ninja has trouble tracking headers with localized MSVC output, run `chcp 65001` and configure a fresh build directory. On Linux install the OpenSSL development files and, for SDK generation, `clang` and `libclang-dev`. On macOS use a toolchain with C++20 `<stop_token>` support and an LLVM installation for SDK generation.

| Where | Include |
| --- | --- |
| Host application | `<huxerui/pluginkit.h>` and the generated `.host.h` files for its contracts |
| Plugin implementation | `<huxerui/pluginkit/plugin.h>` and the generated `.bindings.h` files for its contracts |
| Shared interface declaration | `<huxerui/pluginkit/contract.h>` and the HuxerUI and C++ headers used by the interface |

The `pluginkit` namespace is nested in `huxerui`.

## Publish an extension SDK

The host owns the interface and the extension point. An `ExtensionPoint<T>` gives a host feature a stable name and associates it with `T`. A plugin manifest uses that point name to declare each implementation. In this example, save the following header as `sdk/text_formatter.h` in the host project.

```cpp
#pragma once

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>
#include <string>

namespace formatter_sdk {

using namespace huxerui;

struct FormatRequest {
  std::string text;
};

class HPK_REMOTE TextFormatter {
public:
  virtual ~TextFormatter() = default;

  virtual std::string Name() const = 0;
  virtual Task<std::string> Format(FormatRequest request) = 0;
};

inline constexpr pluginkit::ExtensionPoint<TextFormatter> Formatters{
    "example.text-formatters"};

}
```

`HPK_REMOTE` marks an interface for automatic binding generation. The default contract ID is `cpp.` followed by the SHA-256 of the qualified C++ name, here `formatter_sdk::TextFormatter`. The default version is `1`. To set both explicitly, use `class HPK_REMOTE_CONTRACT("example.text-formatter", 2) TextFormatter`. The generated contract also includes the fingerprint used to check both ends. The point ID `example.text-formatters` is the name used by plugin manifests.

### Add PluginKit to the host build

In the host's existing desktop HuxerUI project, select the shared SDK before creating the application target. For an installed SDK, use `find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)`. With the SDK built from source, enable the shared build and disable the static build for this application. The following example assumes the app target is `viewer` and the Lib-PluginKit checkout is at `third_party/Lib-PluginKit`.

```cmake
set(kit_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Lib-PluginKit")
huxerui_use_library(viewer TARGET HuxerUI::PluginKit PATH "${kit_dir}")

hpk_add_sdk(viewer HEADERS sdk)
hpk_use_worker(viewer)
```

Building `viewer` also builds `viewer-plugin-sdk`. `HEADERS` accepts files and directories at any depth under the publication base. Directory entries recursively select `.h`, `.hh`, `.hpp`, and `.hxx` files. A file entry selects that file. Relative inputs resolve from the current CMake source directory. Repeated selections of the same file are deduplicated.

Each selected file is published relative to the base directory. A single directory entry serves as its own base by default. Multiple entries or a single file default to the current CMake source directory. Set `BASE_DIRECTORY` to preserve a different shared layout. The following call publishes `text_formatter.h`, `types/…`, and `services/public/…` at the SDK root.

```cmake
hpk_add_sdk(viewer
    HEADERS sdk/text_formatter.h sdk/types sdk/services/public
    BASE_DIRECTORY sdk
)
```

Each header with marked interfaces gets one `.bindings.h`, one `.host.h`, and one `.contract.json` array describing its contracts. Include the callback headers used by those interfaces in `HEADERS` so the generator can resolve the bindings from their published paths. Conflicting output paths report both source files. Use `CLANG_OPTIONS` for additional include paths or defines needed to parse the SDK headers.

Distribute `plugin-sdk/viewer/Release` from the host build directory for a Release build. It also contains the PluginKit headers, `PluginKit.cmake`, and `cpp-abi.txt`. Set `OUTPUT_DIRECTORY` to choose a different location outside the scanned header directories. [CSV host build](examples/preview/CMakeLists.txt)

For an app consuming a pre-generated SDK, add its include directory. A build that only publishes native headers or consumes pre-generated bindings can set `HPK_BUILD_GENERATOR=OFF`. Call `hpk_use_worker()` for worker support. It stages `hpk-worker` beside the executable in the build tree and registers it and the shared HuxerUI runtime for app packaging. On macOS, apply the app's signing and Library Validation policy to the process that loads the plugin.

For in-process-only interfaces, select the header through `HEADERS` and provide `Contract<T>::Info` in the shared SDK. Give it a contract ID, positive version, and a 64-character lowercase hexadecimal fingerprint. Use the same `ExtensionPoint<T>` and `BindExtension` APIs. These interfaces can use native C++ signatures and need a virtual destructor. Publish such packages with `"execution_modes": ["in_process"]`.

## Build a plugin

Implement the SDK interface in a separate project. This `ascii_formatter.cpp` binds one factory and reads its saved setting through the optional `PluginContext` constructor.

```cpp
#include "text_formatter.bindings.h"
#include <huxerui/pluginkit/plugin.h>
#include <utility>

using namespace huxerui;
namespace hpk = pluginkit;

class AsciiFormatter final : public formatter_sdk::TextFormatter {
public:
  explicit AsciiFormatter(const hpk::PluginContext& context)
      : uppercase_(std::get<std::string>(context.Settings().at("letter_case")) == "upper") {}

  std::string Name() const override { return "ASCII Formatter"; }

  Task<std::string> Format(formatter_sdk::FormatRequest request) override {
    if (uppercase_) {
      for (auto& ch : request.text) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
      }
    }
    co_return std::move(request.text);
  }

private:
  bool uppercase_;
};

HPK_EXPORT_PLUGIN(nullptr, nullptr,
    hpk::BindExtension<formatter_sdk::TextFormatter, AsciiFormatter>(formatter_sdk::Formatters))
```

Save this package declaration as `plugin.json.in`. Use one `extensions` entry per exported factory. A package can contain several factories for the same point and factories for different points. For each point, keep the manifest entries and their matching `BindExtension` calls in the same order.

```json
{
  "schema_version": 1,
  "id": "example.ascii-formatter",
  "version": "0.1.0",
  "target": "@package_target@",
  "execution_modes": ["in_process", "worker_process"],
  "cpp_abi": "@cpp_abi@",
  "extensions": [
    {
      "point": "example.text-formatters",
      "metadata": {"name": "ASCII Formatter"}
    }
  ],
  "settings": [
    {
      "key": "letter_case",
      "type": "enum",
      "title": "Letter case",
      "default": "preserve",
      "options": [
        {"value": "preserve", "title": "Preserve"},
        {"value": "upper", "title": "Uppercase"}
      ]
    }
  ]
}
```

Use stable plugin and point IDs up to 128 characters long, starting with a lowercase ASCII letter or digit. The rest can also contain dots, underscores, and hyphens. `metadata` is an optional JSON object interpreted by the host. For example, the CSV host uses it to select a factory by file suffix. The loader checks all factory contracts against the host SDK before activating the package.

Use `PluginKit.cmake` from the published SDK in the standalone plugin project. Define a shared or module library with CMake, then pass that target to `hpk_add_plugin()`. It adds the SDK include path and HuxerUI runtime, fills `@package_target@` and `@cpp_abi@` in the manifest template, and creates a package target. The package stages the target's library under the filename expected by PluginKit. `SDK` defaults to the directory containing the included module and accepts an explicit path when needed.

```cmake
cmake_minimum_required(VERSION 3.24)
project(AsciiFormatter LANGUAGES CXX)

find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared HINTS "$ENV{HUXERUI_HOME}")
set(FORMATTER_SDK "$ENV{FORMATTER_SDK}" CACHE PATH "Published formatter SDK directory")
include("${FORMATTER_SDK}/PluginKit.cmake")

add_library(ascii_formatter SHARED ascii_formatter.cpp)
hpk_add_plugin(ascii_formatter MANIFEST plugin.json.in)
```

### Build the plugin directory

Use `hpk pack` in a HuxerUI CMake project with the CLI platform files. Set `FORMATTER_SDK` to the absolute path of the published SDK in the example above. Build the `hpk` target from PluginKit and make it and `huxerui` available on `PATH`. Set `HPK_BUILD_GENERATOR=OFF` for a packaging-only build of `hpk`. On Windows, keep the DLLs staged beside `hpk.exe` with it.

```sh
hpk pack linux
hpk pack windows --abi x86_64 --target ascii_formatter
```

`hpk pack` calls `huxerui build` with the platform and build options you pass. It defaults to `--config release`. It scans the current build's package targets under `.huxerui/build/home-*/<platform-and-abi>/<build-configuration>/packages/<target>/<configuration>`. When the build produces several packages, use `--target` with the name passed to `hpk_add_plugin()`. Build one platform and ABI per package.

On success the last line of output is a JSON object with the absolute `path`, CMake `target`, package `id`, `version`, and SHA-256 `digest`. The tool uses `PluginManager::InspectPackage()` to validate the chosen directory and compute the approval digest. It covers the manifest, the plugin library, and the contents and paths under `resources/`. Finish any signing that modifies the library before computing the digest.

For a standalone CMake build, build the package target directly.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFORMATTER_SDK="<published SDK directory>"
cmake --build build --target ascii_formatter-package
```

The standalone build stages `build/packages/ascii_formatter/Release`. The host accepts that directory or the directory reported by `hpk pack`. It contains `plugin.json`, the library named for the target, and `resources/` when declared. Archive creation and extraction are separate steps.

Use `HPK_EXPORT_PLUGIN(OnActivate, OnDeactivate, ...)` with hooks of signature `void(const PluginContext&)` when the package needs activation or teardown work. Use `nullptr` for an omitted hook. An activation calls `OnActivate` once, then shares one context across its factories, instances, and `OnDeactivate`. Desktop worker mode uses one process per active package. Android assigns packages to its Service process pool. On orderly teardown, PluginKit destroys the instances before calling `OnDeactivate`.

The [standalone CSV plugin](examples/csv_import/plugin/CMakeLists.txt) publishes several factories and packages compiled HuxerUI resources.

## Android integration

Android uses an installed static HuxerUI SDK without modifying its files. PluginKit builds `libhpk_runtime.so` from the SDK archive and redirects the participating CMake targets to that shared owner. The app and its worker processes each have their own HuxerUI Runtime; plugin libraries in a process use that process's Runtime. The APK supplies the common runtime, `libhpk_android.so`, and `libc++_shared.so`.

Set `huxeruiStl=c++_shared` in the app's `platform/android/gradle.properties`. `hpk_use_worker(app)` includes the runtime integration. An in-process-only host can call `hpk_use_runtime(app)` instead. Keep other native libraries on the same C++ runtime. The published plugin SDK includes its ABI-specific runtime under `runtime/<abi>/` for linking independently built plugins. `hpk_add_plugin()` uses that library; installable plugin packages exclude the common runtime and STL. Both Android execution modes check the published runtime build identity.

Call the Java bootstrap from `Application.attachBaseContext` before loading the host application library. Replace the template's unconditional static native-library initializer with this branch:

```java
@Override
protected void attachBaseContext(Context base) {
    super.attachBaseContext(base);
    if (!PluginKit.initializeProcess(this)) {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
```

Import `org.huxerui.lib.pluginkit.PluginKit`. If the Application also overrides `onCreate`, skip host-only work when `PluginKit.isWorkerProcess()` returns true. The worker branch declares its own empty application and uses the SDK's existing windowless Android initialization. The host application library is not loaded into workers.

Construct `PluginManager` with `RuntimeDesc::Android()` to enable the Android backend, or `std::nullopt` to disable it. The same synchronous `CreateExtension`, `CreateExtensions`, and `GetService` APIs apply. Binder bootstrap callbacks run independently of the main-thread ServiceConnection callback, so cold synchronous creation does not wait for that callback. Business calls, streams, and callbacks use independent package socket channels and the existing v1 protocol. Task dispatch can interleave across packages without mixing invocation identities or grants.

The library declares four private Services with separate process names. Processes start on demand. Each package is assigned to the least-loaded slot and stays there for the activation. Several packages can share a process, with independent PluginContexts, resources, settings, grants, and data directories. The slot count limits worker processes only. Normal package shutdown closes its channel; the process exits when the last channel retires. Process crashes, protocol failures, and unresponsive calls terminate every activation sharing that process. All affected packages report `WorkerExited`; the host remains alive. Binder death does not provide a portable exit code or signal, so those diagnostic fields can be absent on Android.

Cancellation revokes the call's grants and drains the reply within the remaining deadline, bounded by a five-second cancellation budget. A process is terminated if the exchange cannot finish safely. A package upgrade drains that package and loads its new version from a separate installed path; other packages can retain the worker. In-process upgrades still require a host process restart. Uninstall retains data unless the host explicitly requests deletion.

The app owns source selection, copying, and archive extraction. Import into an app-private local staging directory, inspect the package, obtain digest and service-grant approval, then call `InstallPackage`. The installed host APK does not need to contain plugin code. HuxerUI resource directories and settings use the same APIs as desktop packages.

Android SDK generation runs a native `hpk` built automatically in PluginKit's build cache with the existing SDK CLI. Target Clang options come from the NDK, and `hpk abi` evaluates the target ABI without executing Android binaries. Publishing Android headers still needs this ABI tool when `HPK_BUILD_GENERATOR=OFF`. Set `HPK_HOST_TOOL` to an existing native `hpk` executable when a build environment supplies one. Android builds require API 23 or later and a matching NDK/SDK ABI.

## Install and call from the host

Create one `PluginManager` per writable profile on the owning HuxerUI application thread. Supply `RuntimeDesc::Executable(worker_executable)` on desktop, `RuntimeDesc::Android()` on Android, or `std::nullopt` for in-process-only use. Register the extension points and host services on each startup, then await `ApplyPendingChanges()` before using installed plugins. Each result reports any error applying a staged upgrade.

```cpp
#include "text_formatter.host.h"
#include <huxerui/task.h>
#include <iostream>
#include <utility>

using namespace huxerui;
namespace hpk = pluginkit;

Task<void> InitializePlugins(hpk::PluginManager& plugins) {
  plugins.RegisterExtensionPoint(formatter_sdk::Formatters);
  for (const auto& change : co_await plugins.ApplyPendingChanges()) {
    if (change.error) std::cerr << change.plugin_id << " " << change.message << '\n';
  }
}

Task<std::string> FormatText(hpk::PluginManager& plugins, std::string text) {
  auto formatter = plugins.CreateExtension(formatter_sdk::Formatters, "example.ascii-formatter");
  std::clog << formatter->Name() << '\n';
  formatter_sdk::FormatRequest request{std::move(text)};
  co_return co_await formatter->Format(std::move(request));
}
```

In a running HuxerUI application, construct the manager with the application task scope.

```cpp
hpk::PluginManager plugins(profile_directory, hpk::RuntimeDesc::Executable(worker_executable), UseApplicationTaskScope());
co_await InitializePlugins(plugins);
```

For a new installation, show the inspected package identity and digest to the host's approval flow. Pass the approved digest, execution mode, and service grant list to `InstallPackage()`.

```cpp
const auto package = hpk::PluginManager::InspectPackage(package_directory);
// Present package.id, package.version, and package.digest for approval.
plugins.InstallPackage(package_directory, approved_digest, {
    .mode = hpk::ExecutionMode::WorkerProcess,
    .services = {},
});
auto formatted = co_await FormatText(plugins, "Hello");
```

The profile stores the approved package, grants, enablement, and settings. Open the same profile on subsequent launches to reuse them. Use `ExecutionMode::InProcess` to approve the formatter for in-process use. The `FormatText` call is the same in both modes.

| API | When to use it |
| --- | --- |
| `ListPlugins()` | Inspect installed packages, enablement, mode, worker PID, and staged upgrades. |
| `ListExtensions(point)` | Discover available factories and inspect their `metadata_json`. |
| `CreateExtension(point, candidate)` | Select a specific factory from a discovered `ExtensionInfo`. Refresh candidates after a package upgrade. |
| `CreateExtension(point, plugin_id)` | Select the sole factory for that plugin at the point. Several matching factories report `Ambiguous`. |
| `CreateExtensions(point, plugin_id)` | Create handles for all factories at the point, optionally filtered by plugin ID. |

Creation returns synchronously. An `HPK_REMOTE` handle activates the package on its first managed call. An unmarked in-process interface acquires the native object during creation. The move-only handle releases the instance on destruction. Keep the handle alive through the last call. Release handles that borrow native interfaces before awaiting disable, uninstall, or `Shutdown()`. Await `Shutdown()` before closing the HuxerUI application task scope.

## Worker interface types

Use public pure virtual methods with distinct names and a public virtual destructor on an `HPK_REMOTE` interface. Methods may be `const` and must allow exceptions to propagate. Methods can take zero or more typed parameters in C++ declaration order. A call can interleave several ordinary values, streams, and callback objects.

| Usage | Supported signature |
| --- | --- |
| Ordinary value | `bool`, `std::int32_t`, `std::uint32_t`, `std::int64_t`, `std::uint64_t`, UTF-8 `std::string`, and aliases of these types |
| Containers | `std::vector<T>` with the standard allocator and `std::optional<T>`, where `T` is a supported wire value |
| Records | Named namespace-scope structs made of supported named public fields |
| Custom value | An owning type with a visible `WireCodec<T>` specialization that projects it to a supported wire value |
| Synchronous method | Return a wire value or `void`. Pass ordinary values by value or `const T&`. Pass `InputStream` and `OutputStream` by value. |
| Task method | Return `Task<T>` or `Task<void>`. Pass values, `AsyncInputStream`, `AsyncOutputStream`, and invocation callback objects by value. |

Use structs with fields only for automatic record encoding. Recursive records and bit fields are rejected. Keep raw pointers, reference results, mutable references, borrowed views, and live UI objects such as `View` on in-process-only interfaces. Across processes, pass an owned value or an endpoint from the table.

Declare custom mappings in the interface header or in a header it includes. A mapping defines `WireType`, `static WireType Encode(const T&)`, and `static T Decode(WireType)`. Use this for enums, floating-point types, or application values that need validation. The CSV SDK maps a validated separator to text, for example.

```cpp
using namespace huxerui;

template<>
struct pluginkit::WireCodec<csv_import::Separator> {
  using WireType = std::string;

  static WireType Encode(const csv_import::Separator& value) { return value.Text(); }
  static csv_import::Separator Decode(WireType text) {
    return csv_import::Separator(std::move(text));
  }
};
```

IPC encodes the wire representation as CBOR. In-process dispatch uses the original C++ type. The contract fingerprint includes method signatures, parameter names and order, and the selected wire representations. Both the host and plugin must build with the published bindings. Ordinary IPC requests and replies have an 8 MiB limit. Use streams for large inputs or outputs. [CSV exporter contract](examples/preview/sdk/exporters/export_contract.h)

### Streams and cancellation

Move SDK streams from the host into the extension call. In process the plugin reads from or writes to the original endpoint. A worker routes stream operations to their original endpoints in chunks of up to 1 MiB. Complete each operation before starting the next one. Synchronous worker stream callbacks run on a host I/O thread. Supply endpoints that can run there independently of the waiting application thread. Asynchronous stream callbacks run in the manager's application task scope.

The plugin must successfully call `Close()` or await `CloseAsync()` on each output before returning success. The host can stage an output file and publish it after the call succeeds. Stream and callback access ends with that invocation, and cancellation or worker exit revokes outstanding grants. Await operations within the invocation or an awaited child Task. [CSV stream implementations](examples/csv_import/plugin/csv_package.h)

Managed calls for one package are serialized. The default deadline is 5 seconds, including startup and queue time. Pass a different timeout to `CreateExtension()` or `CreateExtensions()` if the workload needs it. A synchronous worker method waits for its reply and can return `Busy` while another call is running. Use a Task for work that needs to suspend or call back into the host.

Cancelling a managed in-process Task requests cooperative stop. Plugin code can await `GetInvocationStopToken()`. Use `RunInvocationWorker()` when a blocking callable borrows the instance or context. PluginKit retains them until that callable finishes. Cancellation or a deadline during an active worker exchange can retire the worker. A call that times out in the queue leaves the running call alone. An unexpected worker exit completes pending calls with `WorkerExited`. A later call through a new handle can start a fresh activation.

### Host services and invocation callbacks

For reusable host functionality, mark a service interface with `HPK_REMOTE` and publish its generated bindings in the host SDK. Its operations use ordinary values. Register one implementation with `ProvideService<T>()` before calling `ApplyPendingChanges()` or installing packages that require it. Add the service's contract ID to `InstallOptions::services` or `UpgradeApproval::services` when approving the package.

```cpp
plugins.ProvideService<csv_import::ImportStatus>(status_implementation);

hpk::InstallOptions options{
    .mode = hpk::ExecutionMode::WorkerProcess,
    .services = {std::string(hpk::Contract<csv_import::ImportStatus>::Info.id)},
};
plugins.InstallPackage(package_directory, approved_digest, std::move(options));
```

Within a managed plugin call, `GetService<T>()` returns a handle synchronously. The method call checks the current invocation and the package's grant. Task service methods work in both execution modes. Synchronous service methods are available within synchronous managed in-process calls. An SDK can wrap a service in an ordinary function so the plugin calls `co_await SetImportedRows(rows)`. [CSV service interface](examples/preview/sdk/services/import_status.h), [CSV SDK helper](examples/preview/sdk/csv_sdk.h)

For progress or other data specific to one call, pass a `std::shared_ptr<T>` to a Task operation. Declare `T` as an `HPK_REMOTE` interface in a separate SDK header with Task methods that take ordinary values. The SDK build generates the callback binding beside that header, such as `receivers/import_progress.bindings.h`. The host supplies a concrete implementation. The plugin uses `co_await progress->OnRows(rows, input_bytes)`. A null pointer omits the callback. Several callback parameters can refer to different host objects. [CSV callback interface](examples/preview/sdk/receivers/import_progress.h), [CSV importer](examples/csv_import/plugin/csv_importer.cpp)

Host Task service and callback implementations run on the manager's application scope. They may read snapshots such as `ListPlugins()`, `ListExtensions()`, and `GetSettings()`. Schedule management changes and further managed calls after the callback returns to avoid `ReentrantCall`.

## Resources and settings

`PluginContext::Info()` provides the plugin ID, release version, and approved digest. `BundleDirectory()` points to the installed snapshot, which plugin code should treat as read-only. `DataDirectory()` points to the writable data directory selected by the current profile. Both paths are HuxerUI `File` values.

For plugin resources, add `RESOURCE_ROOT resources RESOURCE_NAMESPACE ascii_strings` to `hpk_add_plugin()`. The helper uses HuxerUI to compile the resource directory and stages the complete compiled output as `resources/`. The plugin can use the generated keys with `context.Resources().GetString(...)`, `GetRawResource(...)`, or the image getters. PluginKit opens one resource resolver for the activation and retains it through `OnDeactivate`. [CSV resource build](examples/csv_import/plugin/CMakeLists.txt)

### Declare settings and defaults

Put optional settings in the top-level `settings` array in `plugin.json`. Each item needs a stable `key` using the same identifier syntax as a plugin ID, a `type`, a user-facing `title`, and a `default` that satisfies that setting's type and constraints. `description` is optional. The array order gives the host the display order.

| Manifest type | Suggested host control | Value and constraints |
| --- | --- | --- |
| `boolean` | Toggle or checkbox | JSON boolean |
| `integer` | Integer field | Signed 64-bit integer with optional inclusive `minimum` and `maximum` |
| `string` | Text field | UTF-8 string |
| `enum` | Dropdown | String chosen from the ordered `options` array. Each option has a unique `value` and a `title`. The required default matches one of those values. |

The formatter above initially selects `Preserve`. A saved `"letter_case": "upper"` selects `Uppercase`. The host saves the stable option value and displays the option title.

### Localize the form

Titles, descriptions, and option titles accept plain strings or a string resource reference. For example, replace the formatter's title value with this object.

```json
{"resource": "ascii_strings:strings/letter_case"}
```

Add `letter_case = Letter case` to `resources/strings/default.properties` and `letter_case = 字母大小写` to `resources/strings/zh.properties`, then build and package the resources. Inspection, installation, and upgrades validate references using the SDK's default configuration. The host calls `GetSettings(plugin_id, locale)` with the current UI locale to resolve the labels. HuxerUI supplies the fallback text when a translation is missing. Refresh the snapshot when the UI locale changes. Disabled plugins also expose their schema and text.

### Read, save, and apply values

`GetSettings()` returns the localized `schema`, the `effective` values to display, explicit `overrides`, the saved `revision`, and the `active_revision` when the plugin is active. Read labels with `std::get<std::string>(field.title)`. Use the field type and constraints to choose an editor. For an enum, select the option whose `value` equals the effective value.

```cpp
const auto locale = Locale::FromLanguageTag("zh-Hans-CN");
auto settings = plugins.GetSettings("example.ascii-formatter", locale);
auto overrides = std::move(settings.overrides);
overrides.insert_or_assign("letter_case", std::string{"upper"});
co_await plugins.SetSettingsAsync("example.ascii-formatter", settings.revision, std::move(overrides));
```

Save the complete override map. Remove a key to restore its default. A stale edit reports `SettingsChanged`. `SettingsValidationError::Issues()` gives the keys and reasons for a rejected edit. A successful change persists the values atomically and publishes a `PluginCatalogChange::SettingsChanged` event.

The plugin reads effective values with `PluginContext::Settings()`, including defaults. All instances share the snapshot captured at activation. A saved edit applies on the next activation in either execution mode. Use the saved and active revisions to show whether an edit has taken effect. To apply it immediately to an enabled package, disable and re-enable the plugin and obtain new handles.

## Manage the package lifecycle

| Operation | Behavior |
| --- | --- |
| `SetEnabled(id, enabled)` | Persists enablement for the whole package. Disabling closes existing handles and retires the active generation. |
| `UpgradePackage(directory, approval)` | Approves a different release of the same plugin ID. Pass the installed digest, the inspected replacement digest, and the full service grant list. Worker upgrades can apply in the current session. In-process upgrades return `RestartRequired` and are applied by `ApplyPendingChanges()` in a new host process. |
| `UninstallPackage(id, digest)` | Removes the catalog entry, grants, and package. The default keeps data and saved settings for a later installation. Pass `PluginDataPolicy::Delete` to delete them. `CleanupPending` means the profile committed and a later opening will retry file cleanup. |

An enabled upgrade validates the new activation and all factories on a copy of plugin data. Perform any plugin data migration in `OnActivate` and make it safe to run on later activations. The commit selects the package, grants, data, and settings together. If validation fails or is interrupted, the profile reopens the previous committed release. An in-process validation failure after mapping the candidate can require another host restart to load that release. After a successful upgrade, refresh candidates and create new handles.

Upgrades and reinstalls keep explicit settings whose keys and types still match and whose values pass the new constraints. Other settings take their new defaults. A disabled package stays disabled through upgrade.

### Observe changes and diagnose failures

Call `ListPlugins()` for the initial catalog and retain the move-only token from `SubscribeEvents()` for future changes. Events arrive on the manager's application task scope after the corresponding state update. They include catalog changes, activation and stop events, worker exits, and upgrade failures. Destroying the token stops future deliveries.

`Diagnostics(plugin_id)` returns the installed status, current runtime state, and latest failure for the manager session. Use `Error::Code()` and `ErrorName()` for error handling. For `ContractMismatch`, check that your manifest points match the exported factories and that the host and plugin were built with the same published SDK and ABI. For `ApprovalMismatch`, inspect the current package files and present their digest for approval again. [Host API reference](include/huxerui/pluginkit.h)

## Run Photo Studio

The default preview is a photo editor with an original and a result pane. It opens with a sample photo. Install an effect package to add a tool, choose a strength, and click **Apply**. The filter reports progress and writes the result pixels to a stream. Its returned palette drives the color swatches under the result.

Build the preview using the platform commands below. Then build the two effect packages against its published SDK with the same toolchain, ABI, and configuration.

```sh
cmake -S examples/photo_filters/plugin -B build/photo-plugins -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPHOTO_STUDIO_SDK="<absolute path to preview build>/sdk"
cmake --build build/photo-plugins
```

Launch the preview executable with no arguments, or open the preview APK on Android. Click **Install effect** and choose `build/photo-plugins/packages/mono/Release` or `build/photo-plugins/packages/warm/Release`. The approval shows the package digest. Choose **Run in worker** for Black & White and **Run in app** for Warm Tone to try both modes in the same session. **Open photo** accepts a JPEG or PNG.

Warm Tone supplies its own slider and presets through the in-process [panel contract](examples/preview/sdk/photo/effect_panel.h). Select **Soft · 35%** or **Golden · 80%**, then click **Apply**. The studio mounts the returned `View` directly. **Remove effect** removes the panel from the view tree and releases its extension handle through the composition lifecycle so the package can finish uninstalling. Black & White uses the studio's strength slider and can run in either mode.

For Android, use the same NDK settings as the [Android CSV build](#android-csv-verification) and pass the matching Android SDK directory as `PHOTO_STUDIO_SDK`. For a debug preview, use `-DCMAKE_BUILD_TYPE=Debug` and select the package's `Debug` folder on the device. On macOS, sign each package library before approving its digest.

The [host UI](examples/preview/src/photo_studio.cpp) discovers effects from package metadata. Both execution modes use the same [filter contract](examples/preview/sdk/photo/image_filter.h) and plugin implementation. The host decodes the source and caps the preview at 2048 pixels on its longest edge. PluginKit carries RGBA8 pixels through native streams and sends the palette back as typed data.

The sample is [Mountain Lake Sunset](https://commons.wikimedia.org/wiki/File:Mountain_Lake_Sunset.jpg) by 420 Photography, dedicated to the public domain under CC0 1.0.

## Run the CSV example

The preview also has a command mode for the standalone CSV plugin and its automated regressions. The package has two importers and one exporter. The plugin reports progress, calls a host service to record the row count, and reads a localized `max_rows` setting. The export contract exercises synchronous and asynchronous stream operations. [Command host](examples/preview/src/csv_host.cpp), [Plugin source](examples/csv_import/plugin/csv_importer.cpp)

### Build the preview and plugin

Run the applicable HuxerUI CLI command from the repository root and use the build directory it prints.

| Platform | Build command |
| --- | --- |
| Windows | `huxerui build windows --abi x86_64 --config release` |
| Linux | `huxerui build linux --config release` |
| macOS | `huxerui build macos --config release` |

The preview publishes the SDK in the build directory's `sdk/` subdirectory. Build the plugin separately with Ninja. Set `CSV_IMPORT_SDK` to that absolute path. Build the preview, worker, and plugin for the same ABI. On macOS, select `--abi arm64` or `--abi x86_64` for the CLI build and use the matching `-DCMAKE_OSX_ARCHITECTURES=arm64` or `-DCMAKE_OSX_ARCHITECTURES=x86_64` for the standalone plugin. Match the preview's deployment target, which defaults to macOS 12.0. On Linux, select `--abi x86_64` or `--abi aarch64` and use the corresponding compiler and HuxerUI SDK for the plugin build.

```sh
cmake -S examples/csv_import/plugin -B build/csv-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCSV_IMPORT_SDK=<absolute path to preview build>/sdk" -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-plugin
```

`build/csv-plugin/packages/csv-importer/Release` is the installable CSV package directory. For a local macOS run, sign the plugin libraries before inspecting their packages. The example uses ad hoc signing.

| Platform | Preview executable relative to its build directory |
| --- | --- |
| Windows | `example_lib_plugin_kit.exe` |
| Linux | `example_lib_plugin_kit` |
| macOS | `example_lib_plugin_kit.app/Contents/MacOS/example_lib_plugin_kit` |

### Install and use the CSV plugin

The CLI takes the profile directory before the command and returns one JSON reply per command. Launch with `--cli` to open an interactive session on the default profile. Use a new profile for the following commands.

```powershell
$CsvHost = Join-Path '<preview build directory>' 'example_lib_plugin_kit.exe'
$CsvProfile = 'build/csv-profile'
$CsvPackage = 'build/csv-plugin/packages/csv-importer/Release'

$CsvInfo = & $CsvHost $CsvProfile inspect $CsvPackage | ConvertFrom-Json
& $CsvHost $CsvProfile install $CsvPackage $CsvInfo.digest worker_process status
& $CsvHost $CsvProfile list
& $CsvHost $CsvProfile import examples/csv_import/sample.csv example.csv
& $CsvHost $CsvProfile settings example.csv zh-Hans-CN
```

On Linux or macOS, set `csv_host` to the executable from the table. The macOS example signs the plugin before inspection so approval covers the signed library.

```sh
csv_host='<path to preview executable>'
csv_profile='build/csv-profile'
csv_package='build/csv-plugin/packages/csv-importer/Release'

if [ "$(uname -s)" = Darwin ]; then
    codesign --force --sign - "$csv_package/plugin.dylib"
fi

digest=$("$csv_host" "$csv_profile" inspect "$csv_package" |
    python3 -c 'import json, sys; print(json.load(sys.stdin)["digest"])')
"$csv_host" "$csv_profile" install "$csv_package" "$digest" worker_process status
"$csv_host" "$csv_profile" list
"$csv_host" "$csv_profile" import examples/csv_import/sample.csv example.csv
"$csv_host" "$csv_profile" settings example.csv zh-Hans-CN
```

Use `in_process` in the install command with a separate profile to try native execution. Passing just the profile opens a session that accepts one command per line. The `status` command can then report the most recent import's row count in that session. Use `set-settings <id> <revision> <overrides-file>` with a JSON object such as `{"max_rows": 1}` to limit the next activation to one data row per import.

| Command | Example behavior |
| --- | --- |
| `round-trip <csv-file> <ssv-file> <plugin-id>` | Parse equivalent delimited inputs, export the table, and reimport the result. |
| `convert <sync\|async> <ssv-file> <output-file> <plugin-id> [chunk-kib]` | Stream one conversion. The host publishes the output after the call succeeds. |
| `convert-pair <sync\|async> <first-input> <first-output> <second-input> <second-output> <plugin-id>` | Route two inputs and two outputs through one call. The async variant also exercises two progress objects. |

### Verify and package the preview

Run the verification script with Python 3, passing the preview executable and the four fixture directories built above. On macOS, sign `plugin.dylib` in each directory before running the script.

```sh
python3 examples/csv_import/verify.py "$csv_host" \
    build/csv-plugin/packages/csv-importer/Release \
    build/csv-plugin/packages/csv-faults/Release \
    build/csv-plugin/packages/csv-importer-worker/Release \
    build/csv-plugin/packages/csv-faults-upgrade/Release
```

In PowerShell, use `python` and `$CsvHost`, with backticks for line continuation or the arguments on one line. The suite covers both modes, localized settings, callbacks, streaming inputs and outputs larger than 8 MiB, upgrade recovery, and reopening an existing profile. [Verification script](examples/csv_import/verify.py)

Append `--hpk <path-to-hpk>` to also build the CSV sources through `hpk pack`. Keep the same HuxerUI CLI and SDK available to the verification process. This checks target selection, stale build directories, resource validation, and installation with the reported digest.

Use `huxerui package windows --abi x86_64 --config release` to build the Windows installer with the worker beside the preview executable. Use `huxerui package macos --config release` to package the macOS application with its helper.

### Android CSV verification

Build the existing preview with `huxerui build android --abi x86_64 --config debug` from `examples/preview`. The native build publishes the CSV SDK at `platform/android/app/.cxx/Debug/<configuration-hash>/x86_64/sdk` relative to that example. Use the matching `arm64-v8a` directory for an ARM device.

Build the standalone CSV sources with the same NDK and configuration:

```sh
cmake -S examples/csv_import/plugin -B build/csv-android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="<NDK>/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-23 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Debug -DHUXERUI_HOME="<HuxerUI SDK>" \
    -DCSV_IMPORT_SDK="<Android CSV SDK directory>" -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-android
python3 examples/csv_import/verify_android.py \
    examples/preview/platform/android/app/build/outputs/apk/debug/app-debug.apk \
    build/csv-android/packages --serial <adb-device-id>
```

The runner installs the debuggable APK, then imports separately built packages into its private verification directory. It fills all four slots before activating another package to verify shared-process dispatch, cancellation, package-local disable, crash recovery, and a neighbor surviving upgrade. It also checks upgrade failure, saved-profile reuse, localized settings, and sync/async stream input and output above 8 MiB in both execution modes. Native module globals can persist across activations in a shared worker. Initialize and release activation state in the package hooks. The runner removes its own temporary files after verification. Production builds do not accept the example's command-file Intent.

On Android, `hpk pack` first runs the normal CLI build, then completes the registered CMake package targets for the ABIs present in that build. It scans their staged package directories and uses the same package inspection and digest implementation with the Android target. This does not add files to the plugin package or require SDK CLI changes.

## License

Lib-PluginKit uses the [MIT License](LICENSE). See the [design document](docs/design.md) for architecture and planned capabilities.
