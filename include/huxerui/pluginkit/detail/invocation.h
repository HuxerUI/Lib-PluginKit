#pragma once

/// @file
/// @brief Shared dispatch and proxies for host services and call-scoped receivers.

#include <huxerui/task.h>
#include <huxerui/pluginkit/detail/wire.h>

#include <memory>

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

struct HostInvocation {
  const hpk_host_v1* host = nullptr;
  std::uint64_t id = 0;
  TaskExecution execution;
};

inline bool SameExecution(const TaskExecution& left, const TaskExecution& right) noexcept {
  return !left.expired() && !left.owner_before(right) && !right.owner_before(left);
}

inline thread_local const HostInvocation* current_invocation = nullptr;

class InvocationScope {
public:
  explicit InvocationScope(const HostInvocation& call) noexcept
      : previous_(std::exchange(current_invocation, &call)) {}
  ~InvocationScope() { current_invocation = previous_; }

private:
  const HostInvocation* previous_;
};

struct DispatchContext {
  HostInvocation invocation{};
  std::uint32_t receiver = 0;
};

class HostEndpoint {
public:
  explicit HostEndpoint(HostInvocation invocation, std::uint32_t receiver = 0)
      : invocation_(invocation), receiver_(receiver) {}

  void Start(ContractInfo service, std::uint32_t operation, std::span<const std::byte> request,
             const TaskExecution& execution, CallbackCompletion<Bytes> complete) const {
    if (!invocation_.host || !current_invocation || current_invocation->id != invocation_.id ||
        current_invocation->host != invocation_.host || !SameExecution(execution, invocation_.execution)) {
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
  static void HPK_CALL Complete(void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
    const std::unique_ptr<CallbackCompletion<Bytes>> completion(static_cast<CallbackCompletion<Bytes>*>(user));
    try {
      if (buffer.size > MaxPayload || (buffer.size && !buffer.data)) {
        throw Error(ErrorCode::Protocol, "The host returned an invalid buffer");
      }
      if (status) {
        const auto message = buffer.size ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.size)
                                         : std::string("Host callback failed");
        throw Error(static_cast<ErrorCode>(status), message);
      }
      Bytes bytes;
      if (buffer.size) {
        const auto view = std::as_bytes(std::span(buffer.data, buffer.size));
        bytes.assign(view.begin(), view.end());
      }
      completion->Complete(std::move(bytes));
    } catch (...) {
      completion->Fail(std::current_exception());
    }
  }

  HostInvocation invocation_;
  std::uint32_t receiver_;
};

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
