#pragma once

#include "transport.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace huxerui::pluginkit::detail {

// One executor serves the synchronous stream operations of an exchange. A blocked native operation
// retains its arguments independently, so retiring the exchange never joins an uninterruptible I/O call.
class StreamIo {
public:
  StreamIo() = default;
  StreamIo(const StreamIo&) = delete;
  StreamIo& operator=(const StreamIo&) = delete;

  ~StreamIo() {
    if (!thread_.joinable()) return;
    {
      std::lock_guard lock(state_->mutex);
      state_->finished = true;
      state_->work = {};
    }
    state_->ready.notify_one();
    if (abandoned_) thread_.detach();
    else thread_.join();
  }

  Frame Run(std::function<Frame()> work, Deadline deadline, std::stop_token stop) {
    if (!thread_.joinable()) {
      state_ = std::make_shared<State>();
      thread_ = std::thread(Serve, state_);
    }
    std::unique_lock lock(state_->mutex);
    if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The stream operation was cancelled");
    if (std::chrono::steady_clock::now() >= deadline) {
      throw Error(ErrorCode::DeadlineExceeded, "The stream operation exceeded the invocation deadline");
    }
    state_->work = std::move(work);
    state_->ready.notify_one();
    if (!state_->complete.wait_until(lock, stop, deadline, [&] { return state_->reply || state_->error; })) {
      abandoned_ = true;
      if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "The stream operation was cancelled");
      throw Error(ErrorCode::DeadlineExceeded, "The stream operation exceeded the invocation deadline");
    }
    if (auto error = std::exchange(state_->error, {})) std::rethrow_exception(error);
    auto reply = std::move(*state_->reply);
    state_->reply.reset();
    return reply;
  }

private:
  struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable_any complete;
    std::function<Frame()> work;
    std::optional<Frame> reply;
    std::exception_ptr error;
    bool finished = false;
  };

  static void Serve(std::shared_ptr<State> state) {
    std::unique_lock lock(state->mutex);
    for (;;) {
      state->ready.wait(lock, [&] { return state->finished || state->work; });
      if (state->finished) return;
      auto work = std::exchange(state->work, {});
      lock.unlock();
      std::optional<Frame> reply;
      std::exception_ptr error;
      try {
        reply.emplace(work());
      } catch (...) {
        error = std::current_exception();
      }
      work = {};
      lock.lock();
      if (state->finished) return;
      state->reply = std::move(reply);
      state->error = std::move(error);
      state->complete.notify_one();
    }
  }

  std::shared_ptr<State> state_;
  std::thread thread_;
  bool abandoned_ = false;
};

} // namespace huxerui::pluginkit::detail
