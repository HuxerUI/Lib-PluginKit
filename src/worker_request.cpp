#include "worker_process.h"

namespace pluginkit::detail {

Frame Worker::Request(Frame request, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24)) {
    throw Error(ErrorCode::InvalidArgument, "Worker timeout must be between 1 ms and 24 hours");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock(requests_, std::defer_lock);
  // The request is still local while it waits for the worker's serialized channel.
  if (!lock.try_lock_until(deadline) || std::chrono::steady_clock::now() >= deadline) {
    throw Error(ErrorCode::DeadlineExceeded, "Worker deadline expired while waiting to send the request");
  }
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (!Pid()) {
    Stop();
    throw Error(ErrorCode::WorkerExited, "The plugin worker exited");
  }

  Frame reply{};
  try {
    reply = Exchange(request, deadline);
  } catch (const Error& error) {
    // Retire a failed exchange so a late reply cannot be read as the next request's response.
    const bool closed = !active_;
    Stop();
    if (closed && error.Code() != ErrorCode::DeadlineExceeded) {
      throw Error(ErrorCode::Closed, "The plugin generation is closed");
    }
    throw;
  } catch (...) {
    Stop();
    throw;
  }
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");

  if (reply.operation == 0) {
    if (reply.code == 0) {
      if (reply.instance != 0 && (request.instance == 0 || request.instance == reply.instance)) return reply;
    } else if (reply.code <= static_cast<std::uint32_t>(ErrorCode::PayloadTooLarge) && reply.instance == 0 &&
               IsUtf8(AsText(reply.payload))) {
      throw Error(static_cast<ErrorCode>(reply.code), std::string(AsText(reply.payload)));
    }
  }
  Stop();
  throw Error(ErrorCode::Protocol, "Invalid worker response");
}

ExtensionInstance::ExtensionInstance(std::shared_ptr<Worker> worker, std::uint32_t instance) noexcept
    : worker_(std::move(worker)), instance_(instance) {}

ExtensionInstance::ExtensionInstance(ExtensionInstance&& other) noexcept
    : worker_(std::move(other.worker_)), instance_(std::exchange(other.instance_, 0)) {}

ExtensionInstance& ExtensionInstance::operator=(ExtensionInstance&& other) noexcept {
  if (this != &other) {
    Release();
    worker_ = std::move(other.worker_);
    instance_ = std::exchange(other.instance_, 0);
  }
  return *this;
}

ExtensionInstance::~ExtensionInstance() { Release(); }

Bytes ExtensionInstance::Invoke(std::uint32_t operation, Bytes request, std::chrono::milliseconds timeout) const {
  if (!worker_) throw Error(ErrorCode::Closed, "The extension instance is closed");
  if (request.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "Request exceeds the byte limit");
  return worker_->Request({static_cast<std::uint32_t>(Command::Invoke), instance_, operation, std::move(request)},
                          timeout).payload;
}

void ExtensionInstance::Close() {
  // Consume the local handle before sending Close so later destruction cannot send it again.
  auto worker = std::exchange(worker_, {});
  const auto instance = std::exchange(instance_, 0);
  if (!worker || !worker->Pid()) return;
  try {
    worker->Request({static_cast<std::uint32_t>(Command::Close), instance, 0, {}}, DefaultTimeout);
  } catch (...) {
    // Stopping the worker releases the instance when the close request cannot complete.
    worker->Stop();
    throw;
  }
}

void ExtensionInstance::Release() noexcept {
  try {
    Close();
  } catch (...) {
  }
}

} // namespace pluginkit::detail
