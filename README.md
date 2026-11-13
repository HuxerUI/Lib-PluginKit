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

Use the build directory printed by the CLI to locate the preview executable and its published SDK. Pass that SDK to the separate plugin project.

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
& $CsvHostExe $CsvProfileDir import .\examples\csv_import\sample.csv
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

Use an Apple Silicon Mac with Xcode, the HuxerUI SDK and CLI, CMake 3.24 or newer, Ninja, Python 3, and an LLVM installation with Clang and libclang headers. Set `HPK_LLVM_ROOT` to the LLVM installation prefix. The preview uses the macOS 12.0 deployment target from the HuxerUI template.

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

## Package the Windows preview

```powershell
huxerui package windows --abi x86_64 --profile release
```

The CLI writes the setup bundle to `examples/preview/dist/windows`. The installer puts `hpk-worker.exe` beside the preview executable. Starting the preview from its shortcut opens a console session with a profile under `%LOCALAPPDATA%\HuxerUI\Lib-PluginKit\Preview`.

## Verify the Windows example

Build the test fixture in the separate plugin project and run the verification script with Python 3.

```powershell
cmake -S examples/csv_import/plugin -B build/csv-plugin -DCSV_IMPORT_TEST_FIXTURES=ON
cmake --build build/csv-plugin
python examples/csv_import/verify.py $CsvHostExe $CsvPackageDir .\build\csv-plugin\packages\csv-faults\Release
```

The script verifies discovery before activation, CSV conversion and diagnostics, worker exit and timeout recovery in the same host, profile recovery, and rejection of mismatched contracts and modified packages. On Linux and macOS it also sends `SIGKILL` to the host while a plugin call is hung and checks that the worker exits before the call deadline.

## SDK and runtime

Acquire `HuxerUI::PluginKit` with `huxerui_use_library`, include `<huxerui/pluginkit.h>`, and ship `hpk-worker` on Linux and macOS or `hpk-worker.exe` on Windows with the application. `hpk gen` takes a self-contained contract header, a qualified interface name, a contract ID, a positive integer version, and an output directory. Arguments after `--` are passed to Clang. Publish the generated files and the `include/pluginkit` headers to plugin authors. `HPK_BUILD_GENERATOR=OFF` builds the runtime and worker without the developer tool. Set `HPK_LLVM_ROOT` if LLVM is outside `PATH`.

The generator accepts public pure virtual operations with one value or const-reference request and a value result. Wire types include `bool`, the 32-bit and 64-bit fixed-width integers, `std::string`, `std::vector`, `std::optional`, and aggregate structs declared in the contract header. Numbered operations and record fields are recorded in `contract.json`. Records use CBOR arrays in field-number order and both ends check the exact contract fingerprint.

The application registers `ExtensionPoint<T>` with `PluginManager`, lists contributions by their manifest metadata, and opens one with `OpenExtension`. The generated `ExtensionHandle<T>` calls are synchronous with per-request deadlines. A deadline that expires while a request waits its turn affects only that request. A worker exit or in-flight timeout closes that generation and a subsequent open can start a new one. The worker loads the plugin library and queries its C ABI descriptor through `pluginkit_query_v1`.

A directory package contains `plugin.json` and a platform-specific shared library. Use `plugin.dylib` for `macos-arm64`, `plugin.so` for `linux-x86_64-gnu`, and `plugin.dll` for `windows-x86_64-msvc`. Its digest is the SHA-256 of the concatenated lowercase hexadecimal SHA-256 digests of the raw manifest and library, in that order. The profile stores approved package snapshots and enablement state.

On Linux each worker starts in a dedicated process group, receives the verified library file descriptor, and serves requests over a private Unix socket. A thread in the worker watches for host disconnects even during a plugin call. PluginKit owns the worker's exit status and reaps it while the host is running. Workers run under the application's user account and provide process fault isolation for trusted plugins.

On macOS the worker runs in its own process group and loads the approved library from its absolute path in the profile. The worker watches the private Unix socket for host disconnects with `kqueue`. The local preview uses ad hoc signing. A worker distributed with the Hardened Runtime must have a Library Validation policy that permits the plugin's signature.

Lib-PluginKit uses the [MIT License](LICENSE).
