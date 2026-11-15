#include "worker_process.h"
#include "stream_io.h"

#include "posix_socket.h"

#include <signal.h>
#include <sys/wait.h>

namespace huxerui::pluginkit::detail {

void Worker::StartReaper(pid_t child) {
  pid_ = child;
  run_->SetPid(static_cast<std::uint32_t>(child));
  try {
    reaper_ = std::thread([this, child] { Reap(child); });
  } catch (...) {
    run_->ActivationFailed(PluginFailureStage::StartWorker, std::current_exception());
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

void Worker::WaitForExit(std::chrono::milliseconds timeout) {
  std::unique_lock lock(stop_);
  if (!exited_.wait_for(lock, timeout, [this] { return pid_ == 0; })) {
    throw Error(ErrorCode::DeadlineExceeded, "The retired plugin worker has not exited");
  }
}

void Worker::Reap(pid_t child) noexcept {
  siginfo_t info{};
  int result;
  do {
    result = ::waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  std::lock_guard lock(stop_);
  std::optional<std::uint32_t> exit_code;
  std::optional<int> signal;
  if (result == 0) {
    if (info.si_code == CLD_EXITED) exit_code = static_cast<std::uint32_t>(info.si_status);
    if (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED) signal = info.si_status;
    // The unreaped child keeps the process group ID reserved until cleanup is complete.
    ::kill(-child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
  }
  // A subscriber can query the catalog and reopen the plugin as soon as the event is posted.
  pid_ = 0;
  exited_stop_.request_stop();
  run_->WorkerExited(exit_code, signal);
  exited_.notify_all();
  ::shutdown(socket_.get(), SHUT_RDWR);
}

Frame Worker::Exchange(const Frame& request, Deadline deadline, std::stop_token stop,
                        const ReceiverRegistration* receiver, const std::shared_ptr<CallStreams>& streams) {
  const SocketStream stream(socket_.get(), deadline);
  StreamIo stream_io;
  return ExchangeFrames(request, deadline, stop, receiver, streams, request.synchronous ? &stream_io : nullptr,
                        [&stream](const Frame& outgoing) {
    WriteFrame([&stream](std::span<const std::byte> bytes) { stream.Write(bytes); }, outgoing);
    return ReadFrame([&stream](std::span<std::byte> bytes) { stream.Read(bytes); });
  });
}

} // namespace huxerui::pluginkit::detail
