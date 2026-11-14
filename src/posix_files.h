#pragma once

#include "bytes.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace huxerui::pluginkit::detail {

class Handle {
public:
  explicit Handle(int fd = -1) noexcept : fd_(fd) {}
  Handle(Handle&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) reset(std::exchange(other.fd_, -1));
    return *this;
  }
  ~Handle() { reset(); }

  explicit operator bool() const noexcept { return fd_ >= 0; }
  int get() const noexcept { return fd_; }
  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

private:
  int fd_;
};

[[noreturn]] inline void PosixError(ErrorCode code, std::string_view operation, int error = errno) {
  throw Error(code, std::string(operation) + " (errno " + std::to_string(error) + ")");
}

inline Handle OpenRegularFile(const std::filesystem::path& path, int flags = O_RDONLY) {
  Handle file(::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
  if (!file) {
    const int error = errno;
    PosixError(ErrorCode::Io, "Cannot open " + PathText(path), error);
  }
  struct stat status {};
  if (::fstat(file.get(), &status) != 0) PosixError(ErrorCode::Io, "Cannot inspect file");
  if (!S_ISREG(status.st_mode)) throw Error(ErrorCode::Io, "Expected a regular file at " + PathText(path));
  return file;
}

inline std::size_t ReadFileChunk(int file, std::span<std::byte> bytes) {
  ssize_t count;
  do {
    count = ::read(file, bytes.data(), bytes.size());
  } while (count < 0 && errno == EINTR);
  if (count < 0) PosixError(ErrorCode::Io, "Cannot read file");
  return static_cast<std::size_t>(count);
}

inline Bytes ReadFileBytes(int file, std::size_t limit) {
  struct stat status {};
  if (::fstat(file, &status) != 0) PosixError(ErrorCode::Io, "Cannot read file size");
  if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > limit) {
    throw Error(ErrorCode::PayloadTooLarge, "File exceeds the byte limit");
  }

  Bytes bytes(static_cast<std::size_t>(status.st_size));
  std::span remaining(bytes);
  while (!remaining.empty()) {
    const auto count = ReadFileChunk(file, remaining);
    if (!count) throw Error(ErrorCode::Io, "Unexpected end of file");
    remaining = remaining.subspan(count);
  }
  return bytes;
}

inline Bytes ReadFileBytes(const std::filesystem::path& path, std::size_t limit) {
  return ReadFileBytes(OpenRegularFile(path).get(), limit);
}

inline Handle LockProfile(const std::filesystem::path& directory) {
  auto file = OpenRegularFile(directory / "profile.lock", O_RDWR | O_CREAT);
  while (::flock(file.get(), LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    if (error == EINTR) continue;
    if (error == EWOULDBLOCK) throw Error(ErrorCode::ProfileBusy, "The profile is already open");
    PosixError(ErrorCode::Io, "Cannot lock the plugin profile", error);
  }
  return file;
}

inline bool SyncFile(int file) {
  int result;
  do {
    result = ::fsync(file);
  } while (result < 0 && errno == EINTR);
  return result == 0;
}

template<class Feed>
void WriteFileContents(const std::filesystem::path& path, Feed feed, bool exclusive) {
  const auto file = OpenRegularFile(path, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC));
  feed([&](std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
      const auto count = ::write(file.get(), bytes.data(), bytes.size());
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) PosixError(ErrorCode::Io, "Cannot write staged file");
      if (!count) throw Error(ErrorCode::Io, "Zero bytes written to staged file");
      bytes = bytes.subspan(static_cast<std::size_t>(count));
    }
  });
  if (!SyncFile(file.get())) PosixError(ErrorCode::Io, "Cannot flush staged file");
}

inline void WriteAtomicFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
  Handle directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory) PosixError(ErrorCode::Io, "Cannot open staging directory");
  auto staged = path;
  staged += ".tmp";
  WriteFileContents(staged, [&](const auto& write) { write(bytes); }, false);
  if (::rename(staged.c_str(), path.c_str()) != 0) PosixError(ErrorCode::Io, "Cannot commit staged file");
  // The rename is the commit point for the in-memory catalog.
  SyncFile(directory.get());
}

} // namespace huxerui::pluginkit::detail
