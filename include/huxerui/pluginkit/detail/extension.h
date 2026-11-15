#pragma once

/// @file
/// @brief Execution modes and shared implementation of typed extension and service handles.

#include <huxerui/pluginkit/detail/streams.h>

#include <functional>
#include <tuple>
#include <type_traits>

namespace huxerui::pluginkit {

/// @brief Execution location selected by the host when approving a plugin package.
enum class ExecutionMode {
  /// Loads the plugin library into the host and invokes its C++ implementation directly.
  /// Requires the host's C++ SDK and active HuxerUI application. Faults can affect the host process.
  InProcess,
  /// Loads the plugin library into a separate HuxerUI worker process and invokes its generated bindings over IPC.
  /// The worker runs under the host's user account.
  WorkerProcess
};

namespace detail {

using ServiceDispatch = std::function<Task<Bytes>(std::uint32_t, Bytes)>;

template<class Interface>
ServiceDispatch BindCallback(std::shared_ptr<Interface> implementation) {
  if (!implementation) return {};
  if constexpr (requires(Bytes bytes) { Contract<Interface>::Dispatch(*implementation, 0u, std::move(bytes), {}); }) {
    return [implementation = std::move(implementation)](std::uint32_t operation, Bytes request) {
      return Contract<Interface>::Dispatch(*implementation, operation, std::move(request), {});
    };
  } else {
    return {};
  }
}

template<class Interface>
void GrantReceiver(CallGrants& grants, std::uint32_t slot, std::shared_ptr<Interface> implementation) {
  grants.Add(slot, ReceiverRegistration{Contract<Interface>::Info, std::move(implementation),
      [](void* object, std::uint32_t operation, Bytes request) {
        return Contract<Interface>::Dispatch(*static_cast<Interface*>(object), operation, std::move(request), {});
      }});
}

// Callback proxies hold weak references. The active call owns its granted endpoints.
struct LocalInvocation : std::enable_shared_from_this<LocalInvocation> {
  TaskExecution execution;
  TaskScope tasks;
  std::stop_source stop;
  std::shared_ptr<void> services;
  std::shared_ptr<CallGrants> grants;
  std::optional<ErrorCode> interruption;
  std::function<void()> cancel_callback;
  std::vector<std::shared_ptr<void>> background;
  void* (*enter_callback)(TaskExecution) = nullptr;
  void (*leave_callback)(void*) = nullptr;
};

class ExtensionTarget {
public:
  using LocalWork = std::function<Task<void>(void*, std::shared_ptr<LocalInvocation>)>;
  virtual ~ExtensionTarget() = default;
  virtual ExecutionMode Mode() const noexcept = 0;
  virtual void Check() const = 0;
  virtual void Release() noexcept = 0;
  virtual void* BorrowLocal();
  virtual void CallLocal(const std::function<void(void*)>& work, std::shared_ptr<CallGrants> grants);
  virtual Task<void> CallLocal(LocalWork work, std::shared_ptr<CallGrants> grants);
  virtual Bytes CallWorkerSync(std::uint32_t operation, Bytes request, std::shared_ptr<CallGrants> grants);
  virtual Task<Bytes> CallWorker(std::uint32_t operation, Bytes request, std::shared_ptr<CallGrants> grants);
};

class CallbackScope {
public:
  explicit CallbackScope(TaskExecution execution);
  ~CallbackScope();
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

private:
  TaskExecution execution_;
};

template<class Result, class Invoke>
Task<void> CompleteLocalCallback(std::shared_ptr<LocalInvocation> invocation, Invoke invoke,
                                 CallbackCompletion<Result> complete) {
  std::optional work(std::move(invoke));
  try {
    auto scope = std::unique_ptr<void, void(*)(void*)>(
        invocation->enter_callback(co_await CurrentTaskExecution{}), invocation->leave_callback);
    // Completion can resume the plugin immediately. Release the endpoint and reentry guard first.
    const auto release = [&] {
      work.reset();
      scope.reset();
    };
    if constexpr (std::is_void_v<Result>) {
      co_await (*work)();
      release();
      complete.Complete();
    } else {
      auto result = co_await (*work)();
      release();
      complete.Complete(std::move(result));
    }
  } catch (...) {
    work.reset();
    complete.Fail(std::current_exception());
  }
}

template<class Result, class Invoke>
Task<Result> InvokeLocalCallback(std::shared_ptr<LocalInvocation> invocation, Invoke invoke) {
  if (invocation->cancel_callback) throw Error(ErrorCode::Busy, "The invocation already has an active callback");
  struct ClearCallback {
    std::shared_ptr<LocalInvocation> invocation;
    ~ClearCallback() { invocation->cancel_callback = {}; }
  } clear{invocation};
  auto start = [invocation, invoke = std::move(invoke)](auto complete) mutable {
    const auto task = invocation->tasks.Launch(CompleteLocalCallback<Result>(invocation, std::move(invoke), complete));
    invocation->cancel_callback = [task, complete] {
      task.Cancel();
      complete.Fail(std::make_exception_ptr(Error(ErrorCode::Closed, "The plugin call is closing")));
    };
    return std::function<void()>([task] { task.Cancel(); });
  };
  co_return co_await AwaitCallback<Result>(std::move(start));
}

inline std::shared_ptr<LocalInvocation> RequireLocalInvocation(const std::weak_ptr<LocalInvocation>& weak,
                                                               const TaskExecution& execution) {
  auto call = weak.lock();
  if (!call || call->interruption || !call->grants || !SameExecution(execution, call->execution)) {
    throw Error(ErrorCode::ReceiverClosed, "The receiver is outside its owning invocation");
  }
  return call;
}

template<class Work, class... Args>
auto RunPluginWorker(Work work, Args... args)
    -> Task<std::invoke_result_t<Work&, std::stop_token, Args...>> {
  using Result = std::invoke_result_t<Work&, std::stop_token, Args...>;
  const auto& invocation = FindInvocation(co_await CurrentTaskExecution{});
  if (!invocation.find_service) {
    co_return co_await RunWorker(std::move(work), invocation.stop, std::move(args)...);
  }
  auto& call = *static_cast<LocalInvocation*>(invocation.services);
  if (call.interruption) throw Error(ErrorCode::Closed, "The plugin call is closing");
  // HuxerUI may cancel the awaiting coroutine before a running native job exits. Its captures own the lease.
  auto lease = std::make_shared<int>();
  std::erase_if(call.background, [](const auto& active) { return active.use_count() == 1; });
  call.background.push_back(lease);
  struct OwnedWork {
    std::shared_ptr<int> lease;
    Work work;
    std::tuple<Args...> arguments;

    Result operator()(std::stop_token stop) {
      return std::apply([&](auto&&... values) -> Result {
        return std::invoke(work, stop, std::forward<decltype(values)>(values)...);
      }, std::move(arguments));
    }
  };
  auto owned = OwnedWork{std::move(lease), std::move(work), {std::move(args)...}};
  co_return co_await RunWorker(std::move(owned), call.stop.get_token());
}

template<class Interface>
std::shared_ptr<Interface> LocalReceiver(const std::shared_ptr<LocalInvocation>& invocation, std::uint32_t slot) {
  if (!invocation->grants->Has(slot)) return {};
  return std::make_shared<typename Contract<Interface>::Callback>(invocation, slot);
}

template<class Result, class Interface, class Method, class... Args>
Task<Result> InvokeReceiver(std::weak_ptr<LocalInvocation> local, std::uint32_t slot, std::optional<HostEndpoint> worker,
                            std::uint32_t operation, Method method, Args... args) {
  if (worker) {
    co_return co_await InvokeHost<Result, Interface>(*worker, Contract<Interface>::Info, operation, EncodeArguments<Interface>(args...));
  }
  auto call = RequireLocalInvocation(local, co_await CurrentTaskExecution{});
  auto access = call->grants->AcquireReceiver(slot, Contract<Interface>::Info);
  co_return co_await InvokeLocalCallback<Result>(std::move(call),
      [access = std::move(access), method, ...args = std::move(args)]() mutable {
        auto& receiver = *static_cast<Interface*>(access.template Get<ReceiverRegistration>().implementation.get());
        return std::invoke(method, receiver, std::move(args)...);
      });
}

template<class Result, class Interface, class Method, class... Args>
Task<Result> InvokeService(std::uint32_t operation, Method method, Args... args) {
  const auto& invocation = FindInvocation(co_await CurrentTaskExecution{});
  auto service = ResolveService<Interface>(invocation);
  if (service.worker) {
    co_return co_await InvokeHost<Result, Interface>(*service.worker, Contract<Interface>::Info, operation, EncodeArguments<Interface>(args...));
  }
  auto call = static_cast<LocalInvocation*>(invocation.services)->shared_from_this();
  auto access = call->grants ? call->grants->AcquireService() : CallGrants::Access{};
  co_return co_await InvokeLocalCallback<Result>(std::move(call),
      [access = std::move(access), target = std::move(service.local), method, ...args = std::move(args)]() mutable {
        return std::invoke(method, *target, std::move(args)...);
      });
}

template<class Result, class Interface, class Invoke>
Result InvokeServiceSync(Invoke invoke) {
  const auto& invocation = FindSynchronousInvocation();
  auto service = ResolveService<Interface>(invocation);
  const auto& call = *static_cast<LocalInvocation*>(invocation.services);
  auto access = call.grants ? call.grants->AcquireService() : CallGrants::Access{};
  const std::unique_ptr<void, void(*)(void*)> scope(call.enter_callback({}), call.leave_callback);
  return invoke(*service.local);
}

template<class Result, class Interface, class Encode>
Task<Result> InvokeWorker(std::shared_ptr<ExtensionTarget> target, std::uint32_t operation, Encode encode,
                          std::shared_ptr<CallGrants> grants) {
  target->Check();
  co_return DecodeResult<Result, Interface>(co_await target->CallWorker(operation, encode(), std::move(grants)));
}

template<class Result, class Invoke>
Task<Result> InvokeLocal(std::shared_ptr<ExtensionTarget> target, Invoke invoke, std::shared_ptr<CallGrants> grants) {
  // The native invocation can own move-only arguments. The scheduled work is a copyable handle to that state.
  auto work = std::make_shared<Invoke>(std::move(invoke));
  if constexpr (std::is_void_v<Result>) {
    co_await target->CallLocal([work](void* object, auto invocation) {
      return (*work)(object, std::move(invocation));
    }, std::move(grants));
  } else {
    auto result = std::make_shared<std::optional<Result>>();
    co_await target->CallLocal([work, result](void* object, auto invocation) {
      return (*work)(object, std::move(invocation), result);
    }, std::move(grants));
    co_return std::move(**result);
  }
}

template<class Result>
Task<void> StoreLocalResult(Task<Result> call, std::shared_ptr<std::optional<Result>> result) {
  result->emplace(co_await std::move(call));
}

template<class Result, class Invoke>
Result InvokeLocalSync(ExtensionTarget& target, Invoke invoke, std::shared_ptr<CallGrants> grants = {}) {
  if constexpr (std::is_void_v<Result>) {
    target.CallLocal(invoke, std::move(grants));
  } else {
    std::optional<Result> result;
    target.CallLocal([&](void* object) { result.emplace(invoke(object)); }, std::move(grants));
    return std::move(*result);
  }
}

} // namespace detail

/// @brief Typed access to a host service published only for in-process callers.
///
/// Resolve the service from a synchronous managed invocation. HPK_REMOTE contracts provide a generated
/// specialization whose Task methods also resolve services from an asynchronous invocation.
/// @tparam Interface Service interface with a Contract specialization.
/// @see GetService(), PluginManager::ProvideService()
template<class Interface>
class ServiceHandle {
public:
  /// @brief Resolves the granted service and exposes its native C++ interface.
  /// @return Shared pointer that keeps the implementation alive for the member access. Use it within the invocation.
  /// @throws Error With ServiceUnavailable outside a managed invocation, CallbackUnavailable in a managed Task,
  /// ServiceDenied without a grant, ContractMismatch for a different service contract, or UnsupportedMode if
  /// the endpoint belongs to a worker.
  std::shared_ptr<Interface> operator->() const {
    auto service = detail::ResolveService<Interface>(detail::FindSynchronousInvocation());
    if (!service.local) throw Error(ErrorCode::UnsupportedMode, "The service requires an in-process caller");
    return std::move(service.local);
  }
};

} // namespace huxerui::pluginkit
