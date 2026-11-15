#include "worker_process.h"
#include "run_transport.h"
#include "stream_io.h"

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

template<class Result>
struct CallbackReply {
  std::mutex mutex;
  std::condition_variable_any ready;
  std::optional<Result> result;
  std::exception_ptr error;
  bool abandoned = false;
  TaskHandle task;
};

template<class Result>
Task<void> ExecuteCallback(std::shared_ptr<CallbackReply<Result>> reply, std::function<Task<Result>()> work) {
  std::optional<Result> result;
  std::exception_ptr error;
  try {
    {
      CallbackScope scope(co_await CurrentTaskExecution{});
      result.emplace(co_await work());
    }
  } catch (...) {
    error = std::current_exception();
  }
  work = {};
  std::lock_guard lock(reply->mutex);
  if (reply->abandoned) co_return;
  reply->result = std::move(result);
  reply->error = std::move(error);
  reply->ready.notify_one();
}

template<class Result>
Result RunHostCallback(const TaskScope& tasks, std::function<Task<Result>()> work, Deadline deadline,
                       std::stop_token stop, CallStreams* streams) {
  auto reply = std::make_shared<CallbackReply<Result>>();
  tasks.Post([tasks, reply, work = std::move(work), deadline, stop]() mutable {
    std::lock_guard lock(reply->mutex);
    if (reply->abandoned || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return;
    try {
      reply->task = tasks.Launch(ExecuteCallback(reply, std::move(work)));
    } catch (...) {
      reply->error = std::current_exception();
      reply->ready.notify_one();
    }
  });
  std::unique_lock lock(reply->mutex);
  if (!reply->ready.wait_until(lock, stop, deadline, [&] { return reply->result.has_value() || reply->error; }) ||
      stop.stop_requested()) {
    reply->abandoned = true;
    auto task = reply->task;
    lock.unlock();
    if (streams) streams->Revoke();
    task.Cancel();
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The invocation was cancelled");
    throw Error(ErrorCode::DeadlineExceeded, "The host callback exceeded the invocation deadline");
  }
  if (reply->error) std::rethrow_exception(reply->error);
  return std::move(*reply->result);
}

Frame MakeStreamReply(const Frame& request, IoResult<Bytes> result) {
  Frame reply{0, request.instance, request.operation, {}, FrameKind::StreamReply, request.call, request.callback};
  reply.streams = request.streams;
  if (result.Succeeded()) {
    reply.payload = std::move(result).Value();
  } else {
    // A native diagnostic can reveal the host's source or destination path.
    const auto text = AsBytes("The host stream operation failed");
    reply.code = static_cast<std::uint32_t>(result.Error().code) + 1;
    reply.payload.assign(text.begin(), text.end());
  }
  return reply;
}

IoResult<Bytes> AcknowledgeStream(IoResult<void> result) {
  if (!result.Succeeded()) return IoResult<Bytes>(std::move(result).Error());
  return IoResult<Bytes>(Bytes{});
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

Frame Worker::DispatchCallback(Frame frame, Deadline deadline, std::stop_token stop,
                               const ReceiverRegistration* receiver, const std::shared_ptr<CallStreams>& streams,
                               StreamIo* sync_io) {
  if (frame.kind == FrameKind::ServiceCall && sync_io) return RejectSynchronousCallback(frame);
  // The transport has no read pending while it waits for a host callback.
  std::stop_source callback;
  std::stop_callback stop_exchange(stop, [&] { callback.request_stop(); });
  std::stop_callback stop_callback(callback_stop_.get_token(), [&] { callback.request_stop(); });
  std::stop_callback worker_exited(exited_stop_.get_token(), [&] { callback.request_stop(); });
  const auto token = callback.get_token();
  try {
    CheckExchange(deadline, token);
    auto reply = frame.kind == FrameKind::StreamCall
        ? DispatchStream(std::move(frame), deadline, token, streams, sync_io)
        : DispatchService(frame, deadline, token, receiver, streams.get());
    CheckExchange(deadline, token);
    return reply;
  } catch (...) {
    if (streams) streams->Revoke();
    if (callback_stop_.stop_requested()) throw Error(ErrorCode::Closed, "The plugin generation is closing");
    if (exited_stop_.stop_requested() && !stop.stop_requested()) {
      throw Error(ErrorCode::WorkerExited, "The plugin worker exited during a host callback");
    }
    throw;
  }
}

Frame Worker::DispatchService(const Frame& frame, Deadline deadline, std::stop_token stop,
                              const ReceiverRegistration* receiver, CallStreams* streams) {
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
    reply.payload = RunHostCallback<Bytes>(callbacks_,
        [call = *dispatch, operation = frame.operation, data = Bytes(request.request.begin(), request.request.end())]() mutable {
          return call(operation, std::move(data));
        }, deadline, stop, streams);
  } catch (const Error& error) {
    reply = ErrorReply(error);
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

Frame Worker::DispatchStream(Frame frame, Deadline deadline, std::stop_token stop,
                             const std::shared_ptr<CallStreams>& streams, StreamIo* sync_io) {
  if (sync_io) {
    return sync_io->Run([streams, request = std::move(frame)] {
      if (request.operation == HPK_READ_STREAM) {
        Bytes bytes(request.read_limit);
        auto count = streams->Read(bytes);
        if (!count.Succeeded()) return MakeStreamReply(request, IoResult<Bytes>(std::move(count).Error()));
        bytes.resize(count.Value());
        return MakeStreamReply(request, IoResult<Bytes>(std::move(bytes)));
      }
      auto result = request.operation == HPK_WRITE_STREAM ? streams->Write(request.payload) : streams->Close();
      return MakeStreamReply(request, AcknowledgeStream(std::move(result)));
    }, deadline, stop);
  }
  return RunHostCallback<Frame>(callbacks_,
      [streams, request = std::move(frame)]() mutable -> Task<Frame> {
        if (request.operation == HPK_READ_STREAM) {
          co_return MakeStreamReply(request, co_await streams->ReadAsync(request.read_limit));
        }
        if (request.operation == HPK_WRITE_STREAM) {
          co_return MakeStreamReply(request, AcknowledgeStream(co_await streams->WriteAsync(std::move(request.payload))));
        }
        co_return MakeStreamReply(request, AcknowledgeStream(co_await streams->CloseAsync()));
      }, deadline, stop, streams.get());
}

void Worker::ObserveRequestFailure(bool activate, const Error& error) noexcept {
  if (error.Code() == ErrorCode::WorkerExited) {
    run_->Disconnected();
  } else if (!shutting_down_) {
    if (activate) {
      run_->ActivationFailed(PluginFailureStage::Activate, std::make_exception_ptr(error));
    } else {
      const auto reason = error.Code() == ErrorCode::DeadlineExceeded
          ? PluginStopReason::DeadlineExceeded : PluginStopReason::TransportError;
      run_->BeginStop(reason, error.Code(), error.what());
    }
  }
}

void Worker::Abort(const Error& error) noexcept {
  ObserveRequestFailure(false, error);
  Stop();
}

void Worker::Activate(Deadline deadline, std::stop_token stop, bool started, bool wait_for_slot) {
  Request({static_cast<std::uint32_t>(Command::Activate), 0, 0, {}}, deadline, nullptr, stop, wait_for_slot, {}, started);
}

Frame Worker::Request(Frame request, std::chrono::milliseconds timeout, const ReceiverRegistration* receiver,
                       std::stop_token stop, bool wait_for_slot, std::shared_ptr<CallStreams> streams) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::hours(24)) {
    throw Error(ErrorCode::InvalidArgument, "Worker timeout must be between 1 ms and 24 hours");
  }
  return Request(std::move(request), std::chrono::steady_clock::now() + timeout, receiver, stop,
                 wait_for_slot, std::move(streams));
}

Frame Worker::Request(Frame request, Deadline deadline, const ReceiverRegistration* receiver,
                       std::stop_token stop, bool wait_for_slot, std::shared_ptr<CallStreams> streams, bool started) {
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
    // The caller that started an idle worker can retire it if its first activation expires.
    // An expired call sharing an existing worker leaves it available to the other callers.
    const bool initial_activation = activate && started && !request_active_ && !activated_ && active_ && !shutting_down_;
    if (!initial_activation) {
      if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The call was cancelled");
      if (std::chrono::steady_clock::now() >= deadline) {
        throw Error(ErrorCode::DeadlineExceeded, "The request expired in the worker queue");
      }
    }
    if (!active_ || (shutting_down_ && !shutdown)) throw Error(ErrorCode::Closed, "The plugin generation is closed");
    request_active_ = true;
    request_maintenance_ = request.code == static_cast<std::uint32_t>(Command::Close) || shutdown;
  }
  RequestSlot slot{[this] {
    std::lock_guard lock(requests_);
    request_active_ = false;
    request_ready_.notify_all();
  }};
  if (streams) streams->SetDeadline(deadline);
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (!Pid()) {
    run_->Disconnected();
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
          if (const auto active = worker.lock()) {
            active->run_->BeginStop(PluginStopReason::Cancelled);
            active->Stop();
          }
        });
      } catch (...) {
        run_->BeginStop(PluginStopReason::Cancelled);
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
    request.streams = streams ? streams->Slots() : 0u;
    if (activate) request.payload = startup_.Encode();
    reply = Exchange(request, deadline, generation_stop_.get_token(), receiver, streams);
    CheckExchange(deadline, generation_stop_.get_token());
  } catch (const Error& error) {
    // Retire a failed exchange so a late reply cannot be read as the next request's response.
    const bool closed = !active_;
    if (stop.stop_requested()) run_->BeginStop(PluginStopReason::Cancelled);
    else ObserveRequestFailure(activate, error);
    Stop();
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The call was cancelled");
    if (closed && error.Code() != ErrorCode::DeadlineExceeded) {
      // Opening a new generation from the exit event may have retired this worker already.
      if (!shutting_down_ && run_->ExitedUnexpectedly()) {
        throw Error(ErrorCode::WorkerExited, "The plugin worker exited");
      }
      throw Error(ErrorCode::Closed, "The plugin generation is closed");
    }
    throw;
  } catch (...) {
    if (activate) run_->ActivationFailed(PluginFailureStage::Activate, std::current_exception());
    else run_->BeginStop(PluginStopReason::TransportError, ErrorCode::PluginFailed, "The worker exchange failed");
    Stop();
    throw;
  }
  if (stop.stop_requested()) {
    run_->BeginStop(PluginStopReason::Cancelled);
    Stop();
    throw Error(ErrorCode::Cancelled, "The call was cancelled");
  }
  if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
  if (request.code == static_cast<std::uint32_t>(Command::Invoke) && callback_stop_.stop_requested()) {
    throw Error(ErrorCode::Closed, "The plugin generation is closing");
  }

  if (reply.kind == FrameKind::Reply && reply.call == request.call && !reply.callback && !reply.operation &&
      !reply.receiver && !reply.synchronous && !reply.streams) {
    if (reply.code == 0) {
      const auto valid = (shutdown || activate)
          ? reply.instance == 0 && reply.payload.empty()
          : reply.instance != 0 && (request.instance == 0 || request.instance == reply.instance);
      if (valid) {
        if (activate) {
          activated_ = true;
          run_->Activated();
        }
        if (streams) streams->CheckOutput();
        return reply;
      }
    } else if (IsErrorCode(reply.code) && reply.instance == 0 &&
               IsUtf8(AsText(reply.payload))) {
      const Error error(static_cast<ErrorCode>(reply.code), std::string(AsText(reply.payload)));
      if (activate) {
        run_->ActivationFailed(PluginFailureStage::Activate, std::make_exception_ptr(error));
        Stop();
      }
      throw error;
    }
  }
  const Error error(ErrorCode::Protocol, "Invalid worker response");
  ObserveRequestFailure(activate, error);
  Stop();
  throw error;
}

void Worker::ValidateFactories(std::stop_token stop) {
  Activate(std::chrono::steady_clock::now() + DefaultTimeout, stop, true);
  // Exercise each factory on the candidate's activation before the manager commits the package.
  for (const auto& extension : startup_.extensions) {
    auto factory = ExtensionPayload(extension.point, extension.ordinal);
    const auto reply = Request({static_cast<std::uint32_t>(Command::Create), 0, 0, std::move(factory)},
                               DefaultTimeout, nullptr, stop);
    Request({static_cast<std::uint32_t>(Command::Close), reply.instance, 0, {}}, DefaultTimeout, nullptr, stop);
  }
}

void Worker::BeginShutdown(PluginStopReason reason) noexcept {
  run_->BeginStop(reason);
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
