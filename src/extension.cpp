#include "extension.h"
#include "plugin_module.h"
#include "run_transport.h"
#include "worker_process.h"

#include <algorithm>
#include <deque>

namespace huxerui::pluginkit::detail {

const RegisteredService& RequireHostService(std::span<const RegisteredService> services, ContractInfo contract) {
  const auto service = std::ranges::find(services, contract.id, &RegisteredService::id);
  if (service == services.end()) throw Error(ErrorCode::ServiceDenied, "Host service is not authorized for this package");
  if (ContractInfo{service->id, service->version, service->fingerprint} != contract) {
    throw Error(ErrorCode::ContractMismatch, "Host service contract does not match");
  }
  return *service;
}

namespace {

Error Closed() { return Error(ErrorCode::Closed, "The extension instance is closed"); }
Error Expired() { return Error(ErrorCode::DeadlineExceeded, "The plugin call exceeded its deadline"); }

void CheckDeadline(Deadline deadline) {
  if (std::chrono::steady_clock::now() >= deadline) throw Expired();
}

std::chrono::milliseconds Remaining(Deadline deadline) {
  const auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining <= Deadline::duration::zero()) throw Expired();
  return std::chrono::ceil<std::chrono::milliseconds>(remaining);
}

struct RemoteInstance {
  std::shared_ptr<Worker> worker;
  std::uint32_t id;
};

class WorkerExtension final : public ExtensionTarget, public std::enable_shared_from_this<WorkerExtension> {
public:
  WorkerExtension(std::function<std::shared_ptr<Worker>(Deadline, std::stop_token)> start,
                  std::chrono::milliseconds timeout, std::stop_token admission, Bytes factory)
      : start_(std::move(start)), timeout_(timeout), admission_(admission), factory_(std::move(factory)) {}

  ExecutionMode Mode() const noexcept override { return ExecutionMode::WorkerProcess; }

  void Check() const override {
    if (released_ || admission_.stop_requested()) throw Closed();
    std::lock_guard lock(mutex_);
    if (instance_ && !instance_->worker->Available()) throw Closed();
  }

  void Release() noexcept override {
    if (released_.exchange(true)) return;
    RetireWhenIdle();
  }

  Bytes CallWorker(std::uint32_t operation, Bytes request) override {
    return Invoke(operation, std::move(request), nullptr, true, {});
  }

  Task<Bytes> CallWorker(std::uint32_t operation, Bytes request, std::optional<ReceiverRegistration> receiver) override {
    RejectServiceReentry(co_await CurrentTaskExecution{});
    auto work = [self = shared_from_this(), operation, request = std::move(request), receiver = std::move(receiver)]
        (std::stop_token stop) mutable {
      return self->Invoke(operation, std::move(request), receiver ? &*receiver : nullptr, false, stop);
    };
    co_return co_await RunTransport(std::move(work));
  }

private:
  void RetireWhenIdle() noexcept {
    std::optional<RemoteInstance> instance;
    {
      std::lock_guard lock(mutex_);
      if (!released_ || calls_ != 0) return;
      instance = std::exchange(instance_, {});
    }
    if (!instance) return;
    try {
      Retire(*instance);
    } catch (...) {
      instance->worker->Stop();
    }
  }

  static void Retire(RemoteInstance instance) {
    std::thread([instance = std::move(instance)] {
      try {
        if (instance.worker->Pid()) {
          instance.worker->Request({static_cast<std::uint32_t>(Command::Close), instance.id, 0, {}}, DefaultTimeout);
        }
      } catch (const Error& error) {
        if (error.Code() != ErrorCode::Closed) instance.worker->Stop();
      } catch (...) {
        instance.worker->Stop();
      }
    }).detach();
  }

