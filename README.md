# Lib-PluginKit

Lib-PluginKit is a HuxerUI C++20 library for application plugins. One desktop host can install approved packages and run each either in its own process or in a separate worker. Both modes expose the same C++ interface. The [design document](docs/design.md) describes the broader architecture.

The library and Preview platform directories follow the HuxerUI library template generated with `--platform all`. The PluginKit runtime and CSV preview target Windows x64 with MSVC, Linux x86_64 with glibc, and macOS arm64.

## Build and run the CSV example

### Windows

Use Windows 10 or 11 x64, the HuxerUI SDK and CLI, CMake 3.24 or newer, Ninja, and an LLVM installation that includes `libclang.dll`, its import library, and `clang-c` headers. Put the LLVM `bin` directory on `PATH` for the CLI build. Use an x64 Visual Studio developer shell for the standalone plugin build. If Ninja fails to track headers with localized MSVC output, run `chcp 65001` and configure a fresh build directory.

Build the preview from the library root. HuxerUI CLI selects the app in `examples/preview`.

```powershell
huxerui build windows --abi x86_64 --profile release
```

Use the build directory printed by the CLI to locate the preview executable and its published contract SDK. The separate plugin project also finds the HuxerUI SDK through `HUXERUI_HOME` or CMake's package search.

```powershell
$CsvPreviewBuild = '<build directory printed by the CLI>'
$CsvSdk = Join-Path $CsvPreviewBuild 'sdk'
cmake -S examples/csv_import/plugin -B build/csv-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCSV_IMPORT_SDK=$CsvSdk"
cmake --build build/csv-plugin
```

Inspect the example package, then pass its approved digest to the host to install it.

```powershell
$CsvHostExe = Join-Path $CsvPreviewBuild 'example_lib_plugin_kit.exe'
$CsvProfileDir = '.\build\csv-profile'
$CsvPackageDir = '.\build\csv-plugin\packages\csv-importer\Release'
$CsvInfo = & $CsvHostExe $CsvProfileDir inspect $CsvPackageDir | ConvertFrom-Json
$CsvInfo
& $CsvHostExe $CsvProfileDir install $CsvPackageDir $CsvInfo.digest in_process status
& $CsvHostExe $CsvProfileDir list
@(
    'import ".\examples\csv_import\sample.csv" example.csv'
    'status'
    'round-trip ".\examples\csv_import\sample.csv" ".\examples\csv_import\sample.ssv" example.csv'
) | & $CsvHostExe $CsvProfileDir
```

### Linux

Use Ubuntu 24.04 x86_64 with the HuxerUI SDK and CLI, CMake 3.24 or newer, Ninja, and a C++20 compiler. The generator and runtime build use the `clang`, `libclang-dev`, and `libssl-dev` packages. Build the preview from the library root.

```bash
huxerui build linux --profile release
```

Use the build directory printed by the CLI to build the standalone CSV plugin from the published SDK.

```bash
preview_build='<build directory printed by the CLI>'
cmake -S examples/csv_import/plugin -B build/csv-plugin -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCSV_IMPORT_SDK="$preview_build/sdk" -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-plugin
python3 examples/csv_import/verify.py "$preview_build/example_lib_plugin_kit" \
  build/csv-plugin/packages/csv-importer/Release build/csv-plugin/packages/csv-faults/Release \
  build/csv-plugin/packages/csv-importer-worker/Release build/csv-plugin/packages/csv-faults-upgrade/Release
```

The Linux host looks for `hpk-worker` beside its executable. Its default profile is `$XDG_DATA_HOME/huxerui/lib-pluginkit/preview`, with `$HOME/.local/share` as the data directory fallback.

### macOS

Use an Apple Silicon Mac with Xcode 26 or newer, the HuxerUI SDK and CLI, CMake 3.24 or newer, Ninja, Python 3, and an LLVM installation with Clang and libclang headers. Set `HPK_LLVM_ROOT` to the LLVM installation prefix. The preview uses the macOS 12.0 deployment target from the HuxerUI template. The task runtime uses the standard C++20 `<stop_token>` facilities. [Xcode 26 release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-26-release-notes)

