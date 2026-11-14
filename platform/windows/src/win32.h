#pragma once

#include "bytes.h"

#include <windows.h>

#include <memory>

namespace huxerui::pluginkit::detail {

struct CloseHandleDeleter {
  void operator()(void* handle) const noexcept { CloseHandle(handle); }
};

using Handle = std::unique_ptr<void, CloseHandleDeleter>;

[[noreturn]] inline void WindowsError(ErrorCode code, std::string_view operation, DWORD error = GetLastError()) {
  throw Error(code, std::string(operation) + " (Win32 error " + std::to_string(error) + ")");
}

inline std::string Utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const auto length = static_cast<int>(text.size());
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
  if (!size) WindowsError(ErrorCode::InvalidArgument, "Invalid UTF-16 text");
  std::string result(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), size, nullptr, nullptr)) {
    WindowsError(ErrorCode::InvalidArgument, "Cannot encode UTF-8 text");
  }
  return result;
}

inline Handle OpenRegularFile(const std::filesystem::path& path) {
  const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    WindowsError(ErrorCode::Io, "Cannot open " + Utf8(path.native()), error);
  }
  Handle file(raw);
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info, sizeof(info))) {
    const auto error = GetLastError();
    WindowsError(ErrorCode::Io, "Cannot inspect " + Utf8(path.native()), error);
  }
  if (info.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
    throw Error(ErrorCode::Io, "Expected a regular file at " + Utf8(path.native()));
  }
  return file;
}

inline std::size_t ReadFileChunk(HANDLE file, std::span<std::byte> bytes, ErrorCode code = ErrorCode::Io,
                                 std::string_view operation = "Cannot read file") {
  DWORD count = 0;
  if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) WindowsError(code, operation);
  return count;
}

inline void ReadAll(HANDLE file, std::span<std::byte> bytes, ErrorCode code, std::string_view operation) {
  while (!bytes.empty()) {
    const auto count = ReadFileChunk(file, bytes, code, operation);
    if (!count) throw Error(code, std::string(operation) + " (unexpected end of file)");
    bytes = bytes.subspan(count);
  }
}

inline Bytes ReadFileBytes(HANDLE file, std::size_t limit) {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size)) WindowsError(ErrorCode::Io, "Cannot read file size");
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit) {
    throw Error(ErrorCode::PayloadTooLarge, "File exceeds the byte limit");
  }
  Bytes bytes(static_cast<std::size_t>(size.QuadPart));
  ReadAll(file, bytes, ErrorCode::Io, "Cannot read file");
  return bytes;
}

inline Bytes ReadFileBytes(const std::filesystem::path& path, std::size_t limit) {
  return ReadFileBytes(OpenRegularFile(path).get(), limit);
}

inline Handle LockProfile(const std::filesystem::path& directory) {
  const auto file = directory / L"profile.lock";
  const HANDLE raw = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION) throw Error(ErrorCode::ProfileBusy, "The profile is already open");
    WindowsError(ErrorCode::Io, "Cannot lock the plugin profile", error);
  }
  return Handle(raw);
}

inline void WriteAll(HANDLE file, std::span<const std::byte> bytes, ErrorCode code, std::string_view operation) {
  while (!bytes.empty()) {
    DWORD count = 0;
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) WindowsError(code, operation);
    if (!count) throw Error(code, std::string(operation) + " (zero bytes written)");
    bytes = bytes.subspan(count);
  }
}

template<class Feed>
void WriteFileContents(const std::filesystem::path& path, Feed feed, bool exclusive) {
  const HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, exclusive ? CREATE_NEW : CREATE_ALWAYS,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    WindowsError(ErrorCode::Io, "Cannot write " + PathText(path), error);
  }
  const Handle file(raw);
  feed([&](std::span<const std::byte> bytes) { WriteAll(file.get(), bytes, ErrorCode::Io, "Cannot write staged file"); });
  if (!FlushFileBuffers(file.get())) WindowsError(ErrorCode::Io, "Cannot flush staged file");
}

inline void WriteAtomicFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  auto staged = path;
  staged += L".tmp";
  WriteFileContents(staged, [&](const auto& write) { write(bytes); }, false);
  if (!MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto error = GetLastError();
    WindowsError(ErrorCode::Io, "Cannot commit " + Utf8(path.native()), error);
  }
}

} // namespace huxerui::pluginkit::detail