  RemoteInstance EnsureInstance(Deadline deadline, bool synchronous, std::stop_token stop) {
    std::unique_lock lock(mutex_);
    if (synchronous && creating_) throw Error(ErrorCode::Busy, "The worker instance is being created");
    if (!ready_.wait_until(lock, stop, deadline, [&] { return !creating_; })) {
      if (stop.stop_requested()) throw Error(ErrorCode::Cancelled, "Extension creation was cancelled");
      throw Expired();
    }
    if (released_ || admission_.stop_requested()) throw Closed();
    if (instance_) {
      if (!instance_->worker->Available()) throw Closed();
      return *instance_;
    }
    creating_ = true;
    lock.unlock();
    const auto finish = [&] {
      creating_ = false;
      ready_.notify_all();
    };
    try {
      auto worker = start_(deadline, stop);
      worker->Request({static_cast<std::uint32_t>(Command::Activate), 0, 0, {}}, Remaining(deadline), nullptr,
                      stop, !synchronous);
      const auto reply = worker->Request({static_cast<std::uint32_t>(Command::Create), 0, 0, factory_}, Remaining(deadline),
                                         nullptr, stop, !synchronous);
      RemoteInstance created{std::move(worker), reply.instance};
      lock.lock();
      if (released_ || admission_.stop_requested()) {
        lock.unlock();
        if (created.worker->Pid()) {
          try {
            Retire(created);
          } catch (...) {
            created.worker->Stop();
          }
        }
        throw Closed();
      }
      instance_ = created;
      finish();
      return created;
    } catch (...) {
      if (!lock.owns_lock()) lock.lock();
      finish();
      throw;
    }
  }

  Bytes Invoke(std::uint32_t operation, Bytes request, const ReceiverRegistration* receiver, bool synchronous,
               std::stop_token stop) {
    Check();
    if (request.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "Request exceeds the byte limit");
    {
      std::lock_guard lock(mutex_);
      if (released_) throw Closed();
      ++calls_;
    }
    struct Finish {
      WorkerExtension& extension;
      ~Finish() {
        {
          std::lock_guard lock(extension.mutex_);
          --extension.calls_;
        }
        extension.RetireWhenIdle();
      }
    } finish{*this};
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    const auto instance = EnsureInstance(deadline, synchronous, stop);
    if (released_ || admission_.stop_requested()) throw Closed();
    Frame frame{static_cast<std::uint32_t>(Command::Invoke), instance.id, operation, std::move(request)};
    frame.synchronous = synchronous;
    return instance.worker->Request(std::move(frame), Remaining(deadline), receiver, stop,
                                    !synchronous).payload;
  }

  const std::function<std::shared_ptr<Worker>(Deadline, std::stop_token)> start_;
  const std::chrono::milliseconds timeout_;
  const std::stop_token admission_;
  const Bytes factory_;
  std::atomic_bool released_{false};
  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::optional<RemoteInstance> instance_;
  bool creating_ = false;
  std::size_t calls_ = 0;
};

struct LocalInstance {
  LocalInstance(std::string point, std::uint32_t ordinal) : point(std::move(point)), ordinal(ordinal) {}

  const std::string point;
  const std::uint32_t ordinal;
  void* object = nullptr;
  const hpk_extension_v1* factory = nullptr;
  bool released = false;
  bool borrowed = false;

  void Close() noexcept {
    if (object) factory->close(std::exchange(object, nullptr));
  }
  ~LocalInstance() { Close(); }
};

struct CallExecution {};

struct LocalCall : LocalInvocation {
  explicit LocalCall(CallbackCompletion<void> complete) : complete(std::move(complete)) {}
  std::shared_ptr<LocalInstance> instance;
  ExtensionTarget::LocalWork work;
  CallbackCompletion<void> complete;
  Deadline deadline;
  TaskHandle timer;
  TaskHandle task;
  std::weak_ptr<CallExecution> running;
};

} // namespace

class LocalPlugin : public std::enable_shared_from_this<LocalPlugin> {
public:
  LocalPlugin(TaskScope tasks, std::function<const hpk_plugin_v1&()> load, PluginStartup startup,
              std::vector<RegisteredService> services)
      : tasks(std::move(tasks)), load_(std::move(load)), startup_(std::move(startup)), services_(std::move(services)) {}

  ~LocalPlugin() {
    if (!activation_) return;
    for (const auto& instance : instances) instance->Close();
    binding_->deactivate(activation_);
  }

  void Check(const LocalInstance& instance) const {
    if (closing || instance.released) throw Closed();
  }

  void* Ensure(LocalInstance& instance) {
    Check(instance);
    if (instance.object) return instance.object;
    if (!activation_) {
      const auto& binding = load_();
      activation_ = ActivatePlugin(binding, startup_, HPK_IN_PROCESS);
      binding_ = &binding;
    }
    instance.factory = FindExtension(*binding_, instance.point, instance.ordinal);
    if (!instance.factory) throw Error(ErrorCode::NotFound, "The plugin has no such factory at this extension point");
    const auto status = instance.factory->create(activation_, &instance.object);
    if (status != HPK_OK) {
      instance.Close();
      CheckPluginStatus(status);
    }
    if (!instance.object) throw Error(ErrorCode::Protocol, "The plugin returned an empty instance");
    return instance.object;
  }

