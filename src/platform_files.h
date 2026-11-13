#pragma once

#if defined(_WIN32)
#include "win32.h"
#else
#include "posix_files.h"
#endif

namespace pluginkit::detail {

#if defined(_WIN32)
inline constexpr char PackageTarget[] = "windows-x86_64-msvc";
inline constexpr char ModuleName[] = "plugin.dll";
#elif defined(__APPLE__)
inline constexpr char PackageTarget[] = "macos-arm64";
inline constexpr char ModuleName[] = "plugin.dylib";
#else
inline constexpr char PackageTarget[] = "linux-x86_64-gnu";
inline constexpr char ModuleName[] = "plugin.so";
#endif

} // namespace pluginkit::detail