```bash
huxerui build macos --profile release
preview_build='<build directory printed by the CLI>'
cmake -S examples/csv_import/plugin -B build/csv-plugin-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCSV_IMPORT_SDK="$preview_build/sdk" -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-plugin-macos
packages=build/csv-plugin-macos/packages
codesign --force --sign - "$packages/csv-importer/Release/plugin.dylib"
codesign --force --sign - "$packages/csv-faults/Release/plugin.dylib"
codesign --force --sign - "$packages/csv-faults-upgrade/Release/plugin.dylib"
codesign --force --sign - "$packages/csv-importer-worker/Release/plugin.dylib"
host="$preview_build/example_lib_plugin_kit.app/Contents/MacOS/example_lib_plugin_kit"
python3 examples/csv_import/verify.py "$host" "$packages/csv-importer/Release" "$packages/csv-faults/Release" \
  "$packages/csv-importer-worker/Release" "$packages/csv-faults-upgrade/Release"
```

The macOS preview locates `hpk-worker` in `Contents/MacOS` beside the host. Its default profile is `~/Library/Application Support/HuxerUI/Lib-PluginKit/Preview`. Use `huxerui package macos --profile release` to package the app with its helper. For local acceptance, run the same verification script against the packaged app's executable.

### Host commands

Run the host with just a profile directory for a session that accepts one command per line. Each command produces a JSON response. `list` shows installed packages, available importers and exporters, execution modes, and active worker process IDs. `enable <id>` and `disable <id>` change the whole package and persist across sessions. `import <file> [plugin-id] [timeout-ms]` selects an importer by its manifest suffix and reports the delimiter, plugin process ID, and parsed rows. The CSV plugin uses the first record as the header and reports malformed input with one-based row and column numbers.

`round-trip <csv-file> <ssv-file> <plugin-id>` uses the comma and semicolon importers from one package to parse equivalent inputs. It exports the resulting table as CSV and reimports it to verify the fields. The reply includes the CSV text and the process ID shared by all three factories.

Use `install <package> <approved-digest> <in_process|worker_process> [status]` to select the mode and grant access to the registered `ImportStatus` service. The approval persists in the profile. For `sample.csv`, the import reply reports `progress_rows` as 2 and the `status` command reports `last_imported_rows` as 2.

Use `upgrade <package> <installed-digest> <approved-digest> [status]` to replace a local package with another release of the same plugin. A worker upgrade takes effect in the current session. An in-process upgrade is applied when the preview starts in a new process. `uninstall <id> <installed-digest> [delete-data]` removes the plugin and its grants. The default preserves its current data for a later installation.

## Package the Windows preview

```powershell
huxerui package windows --abi x86_64 --profile release
```

The CLI writes the setup bundle to `examples/preview/dist/windows`. The installer puts `hpk-worker.exe` beside the preview executable. Starting the preview from its shortcut opens a console session with a profile under `%LOCALAPPDATA%\HuxerUI\Lib-PluginKit\Preview`.

## Verify the Windows example

Build the test fixture in the separate plugin project and run the verification script with Python 3.

```powershell
cmake -S examples/csv_import/plugin -B build/csv-plugin -G Ninja -DCMAKE_BUILD_TYPE=Release `
  "-DCSV_IMPORT_SDK=$CsvSdk" -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-plugin
python examples/csv_import/verify.py $CsvHostExe $CsvPackageDir .\build\csv-plugin\packages\csv-faults\Release `
  .\build\csv-plugin\packages\csv-importer-worker\Release .\build\csv-plugin\packages\csv-faults-upgrade\Release
```

The script installs two builds of the same CSV package in one profile, one per execution mode. It verifies ordered batch creation, ambiguous plugin ID lookup, candidate selection, and a round trip through the package's two importers and one exporter. It checks that the factories share a process and context, activate once, and are enabled or disabled together. The script also checks requests above the IPC size limit, service approval, receiver lifetime, catalog queries during callbacks, cancellation, and worker recovery. On Linux and macOS it checks that the worker exits when the host is killed during a pending call.

The upgrade fixture keeps the same plugin identifier and contracts and changes the release version. The script checks hot switching, restart switching, stale candidates, retained and deleted data, failures in activation and factory construction, interrupted validation, and cleanup after a commit.

## SDK and runtime

Acquire `HuxerUI::PluginKit` with `huxerui_use_library`. The C++ API is in `huxerui::pluginkit`. Link a mixed-mode host and its plugins to the same shared HuxerUI SDK, and ship `hpk-worker` beside the host executable. With an installed desktop SDK, request `find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)` before creating the app target. PluginKit validates the in-process build identity and that the library sees the host's active HuxerUI application.

