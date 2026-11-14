#pragma once

#include "platform_files.h"
#include "transport.h"

#include <huxerui/pluginkit.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#if !defined(_WIN32)
#include <thread>
#endif

namespace huxerui::pluginkit::detail {

class Worker : public std::enable_shared_from_this<Worker> {
public:
  struct Service {
    std::string id;
    std::uint32_t version;
    std::string fingerprint;
    ServiceDispatch dispatch;
  };

  Worker(const std::filesystem::path& executable, const std::filesystem::path& module_path,
         const Handle& verified_module, std::vector<Service> services, TaskScope callbacks,
         PluginStartup startup);
  ~Worker();

  std::optional<std::uint32_t> Pid() const noexcept;
  Frame Request(Frame request, std::chrono::milliseconds timeout, const ReceiverRegistration* receiver = nullptr,
                std::stop_token stop = {}, bool wait_for_slot = true);
  void ValidateFactories(std::stop_token stop);
  void Shutdown(std::chrono::milliseconds timeout = DefaultTimeout) noexcept;
  /// Waits for the process to exit after shutdown, including when handles still reference this Worker.
  void WaitForExit(std::chrono::milliseconds timeout = DefaultTimeout);
  void BeginShutdown() noexcept;
  void Stop() noexcept;
  bool Available() const noexcept { return active_ && !shutting_down_; }

private:
  Frame Exchange(const Frame& request, Deadline deadline, std::stop_token stop,
                 const ReceiverRegistration* receiver);
  Frame DispatchService(const Frame& request, Deadline deadline, std::stop_token stop,
                        const ReceiverRegistration* receiver);

  template<class Transact>
  Frame ExchangeFrames(const Frame& request, Deadline deadline, std::stop_token stop,
                       const ReceiverRegistration* receiver, Transact transact) {
    auto frame = transact(request);
    std::uint32_t callback = 0;
    for (;;) {
      CheckExchange(deadline, stop);
      if (frame.kind == FrameKind::Reply) return frame;
      if (frame.kind != FrameKind::ServiceCall || request.code != static_cast<std::uint32_t>(Command::Invoke) ||
          frame.call != request.call || !frame.callback || frame.callback != ++callback ||
          frame.instance != request.instance || !frame.operation || frame.code || frame.synchronous) {
        throw Error(ErrorCode::Protocol, "Invalid host service request");
      }
      if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
      auto reply = request.synchronous
          ? RejectSynchronousCallback(frame)
          : DispatchService(frame, deadline, stop, receiver);
      CheckExchange(deadline, stop);
      if (!active_) throw Error(ErrorCode::Closed, "The plugin generation is closed");
      frame = transact(reply);
    }
  }

  static void CheckExchange(Deadline deadline, std::stop_token stop);
  static Frame RejectSynchronousCallback(const Frame& frame);

  const std::vector<Service> services_;
  const TaskScope callbacks_;
  const PluginStartup startup_;
  bool activated_ = false;
  std::uint32_t next_call_ = 0;
  std::atomic_bool active_{true};
  std::mutex requests_;
  std::condition_variable_any request_ready_;
  bool request_active_ = false;
  bool request_maintenance_ = false;
  std::atomic_bool shutting_down_{false};
  std::once_flag shutdown_;
  std::mutex stop_;
  std::stop_source generation_stop_;
  std::stop_source callback_stop_;

#if defined(_WIN32)
  Handle process_;
  Handle to_worker_;
  Handle from_worker_;
  Handle job_;
  std::uint32_t pid_ = 0;
#else
  void StartReaper(pid_t child);
  void Reap(pid_t child) noexcept;

  Handle socket_;
  std::atomic<pid_t> pid_{0};
  std::condition_variable exited_;
  std::thread reaper_;
#endif
};

void RejectServiceReentry();
void RejectServiceReentry(const TaskExecution& execution);

} // namespace huxerui::pluginkit::detail
