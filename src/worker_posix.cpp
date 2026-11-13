#include "worker_process.h"

#include "posix_socket.h"

#include <signal.h>
#include <sys/wait.h>

namespace pluginkit::detail {

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
  if (!active_.exchange(false)) return;
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

Frame Worker::Exchange(const Frame& request, Deadline deadline) {
  const SocketStream stream(socket_.get(), deadline);
  WriteFrame([&stream](ByteView bytes) { stream.Write(bytes); }, request);
  auto reply = ReadFrame([&stream](std::span<std::uint8_t> bytes) { stream.Read(bytes); });
  stream.CheckDeadline();
  return reply;
}

} // namespace pluginkit::detail
