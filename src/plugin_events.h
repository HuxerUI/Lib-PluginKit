#pragma once

#include <huxerui/pluginkit.h>

#include <deque>
#include <exception>
#include <map>
#include <mutex>

namespace huxerui::pluginkit::detail {

class PluginEvents;

// The events mutex protects each run. Process monitors never need the manager's catalog lock.
class PluginRun {
public:
  void SetPid(std::uint32_t pid) noexcept;
  void Activated() noexcept;
  void ActivationFailed(PluginFailureStage stage, std::exception_ptr error) noexcept;
  void Disconnected() noexcept;
  void BeginStop(PluginStopReason reason, std::optional<ErrorCode> code = {}, std::string_view message = {}) noexcept;
  void Stopped() noexcept;
  void WorkerExited(std::optional<std::uint32_t> exit_code, std::optional<int> signal = {}) noexcept;
  bool ExitedUnexpectedly() const noexcept;

private:
  friend class PluginEvents;
  PluginRun(std::weak_ptr<PluginEvents> events, PluginIdentity package, std::uint64_t generation, bool published);
  PluginEvent Event(PluginEventKind kind) const;
  bool Finished() const noexcept;
  bool BeginStopLocked(PluginStopReason reason) noexcept;
  void FinishLocked(PluginEvents& events, PluginEvent event);

  const std::weak_ptr<PluginEvents> events_;
  const PluginIdentity package_;
  const std::uint64_t generation_;
  bool published_;
  PluginRuntimeState state_ = PluginRuntimeState::Activating;
  std::optional<std::uint32_t> pid_;
  bool activated_ = false;
  bool disconnected_ = false;
  std::optional<PluginStopReason> stop_reason_;
  std::optional<PluginFailure> stop_failure_;
  std::optional<PluginEvent> candidate_failure_;
};

class PluginEvents : public std::enable_shared_from_this<PluginEvents> {
public:
  using Callback = std::function<void(const PluginEvent&)>;

  explicit PluginEvents(TaskScope tasks) : tasks_(std::move(tasks)) {}
  std::weak_ptr<PluginEventSubscriber> Subscribe(Callback callback);
  PluginDiagnostics Snapshot(PluginInfo plugin, bool restart_required) const;
  void Close() noexcept;

  std::shared_ptr<PluginRun> Start(PluginIdentity package, bool provisional = false);
  void CatalogChanged(const PluginIdentity& package, PluginCatalogChange change,
                      const std::shared_ptr<PluginRun>& promoted = {}) noexcept;
  void UpgradeFailed(const PluginIdentity& package, PluginFailureStage stage, std::exception_ptr error,
                     const std::shared_ptr<PluginRun>& candidate = {}) noexcept;

private:
  friend class PluginRun;

  struct PluginRecord {
    std::shared_ptr<PluginRun> run;
    std::optional<PluginEvent> last_failure;
  };

  void PublishRuntimeLocked(const PluginRun& run, PluginEvent event);
  void PublishLocked(PluginEvent event, PluginRecord* record = nullptr);
  void Deliver();

  const TaskScope tasks_;
  mutable std::mutex mutex_;
  std::map<std::string, PluginRecord, std::less<>> records_;
  std::vector<std::shared_ptr<PluginEventSubscriber>> subscribers_;
  std::deque<PluginEvent> pending_;
  std::uint64_t sequence_ = 0;
  std::uint64_t generation_ = 0;
  bool closed_ = false;
};

} // namespace huxerui::pluginkit::detail
