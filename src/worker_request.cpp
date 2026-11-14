#include "worker_process.h"
#include "run_transport.h"

#include <algorithm>
#include <limits>

namespace huxerui::pluginkit::detail {
namespace {

// Callbacks for different workers can interleave on the application thread while awaiting child tasks.
thread_local std::vector<TaskExecution> active_callbacks;

template<class Release>
struct RequestSlot {
  Release release;
  ~RequestSlot() { release(); }
};

struct CallbackReply {
  std::mutex mutex;
  std::condition_variable_any ready;
  std::optional<Bytes> bytes;
  std::exception_ptr error;
  bool abandoned = false;
  TaskHandle task;
};

Task<void> ExecuteCallback(std::shared_ptr<CallbackReply> reply, ServiceDispatch dispatch,
                           std::uint32_t operation, Bytes request) {
  std::optional<Bytes> bytes;
  std::exception_ptr error;
  try {
    {
      CallbackScope scope(co_await CurrentTaskExecution{});
      bytes.emplace(co_await dispatch(operation, std::move(request)));
    }
  } catch (...) {
    error = std::current_exception();
  }
  dispatch = {};
  std::lock_guard lock(reply->mutex);
  if (reply->abandoned) co_return;
  reply->bytes = std::move(bytes);
  reply->error = std::move(error);
  reply->ready.notify_one();
}

Bytes RunHostCallback(const TaskScope& tasks, ServiceDispatch dispatch, std::uint32_t operation, Bytes request,
                      Deadline deadline, std::stop_token stop) {
  auto reply = std::make_shared<CallbackReply>();
  tasks.Post([tasks, reply, dispatch = std::move(dispatch), operation, request = std::move(request), deadline, stop]() mutable {
    std::lock_guard lock(reply->mutex);
    if (reply->abandoned || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return;
    try {
      reply->task = tasks.Launch(ExecuteCallback(reply, std::move(dispatch), operation, std::move(request)));
    } catch (...) {
      reply->error = std::current_exception();
      reply->ready.notify_one();
    }
  });
  std::unique_lock lock(reply->mutex);
  if (!reply->ready.wait_until(lock, stop, deadline, [&] { return reply->bytes.has_value() || reply->error; })) {
    reply->abandoned = true;
    auto task = reply->task;
    lock.unlock();
    task.Cancel();
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The invocation was cancelled");
    throw Error(ErrorCode::DeadlineExceeded, "The host callback exceeded the invocation deadline");
  }
  if (reply->error) std::rethrow_exception(reply->error);
  return std::move(*reply->bytes);
}

} // namespace

CallbackScope::CallbackScope(TaskExecution execution) : execution_(std::move(execution)) {
  active_callbacks.push_back(execution_);
}

CallbackScope::~CallbackScope() {
  const auto same = [&](const auto& active) {
    return !active.owner_before(execution_) && !execution_.owner_before(active);
  };
  const auto match = std::find_if(active_callbacks.rbegin(), active_callbacks.rend(), same);
  if (match != active_callbacks.rend()) active_callbacks.erase(std::next(match).base());
}

void RejectServiceReentry() {
  // Guard synchronous management operations conservatively because they have no task identity to compare.
  if (!active_callbacks.empty()) throw Error(ErrorCode::ReentrantCall, "A host callback is active on this thread");
}

void RejectServiceReentry(const TaskExecution& execution) {
  if (std::ranges::any_of(active_callbacks, [&](const auto& active) {
        return SameExecution(active, execution) || !HasExecutionOwner(active);
      })) {
    throw Error(ErrorCode::ReentrantCall, "A host callback cannot reenter PluginKit");
  }
}

Frame Worker::RejectSynchronousCallback(const Frame& frame) {
  auto reply = ErrorReply(Error(ErrorCode::CallbackUnavailable, "A synchronous call cannot use host callbacks"));
  reply.kind = FrameKind::ServiceReply;
  reply.call = frame.call;
  reply.callback = frame.callback;
  reply.instance = frame.instance;
  reply.receiver = frame.receiver;
  return reply;
}

void Worker::CheckExchange(Deadline deadline, std::stop_token stop) {
  if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The invocation was cancelled");
  if (std::chrono::steady_clock::now() >= deadline) {
    throw Error(ErrorCode::DeadlineExceeded, "The plugin worker exceeded the request deadline");
  }
}

Frame Worker::DispatchService(const Frame& frame, Deadline deadline, std::stop_token stop,
                              const ReceiverRegistration* receiver) {
  const auto request = ReadServicePayload(frame.payload);
  Frame reply{};
  try {
    const ContractInfo requested{request.id, request.version, request.fingerprint};
    const ServiceDispatch* dispatch;
    if (frame.receiver) {
      if (!receiver || frame.receiver != 1) throw Error(ErrorCode::ReceiverClosed, "The receiver is unavailable");
      if (receiver->contract != requested) throw Error(ErrorCode::ContractMismatch, "Receiver contract does not match");
      dispatch = &receiver->dispatch;
    } else {
      dispatch = &RequireHostService(services_, requested).dispatch;
    }
    std::stop_source callback;
    std::stop_callback stop_exchange(stop, [&] { callback.request_stop(); });
    std::stop_callback stop_callback(callback_stop_.get_token(), [&] { callback.request_stop(); });
    reply.payload = RunHostCallback(callbacks_, *dispatch, frame.operation,
                                    Bytes(request.request.begin(), request.request.end()), deadline, callback.get_token());
  } catch (const Error& error) {
    reply = ErrorReply(callback_stop_.stop_requested()
        ? Error(ErrorCode::Closed, "The plugin generation is closing") : error);
  } catch (...) {
    reply = ErrorReply(Error(ErrorCode::PluginFailed, "Host callback implementation failed"));
  }
  reply.kind = FrameKind::ServiceReply;
  reply.call = frame.call;
  reply.callback = frame.callback;
  reply.instance = frame.instance;
  reply.receiver = frame.receiver;
  return reply;
}

Frame Worker::Request(Frame request, std::chrono::milliseconds timeout, const ReceiverRegistration* receiver,
                      std::stop_token stop, bool wait_for_slot) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24)) {
    throw Error(ErrorCode::InvalidArgument, "Worker timeout must be between 1 ms and 24 hours");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const bool shutdown = request.code == static_cast<std::uint32_t>(Command::Shutdown);
  const bool activate = request.code == static_cast<std::uint32_t>(Command::Activate);
  {
    std::unique_lock lock(requests_);
    if (!wait_for_slot && request_active_ && request_maintenance_) {
      request_ready_.wait_until(lock, deadline, [&] { return !request_active_ || !request_maintenance_ || !active_; });
    }
    if (!wait_for_slot && request_active_) throw Error(ErrorCode::Busy, "The worker has an active invocation");
    request_ready_.wait_until(lock, stop, deadline, [&] {
      return !request_active_ || !active_ || (shutting_down_ && !shutdown);
    });
    // A queued cancellation releases only this call; the active exchange keeps its worker.
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The call was cancelled");
    if (std::chrono::steady_clock::now() >= deadline) throw Error(ErrorCode::DeadlineExceeded, "The request expired in the worker queue");
    if (!active_ || (shutting_down_ && !shutdown)) throw Error(ErrorCode::Closed, "The plugin generation is closed");
    request_active_ = true;
    request_maintenance_ = request.code == static_cast<std::uint32_t>(Command::Close) || shutdown;
  }
  RequestSlot slot{[this] {
    std::lock_guard lock(requests_);
    request_active_ = false;
    request_ready_.notify_all();
  }};
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (!Pid()) {
    Stop();
    throw Error(ErrorCode::WorkerExited, "The plugin worker exited");
  }

  // Several handles can ask for the same activation. Check under the worker request slot.
  if (activate && activated_) return {};
  Frame reply{};
  try {
    // A caller cancellation retires an active exchange. External worker shutdown also wakes any callback wait.
    std::stop_callback interrupt(stop, [this] {
      try {
        PostTransportCancellation([worker = weak_from_this()] {
          if (const auto active = worker.lock()) active->Stop();
        });
      } catch (...) {
        Stop();
      }
    });
    CheckExchange(deadline, stop);
    CheckExchange(deadline, generation_stop_.get_token());
    if (next_call_ == (std::numeric_limits<std::uint32_t>::max)()) {
      throw Error(ErrorCode::Closed, "The worker has exhausted its invocation IDs");
    }
    request.call = ++next_call_;
    request.receiver = receiver ? 1u : 0u;
    if (activate) request.payload = startup_.Encode();
    reply = Exchange(request, deadline, generation_stop_.get_token(), receiver);
    CheckExchange(deadline, generation_stop_.get_token());
  } catch (const Error& error) {
    // Retire a failed exchange so a late reply cannot be read as the next request's response.
    const bool closed = !active_;
    Stop();
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The call was cancelled");
    if (closed && error.Code() != ErrorCode::DeadlineExceeded) {
      throw Error(ErrorCode::Closed, "The plugin generation is closed");
    }
    throw;
  } catch (...) {
    Stop();
    throw;
  }
  if (stop.stop_requested()) {
    Stop();
    throw Error(ErrorCode::Cancelled, "The call was cancelled");
  }
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (request.code == static_cast<std::uint32_t>(Command::Invoke) && callback_stop_.stop_requested()) {
    throw Error(ErrorCode::Closed, "The plugin generation is closing");
  }

  if (reply.kind == FrameKind::Reply && reply.call == request.call && !reply.callback && !reply.operation &&
      !reply.receiver && !reply.synchronous) {
    if (reply.code == 0) {
      const auto valid = (shutdown || activate)
          ? reply.instance == 0 && reply.payload.empty()
          : reply.instance != 0 && (request.instance == 0 || request.instance == reply.instance);
      if (valid) {
        if (activate) activated_ = true;
        return reply;
      }
    } else if (IsErrorCode(reply.code) && reply.instance == 0 &&
               IsUtf8(AsText(reply.payload))) {
      if (activate) Stop();
      throw Error(static_cast<ErrorCode>(reply.code), std::string(AsText(reply.payload)));
    }
  }
  Stop();
  throw Error(ErrorCode::Protocol, "Invalid worker response");
}

