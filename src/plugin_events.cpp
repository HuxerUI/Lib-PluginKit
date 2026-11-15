#include "plugin_events.h"

#include <algorithm>

namespace huxerui::pluginkit {
namespace detail {

struct PluginEventSubscriber {
  // Only the owning application thread reads or clears the handler.
  std::shared_ptr<PluginEvents::Callback> callback;
  std::uint64_t after;
};

namespace {

PluginEvent MakeEvent(const PluginIdentity& package, PluginEventKind kind) {
  PluginEvent event{};
  event.kind = kind;
  event.time = std::chrono::system_clock::now();
  event.package = package;
  return event;
}

PluginFailure DescribeFailure(PluginFailureStage stage, ErrorCode code, std::string_view message) {
  constexpr std::size_t limit = 1024;
  auto end = std::min(message.size(), limit);
  while (end < message.size() && end && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80) --end;
  message = message.substr(0, end);
  if (message.empty() || !IsUtf8(message)) message = ErrorName(code);
  return {stage, code, std::string(message), {}, {}};
}

PluginFailure DescribeFailure(PluginFailureStage stage, std::exception_ptr error, ErrorCode fallback) {
  try {
    std::rethrow_exception(error);
  } catch (const Error& failure) {
    return DescribeFailure(stage, failure.Code(), failure.what());
  } catch (const std::exception& failure) {
    return DescribeFailure(stage, fallback, failure.what());
  } catch (...) {
    return DescribeFailure(stage, fallback, {});
  }
}

} // namespace

std::weak_ptr<PluginEventSubscriber> PluginEvents::Subscribe(Callback callback) {
  if (!callback) throw Error(ErrorCode::InvalidArgument, "Expected a plugin event callback");
  auto handler = std::make_shared<Callback>(std::move(callback));
  std::lock_guard lock(mutex_);
  if (closed_) throw Error(ErrorCode::Closed, "The plugin manager is closed");
  std::erase_if(subscribers_, [](const auto& subscriber) { return !subscriber->callback; });
  auto subscriber = std::make_shared<PluginEventSubscriber>(PluginEventSubscriber{std::move(handler), sequence_});
  subscribers_.push_back(subscriber);
  return subscriber;
}

PluginDiagnostics PluginEvents::Snapshot(PluginInfo plugin, bool restart_required) const {
  std::lock_guard lock(mutex_);
  PluginDiagnostics result{std::move(plugin), PluginRuntimeState::Inactive, {}, restart_required, {}};
  if (const auto found = records_.find(result.plugin.package.id); found != records_.end()) {
    result.last_failure = found->second.last_failure;
    if (const auto& run = found->second.run) {
      result.state = run->state_;
      if (!run->Finished()) result.generation = run->generation_;
      else result.plugin.worker_pid.reset();
    }
  }
  return result;
}

void PluginEvents::Close() noexcept {
  std::vector<std::shared_ptr<PluginEventSubscriber>> subscribers;
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    subscribers.swap(subscribers_);
    pending_.clear();
    records_.clear();
  }
  for (const auto& subscriber : subscribers) subscriber->callback.reset();
}

std::shared_ptr<PluginRun> PluginEvents::Start(PluginIdentity package, bool provisional) {
  std::lock_guard lock(mutex_);
  auto run = std::shared_ptr<PluginRun>(new PluginRun(weak_from_this(), std::move(package), ++generation_, !provisional));
  if (!closed_ && !provisional) records_[run->package_.id].run = run;
  return run;
}

void PluginEvents::CatalogChanged(const PluginIdentity& package, PluginCatalogChange change,
                                  const std::shared_ptr<PluginRun>& promoted) noexcept try {
  std::lock_guard lock(mutex_);
  if (closed_) return;
  if (change == PluginCatalogChange::Uninstalled) records_.erase(package.id);
  if (change == PluginCatalogChange::Disabled) {
    if (const auto found = records_.find(package.id); found != records_.end() && found->second.run) {
      auto& run = found->second.run;
      if (run->Finished()) run.reset();
      else run->BeginStopLocked(PluginStopReason::Disabled);
    }
  }
  if (change == PluginCatalogChange::Upgraded) {
    if (promoted) records_[package.id].run = promoted;
    else if (const auto found = records_.find(package.id); found != records_.end()) found->second.run.reset();
  }

  auto event = promoted ? promoted->Event(PluginEventKind::CatalogChanged)
                        : MakeEvent(package, PluginEventKind::CatalogChanged);
  event.change = change;
  PublishLocked(std::move(event));

  if (promoted) {
    // A probe becomes observable only after its package and data have committed.
    promoted->published_ = true;
    if (promoted->state_ == PluginRuntimeState::Active) {
      PublishRuntimeLocked(*promoted, promoted->Event(PluginEventKind::Activated));
    } else if (promoted->candidate_failure_) {
      PublishRuntimeLocked(*promoted, std::move(*promoted->candidate_failure_));
      promoted->candidate_failure_.reset();
    }
  }
} catch (...) {
  // Reporting cannot invalidate a committed change.
}

void PluginEvents::UpgradeFailed(const PluginIdentity& package, PluginFailureStage stage, std::exception_ptr error,
                                 const std::shared_ptr<PluginRun>& candidate) noexcept try {
  std::lock_guard lock(mutex_);
  if (closed_) return;
  auto event = candidate ? candidate->Event(PluginEventKind::UpgradeFailed)
                         : MakeEvent(package, PluginEventKind::UpgradeFailed);
  if (stage == PluginFailureStage::ValidateFactories && candidate && !candidate->activated_) {
    stage = PluginFailureStage::Activate;
  }
  event.failure = DescribeFailure(stage, error, ErrorCode::Io);
  if (candidate && candidate->candidate_failure_ && event.failure->code == ErrorCode::WorkerExited) {
    const auto& exit = *candidate->candidate_failure_->failure;
    event.failure->exit_code = exit.exit_code;
    event.failure->signal = exit.signal;
  }
  PublishLocked(std::move(event), &records_[package.id]);
} catch (...) {
}

void PluginEvents::PublishRuntimeLocked(const PluginRun& run, PluginEvent event) {
  const auto found = records_.find(run.package_.id);
  auto* record = found != records_.end() && found->second.run.get() == &run ? &found->second : nullptr;
  PublishLocked(std::move(event), record);
}

void PluginEvents::PublishLocked(PluginEvent event, PluginRecord* record) {
  if (closed_) return;
  event.sequence = ++sequence_;
  if (record && event.failure) record->last_failure = event;
  if (subscribers_.empty()) return;
  const bool post = pending_.empty();
  pending_.push_back(std::move(event));
  if (post) {
    try {
      tasks_.Post([events = weak_from_this()] {
        if (const auto owner = events.lock()) owner->Deliver();
      });
    } catch (...) {
      pending_.clear();
    }
  }
}

void PluginEvents::Deliver() {
  std::deque<PluginEvent> batch;
  std::vector<std::shared_ptr<PluginEventSubscriber>> subscribers;
  {
    std::lock_guard lock(mutex_);
    batch.swap(pending_);
    std::erase_if(subscribers_, [](const auto& subscriber) { return !subscriber->callback; });
    subscribers = subscribers_;
  }
  // Keep each handler alive until it returns, even if it releases its own subscription.
  for (const auto& event : batch) {
    for (const auto& subscriber : subscribers) {
      auto callback = subscriber->callback;
      if (!callback || event.sequence <= subscriber->after) continue;
      try {
        (*callback)(event);
      } catch (...) {
        // A host observer cannot interrupt delivery to the others.
      }
    }
  }
}

PluginRun::PluginRun(std::weak_ptr<PluginEvents> events, PluginIdentity package, std::uint64_t generation, bool published)
    : events_(std::move(events)), package_(std::move(package)), generation_(generation), published_(published) {}

PluginEvent PluginRun::Event(PluginEventKind kind) const {
  auto event = MakeEvent(package_, kind);
  event.generation = generation_;
  event.worker_pid = pid_;
  return event;
}

bool PluginRun::Finished() const noexcept {
  return state_ == PluginRuntimeState::Inactive || state_ == PluginRuntimeState::Faulted;
}

void PluginRun::SetPid(std::uint32_t pid) noexcept {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    pid_ = pid;
  }
}

