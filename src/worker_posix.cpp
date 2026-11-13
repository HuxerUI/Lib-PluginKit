#include "worker_process.h"

#include "posix_socket.h"

#include <signal.h>
#include <sys/wait.h>

namespace huxerui::pluginkit::detail {

void Worker::StartReaper(pid_t child) {
  pid_ = child;
  try {
    reaper_ = std::thread([this, child] { Reap(child); });
  } catch (...) {
    Stop();
    Reap(child);
    throw;
  }
}

Worker::~Worker() {
  Stop();
  if (reaper_.joinable()) reaper_.join();
}

std::optional<std::uint32_t> Worker::Pid() const noexcept {
  const auto pid = pid_.load();
  if (!active_ || pid == 0) return std::nullopt;
  return static_cast<std::uint32_t>(pid);
}

void Worker::Stop() noexcept {
  std::lock_guard lock(stop_);
  {
    std::lock_guard queue_lock(requests_);
    if (!active_.exchange(false)) return;
  }
  request_ready_.notify_all();
  generation_stop_.request_stop();
  ::shutdown(socket_.get(), SHUT_RDWR);
  const auto pid = pid_.load();
  if (pid > 0) ::kill(-pid, SIGKILL);
}

void Worker::Reap(pid_t child) noexcept {
  siginfo_t info{};
  int result;
  do {
    result = ::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  std::lock_guard lock(stop_);
  if (result == 0) {
    // The unreaped child keeps the process group ID reserved until cleanup is complete.
    ::kill(-child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
  }
  pid_ = 0;
  ::shutdown(socket_.get(), SHUT_RDWR);
}

Frame Worker::Exchange(const Frame& request, Deadline deadline, std::stop_token stop,
                       const ReceiverRegistration* receiver) {
  const SocketStream stream(socket_.get(), deadline);
  return ExchangeFrames(request, deadline, stop, receiver, [&stream](const Frame& outgoing) {
    WriteFrame([&stream](std::span<const std::byte> bytes) { stream.Write(bytes); }, outgoing);
    return ReadFrame([&stream](std::span<std::byte> bytes) { stream.Read(bytes); });
  });
}

} // namespace huxerui::pluginkit::detail
