# Lib-PluginKit

Lib-PluginKit is a HuxerUI C++20 library for application plugins. The desktop previews generate typed SDK bindings, install approved local packages, and run plugins on demand in separate worker processes. The [design document](docs/design.md) describes the broader architecture.

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
& $CsvHostExe $CsvProfileDir install $CsvPackageDir $CsvInfo.digest
& $CsvHostExe $CsvProfileDir list
@(
    "provide-status $($CsvInfo.digest)"
    'import ".\examples\csv_import\sample.csv" example.csv'
    'status'
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
  build/csv-plugin/packages/csv-importer/Release build/csv-plugin/packages/csv-faults/Release
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
host="$preview_build/example_lib_plugin_kit.app/Contents/MacOS/example_lib_plugin_kit"
python3 examples/csv_import/verify.py "$host" "$packages/csv-importer/Release" "$packages/csv-faults/Release"
```

The macOS preview locates `hpk-worker` in `Contents/MacOS` beside the host. Its default profile is `~/Library/Application Support/HuxerUI/Lib-PluginKit/Preview`. Use `huxerui package macos --profile release` to package the app with its helper. For local acceptance, run the same verification script against the packaged app's executable.

### Host commands

Run the host with just a profile directory for a session that accepts one command per line. Each command produces a JSON response. `list` shows the installed packages, enabled importers, and active worker process IDs. `enable <id>` and `disable <id>` persist across sessions. `import <file> [plugin-id] [timeout-ms]` selects an enabled importer from the manifest suffixes. The CSV plugin uses the first record as the header and reports malformed input with one-based row and column numbers.

Run `provide-status <approved-digest>` in the same session before opening the CSV plugin. This grants that package access to the preview's `ImportStatus` service. A successful import calls its receiver's `OnRows` and the service's `SetImportedRows` before returning the table. For `sample.csv`, the import reply reports `progress_rows` as 2 and the `status` command reports `last_imported_rows` as 2.

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
python examples/csv_import/verify.py $CsvHostExe $CsvPackageDir .\build\csv-plugin\packages\csv-faults\Release
```

The script verifies discovery before activation, a shared plugin context across two instances, activation failure and retry, and deactivation on disable and host shutdown. It also checks CSV conversion and diagnostics, worker exit and timeout recovery, profile recovery, and rejection of mismatched contracts and modified packages. Callback checks cover authorization, resumption on the host task's thread, catalog reads during callbacks, rejection of nested state changes, expired receivers, cancellation, and updates applied before the plugin times out. On Linux and macOS the script sends `SIGKILL` to the host while a plugin call is hung and checks that the worker exits before the call deadline.

## SDK and runtime

Acquire `HuxerUI::PluginKit` with `huxerui_use_library`. The C++ API is in `huxerui::pluginkit`. Ship `hpk-worker` on Linux and macOS or `hpk-worker.exe` on Windows with the application and its shared HuxerUI runtime. Build the plugin against the same shared HuxerUI SDK as the worker.

- Hosts include `<huxerui/pluginkit.h>` and the generated `.host.h` for the contracts they call.
- Plugins include `<huxerui/pluginkit/plugin.h>` and the generated `.bindings.h` for the contracts they implement or call back to the host.
- Shared contract headers include `<huxerui/pluginkit/contract.h>` for PluginKit types.

`hpk gen` takes a self-contained contract header, a qualified interface name, a contract ID, a positive integer version, and an output directory. Arguments after `--` are passed to Clang. Publish the generated files and the PluginKit include tree, including its `detail` headers, to plugin authors, who build against the HuxerUI SDK. `HPK_BUILD_GENERATOR=OFF` builds the runtime and worker without the developer tool. Set `HPK_LLVM_ROOT` if LLVM is outside `PATH`.

The generator accepts public pure virtual operations with one value or const-reference request and a value or `void` result. An operation declared with `huxerui::Task<T>` takes its request by value and may also take one `huxerui::pluginkit::Receiver<T>` by value. Generate the receiver interface from its own header and publish its bindings at the same relative path as that header. Wire types include `bool`, the 32-bit and 64-bit fixed-width integers, `std::string`, `std::vector`, `std::optional`, and aggregate structs declared in the contract header. The SDK records operation numbers and record fields in `contract.json`. Records use CBOR arrays in field-number order and both ends check the exact contract fingerprint. Encoded payloads use `huxerui::Bytes` for storage.

Create `PluginManager` with an application `TaskScope` for reverse callbacks. Register an `ExtensionPoint<T>`, list contributions by manifest metadata, and await `CreateExtension(point, id)` in a HuxerUI task. Generated host calls and `Close()` return `huxerui::Task`. Await `Shutdown()` before retiring the application scope. The worker runs a windowless HuxerUI Runtime on its application thread. It loads the plugin through `pluginkit_query_v1` and uses a separate thread for IPC.

Each worker executes one invocation at a time. A deadline or cancellation while a request waits its turn affects only that request. A worker exit, in-flight timeout, or in-flight cancellation closes the generation. Subsequent instance creation can start a new generation. Canceling a HuxerUI task suppresses its continuation. A caller that is still waiting when the plugin is disabled, exits, or exceeds its deadline receives a PluginKit error.

Every activation owns one `PluginContext`. `Info()` supplies the manifest ID, release version, and approved digest. Both directory accessors return `const huxerui::File&`. `BundleDirectory()` points to the installed package snapshot, which plugin code should treat as read-only. `DataDirectory()` is writable and stays associated with the plugin ID across activations in that host profile. Extension implementations may be default-constructed or accept `const huxerui::pluginkit::PluginContext&`. All instances receive the same object as the lifecycle hooks. The manager also takes `huxerui::File` for the profile, worker executable, and package directories.

```cpp
void Activate(const huxerui::pluginkit::PluginContext& context);
void Deactivate(const huxerui::pluginkit::PluginContext& context);

HPK_EXPORT_PLUGIN(csv_import::TableImporter, CsvImporter, Activate, Deactivate)
```

Supply either hook as needed. Use `nullptr` for an omitted activation hook when supplying only deactivation. `Activate` runs once before the first instance is constructed, and a thrown exception fails instance creation. Closing an instance releases that object. Disabling the plugin or shutting down the manager attempts a bounded teardown that destroys the remaining instances and calls `Deactivate`. The module stays loaded through Runtime shutdown. A crash or forced termination can interrupt teardown.

Generate a host service from a separate C++ interface with `hpk gen`. The host registers its implementation with `ProvideService<T>(implementation, approved_digest)` before activating the package. Each worker receives the authorized services available at startup. During an operation, plugin code can `co_await huxerui::pluginkit::GetService<T>().Method(request)`. An application can expose its own convenience functions around that API.

The host passes a `std::shared_ptr<T>` as the receiver argument. The invocation and any running callback retain it for their lifetimes. The plugin awaits the generated proxy's methods. Invoking a receiver saved from an earlier call fails with `ReceiverClosed`. Service and receiver tasks run in the manager's callback scope and share the invocation deadline. Proxy calls check that the awaiting HuxerUI task belongs to the active invocation. `Delay()` and `AwaitCallback<T>()` resume on the owning application thread.

`ListPlugins()` and `ListExtensions()` read catalog snapshots during callbacks. Other manager and worker operations are subject to reentry checks and can fail with `ReentrantCall`. Keep handlers bounded. A synchronous handler can run beyond the deadline, and any changes it already applied remain in effect.

A directory package contains `plugin.json` and a platform-specific shared library. Use `plugin.dylib` for `macos-arm64`, `plugin.so` for `linux-x86_64-gnu`, and `plugin.dll` for `windows-x86_64-msvc`. Its digest is the SHA-256 of the concatenated lowercase hexadecimal SHA-256 digests of the raw manifest and library, in that order. The profile stores approved package snapshots and enablement state.

On Linux each worker starts in a dedicated process group, receives the verified library file descriptor, and serves requests over a private Unix socket. A thread in the worker watches for host disconnects even during a plugin call. PluginKit owns the worker's exit status and reaps it while the host is running. Workers run under the application's user account and provide process fault isolation for trusted plugins.

On macOS the worker runs in its own process group and loads the approved library from its absolute path in the profile. The worker watches the private Unix socket for host disconnects with `kqueue`. The local preview uses ad hoc signing. A worker distributed with the Hardened Runtime must have a Library Validation policy that permits the plugin's signature.

Lib-PluginKit uses the [MIT License](LICENSE).
