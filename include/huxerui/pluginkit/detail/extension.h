#pragma once

/// @file
/// @brief Execution modes and shared implementation of typed extension and service handles.

#include <huxerui/pluginkit/detail/invocation.h>

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

struct ReceiverRegistration {
  ContractInfo contract;
  ServiceDispatch dispatch;
};

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
std::optional<ReceiverRegistration> BindReceiver(std::shared_ptr<Interface> implementation) {
  if (!implementation) return std::nullopt;
  return ReceiverRegistration{Contract<Interface>::Info, BindCallback(std::move(implementation))};
}

// All local callback proxies weakly reference this call. Revocation drops the original receiver immediately.
struct LocalInvocation : std::enable_shared_from_this<LocalInvocation> {
  TaskExecution execution;
  TaskScope tasks;
  std::stop_source stop;
  std::shared_ptr<void> receiver;
  std::shared_ptr<void> services;
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
  virtual void CallLocal(const std::function<void(void*)>& work);
  virtual Task<void> CallLocal(LocalWork work, std::shared_ptr<void> receiver);
  virtual Bytes CallWorker(std::uint32_t operation, Bytes request);
  virtual Task<Bytes> CallWorker(std::uint32_t operation, Bytes request, std::optional<ReceiverRegistration> receiver);
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
  try {
    const std::unique_ptr<void, void(*)(void*)> scope(
        invocation->enter_callback(co_await CurrentTaskExecution{}), invocation->leave_callback);
    if constexpr (std::is_void_v<Result>) {
      co_await invoke();
      complete.Complete();
    } else {
      complete.Complete(co_await invoke());
    }
  } catch (...) {
    complete.Fail(std::current_exception());
  }
}

template<class Result, class Invoke>
Task<Result> InvokeLocalCallback(std::shared_ptr<LocalInvocation> invocation, Invoke invoke) {
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
  if (!call || call->interruption || !call->receiver || !SameExecution(execution, call->execution)) {
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

template<class Result, class Interface, class Invoke, class Encode>
Task<Result> InvokeReceiver(std::weak_ptr<LocalInvocation> local, std::optional<HostEndpoint> worker,
                            std::uint32_t operation, auto request, Invoke invoke, Encode encode) {
  if (worker) co_return co_await InvokeHost<Result>(*worker, Contract<Interface>::Info, operation, encode(request));
  auto call = RequireLocalInvocation(local, co_await CurrentTaskExecution{});
  auto receiver = std::static_pointer_cast<Interface>(call->receiver);
  co_return co_await InvokeLocalCallback<Result>(std::move(call),
      [receiver = std::move(receiver), request = std::move(request), invoke = std::move(invoke)]() mutable {
        return invoke(*receiver, std::move(request));
      });
}

template<class Result, class Interface, class Invoke, class Encode>
Task<Result> InvokeService(std::uint32_t operation, auto request, Invoke invoke, Encode encode) {
  const auto& invocation = FindInvocation(co_await CurrentTaskExecution{});
  auto service = ResolveService<Interface>(invocation);
  if (service.worker) co_return co_await InvokeHost<Result>(*service.worker, Contract<Interface>::Info, operation, encode(request));
  co_return co_await InvokeLocalCallback<Result>(static_cast<LocalInvocation*>(invocation.services)->shared_from_this(),
      [service = std::move(service.local), request = std::move(request), invoke = std::move(invoke)]() mutable {
        return invoke(*service, std::move(request));
      });
}

template<class Result, class Interface, class Invoke>
Result InvokeServiceSync(Invoke invoke) {
  const auto& invocation = FindSynchronousInvocation();
  auto service = ResolveService<Interface>(invocation);
  const auto& call = *static_cast<LocalInvocation*>(invocation.services);
  const std::unique_ptr<void, void(*)(void*)> scope(call.enter_callback({}), call.leave_callback);
  return invoke(*service.local);
}

template<class Result, class Encode>
Task<Result> InvokeWorker(std::shared_ptr<ExtensionTarget> target, std::uint32_t operation, Encode encode,
                          std::optional<ReceiverRegistration> receiver = {}) {
  target->Check();
  co_return DecodeResult<Result>(co_await target->CallWorker(operation, encode(), std::move(receiver)));
}

template<class Result, class Invoke>
Task<Result> InvokeLocal(std::shared_ptr<ExtensionTarget> target, Invoke invoke, std::shared_ptr<void> receiver = {}) {
  if constexpr (std::is_void_v<Result>) {
    co_await target->CallLocal(std::move(invoke), std::move(receiver));
  } else {
    auto result = std::make_shared<std::optional<Result>>();
    co_await target->CallLocal([invoke = std::move(invoke), result](void* object, auto invocation) mutable {
      return invoke(object, std::move(invocation), result);
    }, std::move(receiver));
    co_return std::move(**result);
  }
}

template<class Result>
Task<void> StoreLocalResult(Task<Result> call, std::shared_ptr<std::optional<Result>> result) {
  result->emplace(co_await std::move(call));
}

template<class Result, class Invoke>
Result InvokeLocalSync(ExtensionTarget& target, Invoke invoke) {
  if constexpr (std::is_void_v<Result>) {
    target.CallLocal(invoke);
  } else {
    std::optional<Result> result;
    target.CallLocal([&](void* object) { result.emplace(invoke(object)); });
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
