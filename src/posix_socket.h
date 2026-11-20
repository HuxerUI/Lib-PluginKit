#pragma once

#include "posix_files.h"
#include "transport.h"

#include <algorithm>
#include <poll.h>
#include <stop_token>
#include <sys/socket.h>

namespace huxerui::pluginkit::detail {

inline constexpr int WorkerSocket = 3;
#if defined(__linux__)
inline constexpr int WorkerModule = 4;
#endif

class SocketStream {
public:
  explicit SocketStream(int fd, std::optional<Deadline> deadline = std::nullopt, std::stop_token closing = {})
      : fd_(fd), deadline_(deadline), closing_(closing) {}

  void Read(std::span<std::byte> bytes) const {
    Transfer(bytes, POLLIN, [this](auto remaining, int flags) {
      return ::recv(fd_, remaining.data(), remaining.size(), flags);
    }, "Cannot read worker frame");
  }

  void Write(std::span<const std::byte> bytes) const {
    Transfer(bytes, POLLOUT, [this](auto remaining, int flags) {
#if defined(__linux__)
      flags |= MSG_NOSIGNAL;
#endif
      return ::send(fd_, remaining.data(), remaining.size(), flags);
    }, "Cannot write worker frame");
  }

  void CheckDeadline(Deadline now = std::chrono::steady_clock::now()) const {
    const auto deadline = EffectiveDeadline(now);
    if (deadline && now >= *deadline) {
      throw Error(ErrorCode::DeadlineExceeded, "The plugin worker exceeded the request deadline");
    }
  }

private:
  std::optional<Deadline> EffectiveDeadline(Deadline now) const {
    // A cancelled Android call can drain its protocol without keeping a shared process busy indefinitely.
    if (closing_.stop_requested() && !closing_deadline_) closing_deadline_ = now + std::chrono::seconds(5);
    if (!closing_deadline_) return deadline_;
    return deadline_ ? std::min(*deadline_, *closing_deadline_) : closing_deadline_;
  }

  template<class Byte, class Operation>
  void Transfer(std::span<Byte> bytes, short events, Operation operation, std::string_view message) const {
    const int flags = deadline_ || closing_.stop_possible() ? MSG_DONTWAIT : 0;
    while (!bytes.empty()) {
      CheckDeadline();
      const auto count = operation(bytes, flags);
      if (count == 0) throw Error(ErrorCode::WorkerExited, "The worker socket closed");
      if (count < 0) {
        const int error = errno;
        if (error == EINTR) continue;
        if ((error != EAGAIN && error != EWOULDBLOCK) || (!deadline_ && !closing_.stop_possible())) {
          PosixError(ErrorCode::WorkerExited, message, error);
        }
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
      const auto deadline = EffectiveDeadline(now);
      auto remaining = deadline ? std::chrono::ceil<std::chrono::milliseconds>(*deadline - now)
                                : std::chrono::milliseconds(100);
      if (closing_.stop_possible()) remaining = std::min(remaining, std::chrono::milliseconds(100));
      const auto ready = ::poll(&socket, 1, static_cast<int>(remaining.count()));
      if (ready > 0) return;
      if (ready < 0 && errno != EINTR) PosixError(ErrorCode::WorkerExited, "Cannot wait on worker socket");
    }
  }

  int fd_;
  std::optional<Deadline> deadline_;
  std::stop_token closing_;
  mutable std::optional<Deadline> closing_deadline_;
};

} // namespace huxerui::pluginkit::detail