- Hosts include `<huxerui/pluginkit.h>` and the generated `.host.h` for the contracts they call.
- Plugins include `<huxerui/pluginkit/plugin.h>` and the generated `.bindings.h` for the contracts they implement or call back to the host.
- Shared contract headers include `<huxerui/pluginkit/contract.h>` for PluginKit types.

Mark each IPC-capable interface class with `HPK_REMOTE`. `hpk gen` takes its self-contained header, qualified interface name, contract ID, positive integer version, and output directory. Arguments after `--` are passed to Clang. Publish the generated files and the PluginKit include tree to plugin authors. `HPK_BUILD_GENERATOR=OFF` builds the library and worker without the generator. Set `HPK_LLVM_ROOT` if LLVM is outside `PATH`.

The generator preserves synchronous value or `void` results and asynchronous `huxerui::Task<T>` results. Methods may have no request or one value request. Synchronous methods also accept a const-reference request. A Task method may take a second argument of type `Receiver<T>`, an alias of `std::shared_ptr<T>`. Generate its Task-returning receiver interface from a separate header. Wire values include fixed-width integers, `bool`, `std::string`, vectors, optional values, and aggregate records. Both sides check the exact contract fingerprint.

Register each extension point and the host services with an application-scoped `PluginManager`. Choose one execution mode and a set of service grants when approving the package. Managed calls on a package are serialized across its instances.

```cpp
plugins.RegisterExtensionPoint(Importers);
plugins.RegisterExtensionPoint(Exporters);
plugins.ProvideService<ImportStatus>(status);
plugins.InstallPackage(package, digest, {
    .mode = ExecutionMode::InProcess,
    .services = {std::string(Contract<ImportStatus>::Info.id)},
});

auto candidates = plugins.ListExtensions(Importers);
auto selected = std::ranges::find_if(candidates, [](const auto& candidate) {
    return candidate.plugin_id == "example.csv" &&
        std::ranges::find(candidate.file_extensions, ".csv") != candidate.file_extensions.end();
});
if (selected == candidates.end()) throw Error(ErrorCode::NotFound, "CSV importer unavailable");
auto importer = plugins.CreateExtension(Importers, *selected);
auto delimiter = importer->Delimiter();
auto result = co_await importer->Import(request, progress);

auto all_importers = plugins.CreateExtensions(Importers);
auto package_importers = plugins.CreateExtensions(Importers, "example.csv");
auto exporter = plugins.CreateExtension(Exporters, "example.csv");
```

Candidates refer to the approved package digest. Query again after upgrading. `CreateExtension(point, plugin_id)` reports `Ambiguous` when the plugin provides several factories for that point. `CreateExtensions` returns handles in plugin ID order and in manifest order within each package.

The handle releases the instance at scope exit. The first managed method starts the plugin and creates the instance. In-process calls invoke the real C++ object on the host's application thread with typed values. Worker calls encode values over IPC, and the worker calls the same implementation in a windowless Runtime. A synchronous IPC method blocks until it receives a result or reaches its deadline. It reports `Busy` if another business exchange is in progress and `CallbackUnavailable` if the method requests a host callback. Use Task methods for callbacks and work that needs to suspend.

A queued deadline or cancellation affects only that call. Canceling the caller's HuxerUI task suppresses its continuation. In a worker, canceling or timing out an active exchange closes the worker generation. In process, cancellation revokes the callback endpoint, requests cooperative stop, and retires the invocation Task. `RunInvocationWorker()` retains the instance while blocking work that borrows it finishes. Await `Shutdown()` before retiring the application scope.

Every activation owns one `PluginContext`. `Info()` supplies the manifest ID, release version, and approved digest. Both directory accessors return `const huxerui::File&`. `BundleDirectory()` points to the installed package snapshot, which plugin code should treat as read-only. `DataDirectory()` is writable and stays associated with the plugin ID across activations in that host profile. Extension implementations may be default-constructed or accept `const huxerui::pluginkit::PluginContext&`. All instances receive the same object as the lifecycle hooks. The manager takes `huxerui::File` for the profile and package directories. Pass the worker executable as a `File`, or `std::nullopt` for a host that only runs in-process plugins.

```cpp
void OnActivate(const huxerui::pluginkit::PluginContext& context);
void OnDeactivate(const huxerui::pluginkit::PluginContext& context);

HPK_EXPORT_PLUGIN(OnActivate, OnDeactivate,
    huxerui::pluginkit::BindExtension<TableImporter, CsvImporter>(Importers),
    huxerui::pluginkit::BindExtension<TableImporter, SemicolonImporter>(Importers),
    huxerui::pluginkit::BindExtension<TableExporter, CsvExporter>(Exporters))
```