void PluginRun::Activated() noexcept try {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (state_ != PluginRuntimeState::Activating) return;
    activated_ = true;
    state_ = PluginRuntimeState::Active;
    if (published_) events->PublishRuntimeLocked(*this, Event(PluginEventKind::Activated));
  }
} catch (...) {
}

void PluginRun::ActivationFailed(PluginFailureStage stage, std::exception_ptr error) noexcept try {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (state_ != PluginRuntimeState::Activating || disconnected_) return;
    auto event = Event(PluginEventKind::ActivationFailed);
    event.failure = DescribeFailure(stage, error, ErrorCode::PluginFailed);
    FinishLocked(*events, std::move(event));
  }
} catch (...) {
}

void PluginRun::Disconnected() noexcept {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (!stop_reason_) disconnected_ = true;
  }
}

bool PluginRun::BeginStopLocked(PluginStopReason reason) noexcept {
  if (Finished() || stop_reason_ || disconnected_) return false;
  state_ = PluginRuntimeState::Stopping;
  stop_reason_ = reason;
  return true;
}

void PluginRun::BeginStop(PluginStopReason reason, std::optional<ErrorCode> code, std::string_view message) noexcept try {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (BeginStopLocked(reason) && code) {
      stop_failure_ = DescribeFailure(PluginFailureStage::Exchange, *code, message);
    }
  }
} catch (...) {
}