  void ValidateFactories() {
    for (const auto& entry : startup_.extensions) {
      LocalInstance instance(entry.point, entry.ordinal);
      (void)Ensure(instance);
    }
  }

  void* Borrow(const std::shared_ptr<LocalInstance>& instance) {
    instance->borrowed = true;
    auto keep_alive = instance;
    auto* object = Ensure(*keep_alive);
    Check(*keep_alive);
    return object;
  }

  void CallSync(LocalInstance& instance, const std::function<void(void*)>& work, Deadline deadline) {
    RejectServiceReentry();
    Check(instance);
    if (active_ || synchronous_) throw Error(ErrorCode::Busy, "The in-process plugin has an active call");
    synchronous_ = true;
    struct Finish {
      LocalPlugin& plugin;
      ~Finish() { plugin.synchronous_ = false; plugin.Sweep(); }
    } finish{*this};
    auto* object = Ensure(instance);
    CheckDeadline(deadline);
    auto local = NewInvocation();
    const Invocation invocation{{}, {}, local.get(), FindService, {}};
    const auto previous = binding_->set_invocation(&invocation);
    struct Restore {
      const hpk_plugin_v1& binding;
      const void* previous;
      ~Restore() { binding.set_invocation(previous); }
    } restore{*binding_, previous};
    try {
      work(object);
    } catch (const Error&) {
      throw;
    } catch (...) {
      throw Error(ErrorCode::PluginFailed, "The plugin implementation failed");
    }
    CheckDeadline(deadline);
  }

  void Schedule(const std::shared_ptr<LocalCall>& call) {
    Check(*call->instance);
    const auto weak = weak_from_this();
    call->timer = tasks.Launch([weak, call]() -> Task<void> {
      co_await Delay((std::max)(Deadline::duration::zero(), call->deadline - std::chrono::steady_clock::now()));
      if (auto plugin = weak.lock()) plugin->Interrupt(call, ErrorCode::DeadlineExceeded);
    });
    try {
      queued_.push_back(call);
      StartNext();
    } catch (...) {
      call->timer.Cancel();
      throw;
    }
  }

  void Interrupt(const std::shared_ptr<LocalCall>& call, ErrorCode code) {
    if (call->interruption) return;
    call->interruption = code;
    call->receiver.reset();
    call->timer.Cancel();
    call->stop.request_stop();
    if (auto cancel = std::exchange(call->cancel_callback, {})) cancel();
    if (code != ErrorCode::Cancelled) {
      call->complete.Fail(std::make_exception_ptr(code == ErrorCode::DeadlineExceeded ? Expired() : Closed()));
    }
    std::erase(queued_, call);
    if (active_ == call) {
      call->task.Cancel();
      try {
        (void)tasks.Launch(Drain(shared_from_this(), call));
      } catch (...) {
        if (call->running.expired() && !HasBackgroundWork(*call)) FinishCall(call, {});
      }
    }
  }

  void Release(const std::shared_ptr<LocalInstance>& instance) {
    instance->released = true;
    for (auto i = queued_.begin(); i != queued_.end();) {
      auto call = *i;
      if (call->instance == instance) {
        Interrupt(call, ErrorCode::Closed);
        i = queued_.begin();
      } else {
        ++i;
      }
    }
    // A running call owns the instance until completion. Release revokes later calls and queued work.
    Sweep();
  }

  void BeginShutdown() {
    if (std::exchange(closing, true)) return;
    while (!queued_.empty()) {
      auto call = queued_.front();
      Interrupt(call, ErrorCode::Closed);
    }
    if (auto active = active_) Interrupt(active, ErrorCode::Closed);
    Sweep();
  }

  Task<void> FinishShutdown() {
    auto start = [self = shared_from_this()](CallbackCompletion<void> complete) {
      if (self->finished_) complete.Complete();
      else self->shutdown_waiters_.push_back(complete);
      return std::function<void()>{};
    };
    co_await AwaitCallback<void>(std::move(start));
  }

