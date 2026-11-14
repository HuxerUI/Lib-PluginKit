#pragma once

#if defined(_WIN32)
#include "win32.h"
#else
#include "posix_files.h"
#endif

#include <array>

namespace huxerui::pluginkit::detail {

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

template<class Consume>
void ReadFileChunks(const Handle& file, Consume consume) {
  std::array<std::byte, 64 * 1024> buffer;
  while (const auto count = ReadFileChunk(file.get(), buffer)) {
    consume(std::span<const std::byte>(buffer).first(count));
  }
}

inline void CopyRegularFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
  const auto input = OpenRegularFile(source);
  WriteFileContents(destination, [&](const auto& write) { ReadFileChunks(input, write); }, true);
}

} // namespace huxerui::pluginkit::detail
