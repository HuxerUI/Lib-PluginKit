#pragma once

#include "posix_files.h"
#include "transport.h"

#include <poll.h>
#include <sys/socket.h>

namespace pluginkit::detail {

inline constexpr int WorkerSocket = 3;
#if defined(__linux__)
inline constexpr int WorkerModule = 4;
#endif

class SocketStream {
public:
  explicit SocketStream(int fd, std::optional<Deadline> deadline = std::nullopt) : fd_(fd), deadline_(deadline) {}

  void Read(std::span<std::uint8_t> bytes) const {
    Transfer(bytes, POLLIN, [this](auto remaining, int flags) {
      return ::recv(fd_, remaining.data(), remaining.size(), flags);
    }, "Cannot read worker frame");
  }

  void Write(ByteView bytes) const {
    Transfer(bytes, POLLOUT, [this](auto remaining, int flags) {
#if defined(__linux__)
      flags |= MSG_NOSIGNAL;
#endif
      return ::send(fd_, remaining.data(), remaining.size(), flags);
    }, "Cannot write worker frame");
  }

  void CheckDeadline(Deadline now = std::chrono::steady_clock::now()) const {
    if (deadline_ && now >= *deadline_) {
      throw Error(ErrorCode::DeadlineExceeded, "The plugin worker exceeded the request deadline");
    }
  }

private:
  template<class Byte, class Operation>
  void Transfer(std::span<Byte> bytes, short events, Operation operation, std::string_view message) const {
    const int flags = deadline_ ? MSG_DONTWAIT : 0;
    while (!bytes.empty()) {
      CheckDeadline();
      const auto count = operation(bytes, flags);
      if (count == 0) throw Error(ErrorCode::WorkerExited, "The worker socket closed");
      if (count < 0) {
        const int error = errno;
        if (error == EINTR) continue;
        if ((error != EAGAIN && error != EWOULDBLOCK) || !deadline_) PosixError(ErrorCode::WorkerExited, message, error);
        Wait(events);
      } else {
        bytes = bytes.subspan(static_cast<std::size_t>(count));
      }
    }
  }

  void Wait(short events) const {
    pollfd socket{fd_, events, 0};
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      CheckDeadline(now);
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline_ - now);
      const auto ready = ::poll(&socket, 1, static_cast<int>(remaining.count()));
      if (ready > 0) return;
      if (ready < 0 && errno != EINTR) PosixError(ErrorCode::WorkerExited, "Cannot wait on worker socket");
    }
  }

  int fd_;
  std::optional<Deadline> deadline_;
};

} // namespace pluginkit::detail