Use `nullptr` for each omitted hook. PluginKit validates the whole factory table before running `OnActivate` once for the package. All factories in a worker package share one worker. On disable, uninstall, or shutdown, PluginKit stops admitting calls, drains active local work, destroys the instances, and calls `OnDeactivate` on orderly teardown. In-process libraries stay mapped until host exit. Worker teardown has a bounded deadline. Native-only contracts can publish `Contract<T>::Info` and use `BindExtension` without generated code. A package needs generated dispatch for every factory to run in a worker. Release handles to native-only interfaces after their own work finishes and before awaiting plugin disable, uninstall, or shutdown.

Register the host services and extension points, then await `ApplyPendingChanges()` before creating extensions. Inspect the results to report failed in-process upgrades. Call `UpgradePackage(directory, approval)` with the installed digest, the inspected replacement digest, and the full list of services approved for that release. The selected execution mode and enablement carry over. Open a new extension handle after the upgrade takes effect.

An enabled replacement runs `OnActivate` and exercises every factory's constructor on a private data copy. Plugins migrate their own format in `OnActivate`. Read the format marker before migrating so the same hook can run on later activations. A successful commit selects the replacement package, its data copy, and its grants together. A failed or interrupted validation leaves the last committed state selected. A disabled replacement stays disabled and activates on its first subsequent use. The profile owns these directories. Use `PluginContext::DataDirectory()` for the current activation's path.

`UninstallPackage(id, digest)` preserves the last committed data. Use `PluginDataPolicy::Delete` to remove it. Reinstalling with the same identifier reuses retained data and requires a new grant approval. `CleanupPending` means uninstall committed and file cleanup will be retried when the profile is reopened. In-process libraries may require host exit before their files can be removed.

Register the service implementation once with `ProvideService<T>(implementation)` before opening plugins. During a plugin Task, `GetService<T>()` synchronously returns a handle whose method resolves the current invocation and its package grants. A synchronous in-process extension method can use a synchronous service method. The application SDK can publish ordinary helpers such as `csv_import::SetImportedRows(rows)` around the handle.

The host passes a `std::shared_ptr<T>` as the receiver argument. The plugin calls it with `receiver->Method(...)`. PluginKit forwards native callback arguments directly and encodes worker callbacks for IPC. Using a receiver after its invocation ends fails with `ReceiverClosed`. Service and receiver tasks run in the manager's application scope and share the invocation deadline.

`ListPlugins()` and `ListExtensions()` read catalog snapshots during callbacks. Other manager and worker operations are subject to reentry checks and can fail with `ReentrantCall`. Keep handlers bounded. A synchronous handler can run beyond the deadline, and any changes it already applied remain in effect.

A directory package contains `plugin.json` and a platform-specific shared library. Use `plugin.dylib` for `macos-arm64`, `plugin.so` for `linux-x86_64-gnu`, and `plugin.dll` for `windows-x86_64-msvc`. The manifest lists `execution_modes` and carries the `cpp_abi` stamp for in-process packages. The SDK produced by the example publishes this stamp in `cpp-abi.txt`. Its digest is the SHA-256 of the concatenated lowercase hexadecimal SHA-256 digests of the raw manifest and library, in that order. The profile stores the approved snapshot, its mode, its service grants, and its enablement state.

In the [CSV manifest](examples/csv_import/plugin/plugin.json.in), top-level `provides` lists the contract ID, version, and fingerprint from each generated `contract.json`. Top-level `extensions` declares one entry per factory with its `point`, `contract`, and optional `metadata.extensions` file suffixes. Repeat a point to publish several implementations of its contract. Keep their order the same in the manifest and `HPK_EXPORT_PLUGIN`. The relative order of different points can vary.

On Linux each worker starts in a dedicated process group, receives the verified library file descriptor, and serves requests over a private Unix socket. A thread in the worker watches for host disconnects even during a plugin call. PluginKit owns the worker's exit status and reaps it while the host is running. Workers run under the application's user account and provide process fault isolation for trusted plugins.

On macOS the worker runs in its own process group and watches the private Unix socket for host disconnects with `kqueue`. The local preview uses ad hoc signing. With the Hardened Runtime, the process that loads the plugin needs a Library Validation policy that permits the plugin's signature.

Lib-PluginKit uses the [MIT License](LICENSE).
