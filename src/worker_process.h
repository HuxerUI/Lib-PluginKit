#pragma once

#include "platform_files.h"
#include "transport.h"

#include <pluginkit/manager.h>

#include <atomic>
#include <mutex>
#if !defined(_WIN32)
#include <thread>
#endif

namespace pluginkit::detail {

class Worker {
public:
  Worker(const std::filesystem::path& executable, const std::filesystem::path& module_path,
         const Handle& verified_module, ContractInfo contract);
  ~Worker();

  std::optional<std::uint32_t> Pid() const noexcept;
  Frame Request(Frame request, std::chrono::milliseconds timeout);
  void Stop() noexcept;

private:
  Frame Exchange(const Frame& request, Deadline deadline);

  std::atomic_bool active_{true};
  std::timed_mutex requests_;
  std::mutex stop_;

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
  std::thread reaper_;
#endif
};

} // namespace pluginkit::detail
