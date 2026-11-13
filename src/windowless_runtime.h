#pragma once

#include <huxerui/app.h>

#include <condition_variable>
#include <mutex>
#include <queue>

namespace huxerui::pluginkit::detail {

/// Runs the SDK application dispatcher on the calling thread without creating a window.
class WindowlessRuntime final : public Runtime {
public:
  WindowlessRuntime() : Runtime([this](std::function<void()> work) {
    std::lock_guard lock(mutex_);
    pending_.push(std::move(work));
    ready_.notify_one();
  }) {}

  ~WindowlessRuntime() override { Retire(); }

  void Run(const Application& application, Task<void> task) {
    InitializeApplication(application, std::nullopt);
    (void)UseApplicationTaskScope().Launch(std::move(task));
    while (!stopped_) {
      std::function<void()> work;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !pending_.empty(); });
        work = std::move(pending_.front());
        pending_.pop();
      }
      work();
    }
  }

private:
  void OnRuntimeStopped() override { stopped_ = true; }

  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> pending_;
  bool stopped_ = false;
};

inline View WindowlessRoot() { return {}; }

} // namespace huxerui::pluginkit::detail