  TaskScope tasks;
  bool closing = false;
  std::vector<std::shared_ptr<LocalInstance>> instances;

private:
  template<class Call = LocalInvocation, class... Args>
  std::shared_ptr<Call> NewInvocation(Args&&... args) {
    auto call = std::make_shared<Call>(std::forward<Args>(args)...);
    call->tasks = tasks;
    call->services = shared_from_this();
    call->enter_callback = [](TaskExecution execution) -> void* { return new CallbackScope(std::move(execution)); };
    call->leave_callback = [](void* scope) { delete static_cast<CallbackScope*>(scope); };
    return call;
  }

  static std::shared_ptr<void> FindService(void* context, ContractInfo contract) {
    const auto& call = *static_cast<LocalInvocation*>(context);
    if (call.interruption) throw Closed();
    const auto& plugin = *static_cast<LocalPlugin*>(call.services.get());
    return RequireHostService(plugin.services_, contract).implementation;
  }

  static Task<void> Run(std::shared_ptr<LocalPlugin> plugin, std::shared_ptr<LocalCall> call,
                        std::shared_ptr<CallExecution>) {
    std::exception_ptr error;
    try {
      if (!call->interruption) {
        CheckDeadline(call->deadline);
        auto* object = plugin->Ensure(*call->instance);
        CheckDeadline(call->deadline);
        call->execution = co_await CurrentTaskExecution{};
        const Invocation invocation{call->execution, {}, call.get(), FindService, call->stop.get_token()};
        const auto& binding = *plugin->binding_;
        const std::unique_ptr<void, decltype(binding.detach_invocation)> registration(
            binding.attach_invocation(&invocation), binding.detach_invocation);
        co_await call->work(object, call);
        CheckDeadline(call->deadline);
      }
    } catch (const Error&) {
      error = std::current_exception();
    } catch (...) {
      error = std::make_exception_ptr(Error(ErrorCode::PluginFailed, "The plugin implementation failed"));
    }
    plugin->FinishCall(call, std::move(error));
  }

  static bool HasBackgroundWork(const LocalCall& call) {
    return std::ranges::any_of(call.background, [](const auto& work) { return work.use_count() > 1; });
  }

  static Task<void> Drain(std::shared_ptr<LocalPlugin> plugin, std::shared_ptr<LocalCall> call) {
    // Wait for the root frame to be destroyed and for any blocking work that borrowed the instance.
    while (!call->running.expired() || HasBackgroundWork(*call)) {
      co_await Delay(std::chrono::milliseconds(1));
    }
    if (plugin->active_ == call) plugin->FinishCall(call, {});
  }

  void FinishCall(const std::shared_ptr<LocalCall>& call, std::exception_ptr error) {
    call->timer.Cancel();
    call->receiver.reset();
    call->work = {};
    if (!call->interruption) {
      if (error) call->complete.Fail(std::move(error));
      else call->complete.Complete();
    }
    active_.reset();
    Sweep();
    StartNext();
  }

  void StartNext() {
    while (!closing && !active_ && !queued_.empty()) {
      auto call = queued_.front();
      queued_.pop_front();
      if (std::chrono::steady_clock::now() >= call->deadline) {
        Interrupt(call, ErrorCode::DeadlineExceeded);
        continue;
      }
      active_ = call;
      try {
        auto execution = std::make_shared<CallExecution>();
        call->running = execution;
        call->task = tasks.Launch(Run(shared_from_this(), call, std::move(execution)));
      } catch (...) {
        active_.reset();
        call->timer.Cancel();
        call->complete.Fail(std::current_exception());
      }
    }
  }

  void Sweep() {
    if (synchronous_) return;
    std::erase_if(instances, [&](const auto& instance) {
      if ((active_ && active_->instance == instance) || (!instance->released && (!closing || instance->borrowed))) {
        return false;
      }
      instance->Close();
      return true;
    });
    if (closing && !active_ && instances.empty() && !finished_) {
      if (activation_) binding_->deactivate(std::exchange(activation_, nullptr));
      finished_ = true;
      for (const auto& waiter : shutdown_waiters_) waiter.Complete();
      shutdown_waiters_.clear();
    }
  }

  const hpk_plugin_v1* binding_ = nullptr;
  void* activation_ = nullptr;
  std::function<const hpk_plugin_v1&()> load_;
  PluginStartup startup_;
  std::vector<RegisteredService> services_;
  std::deque<std::shared_ptr<LocalCall>> queued_;
  std::shared_ptr<LocalCall> active_;
  std::vector<CallbackCompletion<void>> shutdown_waiters_;
  bool synchronous_ = false;
  bool finished_ = false;