void Worker::ValidateFactories(std::stop_token stop) {
  Request({static_cast<std::uint32_t>(Command::Activate), 0, 0, {}}, DefaultTimeout, nullptr, stop);
  // Exercise each factory on the candidate's activation before the manager commits the package.
  for (const auto& extension : startup_.extensions) {
    auto factory = ExtensionPayload(extension.point, extension.ordinal);
    const auto reply = Request({static_cast<std::uint32_t>(Command::Create), 0, 0, std::move(factory)},
                               DefaultTimeout, nullptr, stop);
    Request({static_cast<std::uint32_t>(Command::Close), reply.instance, 0, {}}, DefaultTimeout, nullptr, stop);
  }
}

void Worker::BeginShutdown() noexcept {
  {
    std::lock_guard lock(requests_);
    shutting_down_ = true;
  }
  request_ready_.notify_all();
  callback_stop_.request_stop();
}

void Worker::Shutdown(std::chrono::milliseconds timeout) noexcept {
  std::call_once(shutdown_, [&] {
    if (!active_) return;
    BeginShutdown();
    // Give the active exchange and the teardown hook one bounded shutdown budget.
    try {
      Request({static_cast<std::uint32_t>(Command::Shutdown), 0, 0, {}}, timeout);
    } catch (...) {
      // Teardown is best effort. A stuck or disconnected worker still has to be retired.
    }
    Stop();
  });
}

} // namespace huxerui::pluginkit::detail
