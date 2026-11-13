#pragma once

#include "posix_socket.h"

#include <signal.h>
#include <sys/event.h>
#include <thread>

namespace pluginkit::detail {

inline void StartHostWatch() {
  // The watcher observes disconnects while the request thread is inside plugin code.
  Handle queue(::kqueue());
  if (!queue || ::fcntl(queue.get(), F_SETFD, FD_CLOEXEC) < 0) {
    PosixError(ErrorCode::Io, "Cannot create worker disconnect watcher");
  }
  struct kevent change{};
  // Clear readiness after delivery so the watcher can wait for EOF while requests are pending.
  EV_SET(&change, WorkerSocket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) < 0) {
    PosixError(ErrorCode::Io, "Cannot watch the host socket");
  }

  std::thread([queue = std::move(queue)] {
    for (;;) {
      struct kevent event{};
      const auto ready = ::kevent(queue.get(), nullptr, 0, &event, 1, nullptr);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0 || (event.flags & (EV_EOF | EV_ERROR))) break;
    }
    ::kill(-::getpid(), SIGKILL);
    ::_exit(1);
  }).detach();
}

} // namespace pluginkit::detail
