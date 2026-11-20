#include "worker_process.h"
#include "android_bridge.h"
#include "posix_socket.h"
#include "stream_io.h"
#include <algorithm>

namespace huxerui::pluginkit::detail {

Worker::Worker(const std::filesystem::path&, const std::filesystem::path& module, const Handle& verified,
               std::vector<RegisteredService> services, TaskScope callbacks, PluginStartup startup,
               std::shared_ptr<PluginRun> run, Deadline start_deadline)
    : services_(std::move(services)), callbacks_(std::move(callbacks)), startup_(std::move(startup)), run_(std::move(run)) {
  const auto session = OpenAndroidSession(PathText(module), verified.get(),
      std::min(DefaultTimeout, std::chrono::duration_cast<std::chrono::milliseconds>(start_deadline - std::chrono::steady_clock::now())));
  socket_.reset(session.socket);
  pid_ = static_cast<pid_t>(session.pid);
  session_ = session.id;
  run_->SetPid(session.pid);
  try {
    reaper_ = std::thread([this] {
      pollfd endpoint{socket_.get(), POLLRDHUP, 0};
      int result;
      do { result = ::poll(&endpoint, 1, -1); } while (result < 0 && errno == EINTR);
      std::lock_guard lock(stop_);
      pid_ = 0;
      exited_stop_.request_stop();
      run_->WorkerExited({}, {});
      exited_.notify_all();
    });
  } catch (...) { Stop(); throw; }
}

Worker::~Worker() { Stop(); if (reaper_.joinable()) reaper_.join(); }

std::optional<std::uint32_t> Worker::Pid() const noexcept {
  const auto pid = pid_.load();
  if (!active_ || !pid) return std::nullopt;
  return static_cast<std::uint32_t>(pid);
}

void Worker::Stop() noexcept {
  std::lock_guard lock(stop_);
  {
    std::lock_guard queue(requests_);
    if (!active_.exchange(false)) return;
  }
  request_ready_.notify_all();
  generation_stop_.request_stop();
  CloseAndroidSession(session_, graceful_shutdown_);
  ::shutdown(socket_.get(), SHUT_RDWR);
}

void Worker::WaitForExit(std::chrono::milliseconds timeout) {
  // Android retirement waits for this package's channel; other packages can keep the process alive.
  std::unique_lock lock(stop_);
  if (!exited_.wait_for(lock, timeout, [this] { return pid_ == 0; })) {
    throw Error(ErrorCode::DeadlineExceeded, "The retired Android package channel has not closed");
  }
}

Frame Worker::Exchange(const Frame& request, Deadline deadline, std::stop_token stop, const std::shared_ptr<CallGrants>& grants) {
  const SocketStream stream(socket_.get(), deadline, invocation_cancel_.get_token());
  StreamIo stream_io;
  return ExchangeFrames(request, deadline, stop, grants, request.synchronous ? &stream_io : nullptr,
      [&stream](const Frame& outgoing) {
        WriteFrame([&stream](std::span<const std::byte> bytes) { stream.Write(bytes); }, outgoing);
        return ReadFrame([&stream](std::span<std::byte> bytes) { stream.Read(bytes); });
      });
}

} // namespace huxerui::pluginkit::detail
