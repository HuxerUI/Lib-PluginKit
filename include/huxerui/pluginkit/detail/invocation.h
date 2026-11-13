#pragma once

/// @file
/// @brief Shared dispatch and proxies for host services and call-scoped receivers.

#include <huxerui/task.h>
#include <huxerui/pluginkit/detail/wire.h>

#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace huxerui::pluginkit {

namespace detail {

// Child HuxerUI tasks inherit their root execution identity across suspensions.
struct CurrentTaskExecution {
  std::weak_ptr<huxerui::detail::TaskExecution> execution;
  bool await_ready() const noexcept { return false; }

  template<class Promise>
  bool await_suspend(std::coroutine_handle<Promise> coroutine) noexcept {
    execution = coroutine.promise().Execution();
    return false;
  }

  auto await_resume() const noexcept { return execution; }
};

using TaskExecution = std::weak_ptr<huxerui::detail::TaskExecution>;

/// @brief Worker callback table and identity of the invocation allowed to use it.
struct HostInvocation {
  const hpk_host_v1* host = nullptr;
  std::uint64_t id = 0;
  TaskExecution execution;
};

inline bool SameExecution(const TaskExecution& left, const TaskExecution& right) noexcept {
  return !left.expired() && !left.owner_before(right) && !right.owner_before(left);
}

inline bool HasExecutionOwner(const TaskExecution& execution) noexcept {
  const TaskExecution empty;
  return execution.owner_before(empty) || empty.owner_before(execution);
}

/// @brief Worker-side endpoint for an authorized service or an invocation-scoped receiver.
class HostEndpoint {
public:
  /// @brief Binds to the worker invocation and the selected callback target.
  /// @param invocation Worker invocation that owns the endpoint.
  /// @param receiver Slot passed to plugin dispatch, or zero to look up a service by contract.
  explicit HostEndpoint(HostInvocation invocation, std::uint32_t receiver = 0)
      : invocation_(invocation), receiver_(receiver) {}

  void Start(ContractInfo service, std::uint32_t operation, std::span<const std::byte> request,
             const TaskExecution& execution, CallbackCompletion<Bytes> complete) const {
    if (!invocation_.host || !SameExecution(execution, invocation_.execution)) {
      const auto code = receiver_ ? ErrorCode::ReceiverClosed : ErrorCode::ServiceUnavailable;
      throw Error(code, "The proxy is outside its owning invocation");
    }
    if (request.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "Host callback request exceeds the byte limit");
    auto* completion = new CallbackCompletion<Bytes>(std::move(complete));
    const auto status = invocation_.host->call(invocation_.id, receiver_, service.id.data(), service.version,
                                           service.fingerprint.data(), operation,
                                           reinterpret_cast<const std::uint8_t*>(request.data()),
                                           static_cast<std::uint32_t>(request.size()), Complete, completion);
    if (status) {
      delete completion;
      throw Error(static_cast<ErrorCode>(status), "The worker rejected the host callback");
    }
  }

private:
  static Bytes ReadReply(std::uint32_t status, hpk_buffer_v1 buffer) {
    if (buffer.size > MaxPayload || (buffer.size && !buffer.data)) {
      throw Error(ErrorCode::Protocol, "The host returned an invalid buffer");
    }
    if (status) {
      const auto message = buffer.size ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.size)
                                       : std::string("Host callback failed");
      throw Error(static_cast<ErrorCode>(status), message);
    }
    if (!buffer.size) return {};
    const auto view = std::as_bytes(std::span(buffer.data, buffer.size));
    return Bytes(view.begin(), view.end());
  }

  static void HPK_CALL Complete(void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
    const std::unique_ptr<CallbackCompletion<Bytes>> completion(static_cast<CallbackCompletion<Bytes>*>(user));
    try {
      completion->Complete(ReadReply(status, buffer));
    } catch (...) {
      completion->Fail(std::current_exception());
    }
  }

  HostInvocation invocation_;
  std::uint32_t receiver_;
};

/// @brief Worker invocation information needed by generated dispatch to bind a receiver.
/// Service and receiver dispatch on the host use an empty context.
struct DispatchContext {
  /// Worker callback table and identity, borrowed for the duration of the invocation.
  HostInvocation invocation{};
  /// Invocation's receiver slot, or zero if the caller supplied no receiver.
  std::uint32_t receiver = 0;
};

// The plugin owns its thread-local storage even when it is loaded into the host process.
struct Invocation {
  TaskExecution execution;
  std::optional<HostEndpoint> worker;
  void* services = nullptr;
  std::shared_ptr<void> (*find_service)(void*, ContractInfo) = nullptr;
  std::stop_token stop;
};

inline thread_local const Invocation* synchronous_invocation = nullptr;
inline thread_local std::vector<const Invocation*> task_invocations;

// Registrations can overlap while Tasks suspend. Resolve by execution identity and remove the exact entry.
class InvocationScope {
public:
  explicit InvocationScope(const Invocation& call) : call_(&call) { task_invocations.push_back(call_); }
  ~InvocationScope() { std::erase(task_invocations, call_); }
  InvocationScope(const InvocationScope&) = delete;
  InvocationScope& operator=(const InvocationScope&) = delete;

private:
  const Invocation* call_;
};

// Synchronous entry points bind the context only for the duration of their C++ stack call.
inline const Invocation& FindSynchronousInvocation() {
  if (synchronous_invocation) return *synchronous_invocation;
  if (task_invocations.empty()) throw Error(ErrorCode::ServiceUnavailable, "Host services require an active plugin invocation");
  throw Error(ErrorCode::CallbackUnavailable, "Use a Task host service method from a plugin coroutine");
}

inline const Invocation& FindInvocation(const TaskExecution& execution) {
  for (auto i = task_invocations.rbegin(); i != task_invocations.rend(); ++i) {
    if (SameExecution(execution, (*i)->execution)) return **i;
  }
  throw Error(ErrorCode::ServiceUnavailable, "Host services require an active plugin invocation");
}

template<class Interface>
struct ServiceTarget {
  std::shared_ptr<Interface> local;
  std::optional<HostEndpoint> worker;
};

template<class Interface>
ServiceTarget<Interface> ResolveService(const Invocation& invocation) {
  if (invocation.find_service) {
    return {std::static_pointer_cast<Interface>(invocation.find_service(invocation.services, Contract<Interface>::Info)), {}};
  }
  if (invocation.worker) return {{}, invocation.worker};
  throw Error(ErrorCode::ServiceUnavailable, "The invocation has no host service endpoint");
}

template<class Result>
Result DecodeResult(std::span<const std::byte> payload) {
  if constexpr (std::is_void_v<Result>) {
    if (!payload.empty()) throw Error(ErrorCode::Protocol, "A void operation returned a payload");
  } else {
    return Decode<Result>(payload);
  }
}

template<class Result>
Task<Result> InvokeHost(HostEndpoint endpoint, ContractInfo contract, std::uint32_t operation, Bytes request) {
  auto execution = co_await CurrentTaskExecution{};
  auto response = co_await AwaitCallback<Bytes>([&](CallbackCompletion<Bytes> complete) {
    endpoint.Start(contract, operation, request, execution, std::move(complete));
    return std::function<void()>{};
  });
  co_return DecodeResult<Result>(response);
}

} // namespace detail
} // namespace huxerui::pluginkit