  friend class LocalExtension;
};

class LocalExtension final : public ExtensionTarget {
public:
  LocalExtension(std::shared_ptr<LocalPlugin> plugin, std::string point, std::uint32_t ordinal,
                 std::chrono::milliseconds timeout)
      : plugin_(std::move(plugin)), instance_(std::make_shared<LocalInstance>(std::move(point), ordinal)), timeout_(timeout) {
    plugin_->Check(*instance_);
    plugin_->instances.push_back(instance_);
  }

  ExecutionMode Mode() const noexcept override { return ExecutionMode::InProcess; }
  void Check() const override { plugin_->Check(*instance_); }
  void Release() noexcept override { plugin_->Release(instance_); }
  void* BorrowLocal() override { return plugin_->Borrow(instance_); }

  void CallLocal(const std::function<void(void*)>& work) override {
    plugin_->CallSync(*instance_, work, std::chrono::steady_clock::now() + timeout_);
  }

  Task<void> CallLocal(LocalWork work, std::shared_ptr<void> receiver) override {
    RejectServiceReentry(co_await CurrentTaskExecution{});
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    auto start = [plugin = plugin_, instance = instance_, work = std::move(work), receiver = std::move(receiver), deadline]
        (CallbackCompletion<void> complete) mutable {
      auto call = plugin->NewInvocation<LocalCall>(std::move(complete));
      call->instance = std::move(instance);
      call->receiver = std::move(receiver);
      call->work = std::move(work);
      call->deadline = deadline;
      plugin->Schedule(call);
      return [plugin, call] { plugin->Interrupt(call, ErrorCode::Cancelled); };
    };
    co_await AwaitCallback<void>(std::move(start));
  }

private:
  std::shared_ptr<LocalPlugin> plugin_;
  std::shared_ptr<LocalInstance> instance_;
  std::chrono::milliseconds timeout_;
};

std::shared_ptr<LocalPlugin> CreateLocalPlugin(TaskScope tasks, std::function<const hpk_plugin_v1&()> load,
                                               PluginStartup startup, std::vector<RegisteredService> services) {
  return std::make_shared<LocalPlugin>(std::move(tasks), std::move(load), std::move(startup), std::move(services));
}

void BeginLocalShutdown(const std::shared_ptr<LocalPlugin>& plugin) { plugin->BeginShutdown(); }

Task<void> FinishLocalShutdown(std::shared_ptr<LocalPlugin> plugin) { co_await plugin->FinishShutdown(); }

void ValidateLocalPlugin(const std::shared_ptr<LocalPlugin>& plugin) {
  plugin->ValidateFactories();
}

std::shared_ptr<ExtensionTarget> MakeLocalExtension(std::shared_ptr<LocalPlugin> plugin,
    std::string point, std::uint32_t ordinal, std::chrono::milliseconds timeout) {
  return std::make_shared<LocalExtension>(std::move(plugin), std::move(point), ordinal, timeout);
}

std::shared_ptr<ExtensionTarget> MakeWorkerExtension(
    std::function<std::shared_ptr<Worker>(Deadline, std::stop_token)> start, std::chrono::milliseconds timeout,
    std::stop_token admission, Bytes factory) {
  return std::make_shared<WorkerExtension>(std::move(start), timeout, admission, std::move(factory));
}

void* ExtensionTarget::BorrowLocal() { throw Error(ErrorCode::UnsupportedMode, "The extension is not in process"); }
void ExtensionTarget::CallLocal(const std::function<void(void*)>&) {
  throw Error(ErrorCode::UnsupportedMode, "The extension is not in process");
}
Task<void> ExtensionTarget::CallLocal(LocalWork, std::shared_ptr<void>) {
  throw Error(ErrorCode::UnsupportedMode, "The extension is not in process");
  co_return;
}
Bytes ExtensionTarget::CallWorker(std::uint32_t, Bytes) {
  throw Error(ErrorCode::UnsupportedMode, "The extension has no worker");
}
Task<Bytes> ExtensionTarget::CallWorker(std::uint32_t operation, Bytes request, std::optional<ReceiverRegistration>) {
  co_return CallWorker(operation, std::move(request));
}

} // namespace huxerui::pluginkit::detail
