#pragma once

#include <huxerui/task.h>

#include <stop_token>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace huxerui::pluginkit::detail {

// Stop callbacks can terminate a process. Dispatch the signal away from the application thread.
inline void PostTransportCancellation(std::function<void()> work) {
  struct Queue {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::queue<std::function<void()>> pending;
    std::jthread thread{[this](std::stop_token stop) {
      for (;;) {
        std::function<void()> work;
        {
          std::unique_lock lock(mutex);
          if (!ready.wait(lock, stop, [&] { return !pending.empty(); })) return;
          work = std::move(pending.front());
          pending.pop();
        }
        work();
      }
    }};
  };
  static Queue queue;
  std::lock_guard lock(queue.mutex);
  queue.pending.push(std::move(work));
  queue.ready.notify_one();
}

/// Keeps transport arguments alive independently of the waiting HuxerUI task.
template<class Work>
auto RunTransport(Work work) {
  using Value = std::invoke_result_t<Work&, std::stop_token>;
  return AwaitCallback<Value>([work = std::move(work)](CallbackCompletion<Value> complete) mutable {
    std::stop_source stop;
    auto cancel = std::function<void()>([stop]() mutable noexcept { stop.request_stop(); });
    std::thread([work = std::move(work), complete, token = stop.get_token()]() mutable {
      try {
        if constexpr (std::is_void_v<Value>) {
          work(token);
          complete.Complete();
        } else {
          complete.Complete(work(token));
        }
      } catch (...) {
        complete.Fail(std::current_exception());
      }
    }).detach();
    return cancel;
  });
}

} // namespace huxerui::pluginkit::detail