void PluginRun::Stopped() noexcept try {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (Finished()) return;
    auto event = Event(PluginEventKind::Stopped);
    event.stop_reason = stop_reason_.value_or(PluginStopReason::Shutdown);
    FinishLocked(*events, std::move(event));
  }
} catch (...) {
}

void PluginRun::WorkerExited(std::optional<std::uint32_t> exit_code, std::optional<int> signal) noexcept try {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    if (Finished()) return;
    if (!stop_reason_) disconnected_ = true;
    auto event = Event(stop_reason_ ? PluginEventKind::Stopped : PluginEventKind::WorkerExited);
    event.stop_reason = stop_reason_;
    event.failure = stop_reason_ ? std::move(stop_failure_)
                                 : DescribeFailure(PluginFailureStage::WorkerExit, ErrorCode::WorkerExited,
                                                   "The plugin worker exited unexpectedly");
    if (event.failure) {
      event.failure->exit_code = exit_code;
      event.failure->signal = signal;
    }
    FinishLocked(*events, std::move(event));
  }
} catch (...) {
}

bool PluginRun::ExitedUnexpectedly() const noexcept {
  if (const auto events = events_.lock()) {
    std::lock_guard lock(events->mutex_);
    return disconnected_ && state_ == PluginRuntimeState::Faulted;
  }
  return false;
}

void PluginRun::FinishLocked(PluginEvents& events, PluginEvent event) {
  state_ = event.failure ? PluginRuntimeState::Faulted : PluginRuntimeState::Inactive;
  if (!published_) {
    if (event.failure) candidate_failure_ = std::move(event);
  } else if (event.kind != PluginEventKind::Stopped || activated_ || pid_) {
    events.PublishRuntimeLocked(*this, std::move(event));
  }
}

} // namespace detail

PluginEventSubscription::PluginEventSubscription(std::weak_ptr<detail::PluginEventSubscriber> subscriber)
    : subscriber_(std::move(subscriber)) {}

PluginEventSubscription& PluginEventSubscription::operator=(PluginEventSubscription&& other) noexcept {
  if (this != &other) {
    Release();
    subscriber_ = std::move(other.subscriber_);
  }
  return *this;
}

PluginEventSubscription::~PluginEventSubscription() { Release(); }

void PluginEventSubscription::Release() noexcept {
  if (const auto subscriber = std::exchange(subscriber_, {}).lock()) subscriber->callback.reset();
}

} // namespace huxerui::pluginkit
