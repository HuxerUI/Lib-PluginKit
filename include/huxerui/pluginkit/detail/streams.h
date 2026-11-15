#pragma once

/// @file
/// @brief Invocation grants and SDK stream adapters used by generated bindings.

#include <huxerui/pluginkit/detail/invocation.h>
#include <huxerui/stream.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <variant>

namespace huxerui::pluginkit::detail {

inline constexpr std::size_t MaxStreamChunk = HPK_MAX_STREAM_CHUNK;

struct ReceiverRegistration {
  ContractInfo contract;
  std::shared_ptr<void> implementation;
  Task<Bytes> (*dispatch)(void*, std::uint32_t, Bytes);
};

class CallGrants : public std::enable_shared_from_this<CallGrants> {
  using Endpoint = std::variant<InputStream, OutputStream, AsyncInputStream, AsyncOutputStream, ReceiverRegistration>;
  struct Slot {
    Endpoint endpoint;
    bool output_closed = false;
    bool output_failed = false;
  };

public:
  // Revocation drops the table's references. An admitted operation keeps only its own endpoint alive.
  class Access {
  public:
    Access() = default;
    Access(Access&&) noexcept = default;
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
    ~Access() {
      if (owner_) {
        std::lock_guard lock(owner_->mutex_);
        owner_->active_ = false;
      }
    }

    template<class T>
    T& Get() const { return std::get<T>(slot_->endpoint); }

  private:
    friend class CallGrants;
    Access(std::shared_ptr<CallGrants> owner, std::shared_ptr<Slot> slot)
        : owner_(std::move(owner)), slot_(std::move(slot)) {}
    std::shared_ptr<CallGrants> owner_;
    std::shared_ptr<Slot> slot_;
  };

  explicit CallGrants(std::size_t count) : slots_(count) {}

  // Bind arguments before publishing the invocation. Slots are the one-based parameter positions.
  template<class T>
  void Add(std::uint32_t slot, T endpoint) {
    slots_.at(slot - 1) = std::make_shared<Slot>(Slot{Endpoint(std::move(endpoint))});
  }

  bool Has(std::uint32_t slot) {
    std::lock_guard lock(mutex_);
    return Find(slot) != nullptr;
  }

  void SetDeadline(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard lock(mutex_);
    deadline_ = deadline;
  }

  void Revoke() noexcept {
    std::vector<std::shared_ptr<Slot>> released;
    {
      std::lock_guard lock(mutex_);
      revoked_ = true;
      released.swap(slots_);
    }
  }

  void Complete() {
    std::vector<std::shared_ptr<Slot>> released;
    {
      std::lock_guard lock(mutex_);
      if (revoked_) throw Error(ErrorCode::Closed, "The invocation grants have been revoked");
      if (active_) throw Error(ErrorCode::Busy, "An invocation callback is still active");
      for (const auto& slot : slots_) {
        if (slot && (std::holds_alternative<OutputStream>(slot->endpoint) ||
                     std::holds_alternative<AsyncOutputStream>(slot->endpoint)) && !slot->output_closed) {
          throw Error(ErrorCode::Io, "The plugin did not successfully close every output stream");
        }
      }
      revoked_ = true;
      released.swap(slots_);
    }
  }

  Access AcquireService() {
    std::lock_guard lock(mutex_);
    if (revoked_) throw Error(ErrorCode::Closed, "The invocation grants have been revoked");
    return Begin({});
  }

  Access AcquireReceiver(std::uint32_t slot, ContractInfo contract) {
    auto access = Acquire<ReceiverRegistration>(slot, ErrorCode::ReceiverClosed);
    if (access.Get<ReceiverRegistration>().contract != contract) {
      throw Error(ErrorCode::ContractMismatch, "Receiver contract does not match");
    }
    return access;
  }

  IoResult<std::size_t> Read(std::uint32_t slot, std::span<std::byte> buffer) {
    auto access = Acquire<InputStream>(slot);
    return access.Get<InputStream>().Read(buffer);
  }

  Task<IoResult<Bytes>> ReadAsync(std::uint32_t slot, std::size_t count) {
    auto access = Acquire<AsyncInputStream>(slot);
    co_return co_await access.Get<AsyncInputStream>().ReadAsync(count);
  }

  IoResult<void> Write(std::uint32_t slot, std::span<const std::byte> bytes) {
    auto access = Acquire<OutputStream>(slot);
    auto result = access.Get<OutputStream>().Write(bytes);
    RecordOutput(access, result, false);
    return result;
  }

  Task<IoResult<void>> WriteAsync(std::uint32_t slot, Bytes bytes) {
    auto access = Acquire<AsyncOutputStream>(slot);
    auto result = co_await access.Get<AsyncOutputStream>().WriteAsync(std::move(bytes));
    RecordOutput(access, result, false);
    co_return result;
  }

  IoResult<void> Close(std::uint32_t slot) {
    auto access = Acquire<OutputStream>(slot);
    auto result = access.Get<OutputStream>().Close();
    RecordOutput(access, result, true);
    return result;
  }

  Task<IoResult<void>> CloseAsync(std::uint32_t slot) {
    auto access = Acquire<AsyncOutputStream>(slot);
    auto result = co_await access.Get<AsyncOutputStream>().CloseAsync();
    RecordOutput(access, result, true);
    co_return result;
  }

private:
  std::shared_ptr<Slot> Find(std::uint32_t slot) const {
    return slot && slot <= slots_.size() ? slots_[slot - 1] : nullptr;
  }

  Access Begin(std::shared_ptr<Slot> entry) {
    if (std::chrono::steady_clock::now() >= deadline_) {
      throw Error(ErrorCode::DeadlineExceeded, "The callback exceeded the invocation deadline");
    }
    if (active_) throw Error(ErrorCode::Busy, "The invocation already has an active callback");
    auto owner = shared_from_this();
    active_ = true;
    return {std::move(owner), std::move(entry)};
  }

  template<class T>
  Access Acquire(std::uint32_t slot, ErrorCode closed = ErrorCode::Closed) {
    std::lock_guard lock(mutex_);
    auto entry = Find(slot);
    if (!entry) throw Error(closed, "The endpoint is outside its owning invocation");
    if (!std::holds_alternative<T>(entry->endpoint)) {
      throw Error(ErrorCode::Protocol, "The invocation endpoint has a different type");
    }
    if constexpr (std::is_same_v<T, OutputStream> || std::is_same_v<T, AsyncOutputStream>) {
      if (entry->output_failed || entry->output_closed) {
        throw Error(ErrorCode::Closed, "The invocation output stream is closed");
      }
      auto access = Begin(entry);
      // A failed or interrupted write or close must prevent a successful invocation.
      entry->output_failed = true;
      return access;
    } else {
      return Begin(std::move(entry));
    }
  }

  void RecordOutput(const Access& access, const IoResult<void>& result, bool close) {
    std::lock_guard lock(mutex_);
    access.slot_->output_failed = !result.Succeeded();
    access.slot_->output_closed = close && result.Succeeded();
  }

  std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::time_point::max();
  std::mutex mutex_;
  std::vector<std::shared_ptr<Slot>> slots_;
  bool active_ = false;
  bool revoked_ = false;
};

struct RevokeGrants {
  std::shared_ptr<CallGrants> grants;
  ~RevokeGrants() {
    if (grants) grants->Revoke();
  }
};

inline thread_local const void* synchronous_stream_owner = nullptr;

// The SDK synchronous callback has no Task identity. The active native stack supplies its owner.
class SynchronousStreamScope {
public:
  explicit SynchronousStreamScope(const void* owner) : previous_(std::exchange(synchronous_stream_owner, owner)) {}
  ~SynchronousStreamScope() { synchronous_stream_owner = previous_; }
  SynchronousStreamScope(const SynchronousStreamScope&) = delete;
  SynchronousStreamScope& operator=(const SynchronousStreamScope&) = delete;

private:
  const void* previous_;
};

inline void CheckSynchronousStream(const void* owner) {
  if (!owner || synchronous_stream_owner != owner) {
    throw Error(ErrorCode::Closed, "The stream is outside its owning invocation");
  }
}

inline void CheckAsyncStream(const TaskExecution& owner, const TaskExecution& current, std::stop_token stop) {
  if (!SameExecution(owner, current) || stop.stop_requested()) {
    throw Error(ErrorCode::Closed, "The stream is outside its owning invocation");
  }
}

InputStream LocalInput(const std::shared_ptr<CallGrants>& grants, std::uint32_t slot);
OutputStream LocalOutput(const std::shared_ptr<CallGrants>& grants, std::uint32_t slot);
AsyncInputStream LocalInput(const std::shared_ptr<CallGrants>& grants, std::uint32_t slot, TaskExecution execution);
AsyncOutputStream LocalOutput(const std::shared_ptr<CallGrants>& grants, std::uint32_t slot, TaskExecution execution);

// The callback's diagnostic storage belongs to the transport. Copy a bounded reply before returning.
inline IoResult<Bytes> ReceiveStreamReply(std::uint32_t status, hpk_buffer_v1 buffer) {
  if ((buffer.size && !buffer.data) || buffer.size > MaxStreamChunk) {
    throw Error(ErrorCode::Protocol, "Invalid stream response buffer");
  }
  if (status) {
    if (status > static_cast<std::uint32_t>(IoErrorCode::NoSpace) + 1) {
      throw Error(ErrorCode::Protocol, "Invalid stream I/O error");
    }
    auto message = buffer.size ? std::string(reinterpret_cast<const char*>(buffer.data), buffer.size) : std::string{};
    return IoResult<Bytes>::Failure({static_cast<IoErrorCode>(status - 1), std::move(message)});
  }
  if (!buffer.size) return IoResult<Bytes>(Bytes{});
  const auto bytes = std::as_bytes(std::span(buffer.data, buffer.size));
  return IoResult<Bytes>(Bytes(bytes.begin(), bytes.end()));
}

class WorkerStreams {
public:
  explicit WorkerStreams(HostInvocation invocation) : invocation_(std::make_shared<HostInvocation>(invocation)) {}

  const void* Owner() const noexcept { return invocation_.get(); }

  InputStream Input(std::uint32_t slot) const {
    return InputStream::FromReader([self = *this, slot](std::span<std::byte> buffer) -> IoResult<std::size_t> {
      const auto count = (std::min)(buffer.size(), MaxStreamChunk);
      auto data = self.Sync(slot, HPK_READ_STREAM, count);
      if (!data.Succeeded()) return IoResult<std::size_t>(std::move(data).Error());
      const auto& bytes = data.Value();
      if (bytes.size() > count) throw Error(ErrorCode::Protocol, "The host returned more than the requested bytes");
      std::copy(bytes.begin(), bytes.end(), buffer.begin());
      return IoResult<std::size_t>(bytes.size());
    });
  }

  OutputStream Output(std::uint32_t slot) const {
    return OutputStream::FromWriter(
        [self = *this, slot](std::span<const std::byte> bytes) -> IoResult<void> {
          while (!bytes.empty()) {
            const auto chunk = bytes.first((std::min)(bytes.size(), MaxStreamChunk));
            auto result = Acknowledge(self.Sync(slot, HPK_WRITE_STREAM, 0, chunk));
            if (!result.Succeeded()) return result;
            bytes = bytes.subspan(chunk.size());
          }
          return IoResult<void>::Success();
        },
        [self = *this, slot] { return Acknowledge(self.Sync(slot, HPK_CLOSE_STREAM, 0)); });
  }

  AsyncInputStream AsyncInput(std::uint32_t slot) const {
    return AsyncInputStream::FromReader(
        [self = *this, slot](std::stop_token stop, std::size_t count) -> Task<IoResult<Bytes>> {
          count = (std::min)(count, MaxStreamChunk);
          auto result = co_await self.Async(slot, HPK_READ_STREAM, count, {}, stop);
          if (result.Succeeded() && result.Value().size() > count) {
            throw Error(ErrorCode::Protocol, "The host returned more than the requested bytes");
          }
          co_return result;
        });
  }

  AsyncOutputStream AsyncOutput(std::uint32_t slot) const {
    return AsyncOutputStream::FromWriter(
        [self = *this, slot](std::stop_token stop, Bytes bytes) -> Task<IoResult<void>> {
          auto remaining = std::span<const std::byte>(bytes);
          while (!remaining.empty()) {
            const auto chunk = remaining.first((std::min)(remaining.size(), MaxStreamChunk));
            auto response = co_await self.Async(slot, HPK_WRITE_STREAM, 0, chunk, stop);
            auto result = Acknowledge(std::move(response));
            if (!result.Succeeded()) co_return result;
            remaining = remaining.subspan(chunk.size());
          }
          co_return IoResult<void>::Success();
        },
        [self = *this, slot](std::stop_token stop) -> Task<IoResult<void>> {
          co_return Acknowledge(co_await self.Async(slot, HPK_CLOSE_STREAM, 0, {}, stop));
        });
  }

private:
  static IoResult<void> Acknowledge(IoResult<Bytes> result) {
    if (!result.Succeeded()) return IoResult<void>(std::move(result).Error());
    if (!result.Value().empty()) throw Error(ErrorCode::Protocol, "Unexpected bytes in a stream acknowledgement");
    return IoResult<void>::Success();
  }

  void Start(std::uint32_t slot, std::uint32_t operation, std::size_t count, std::span<const std::byte> payload,
             hpk_complete_v1 complete, void* user) const {
    if (!invocation_->host) throw Error(ErrorCode::Closed, "The invocation has no stream endpoint");
    const auto status = invocation_->host->stream(invocation_->id, slot, operation, static_cast<std::uint32_t>(count),
        {reinterpret_cast<const std::uint8_t*>(payload.data()), static_cast<std::uint32_t>(payload.size())},
        complete, user);
    if (status) throw Error(static_cast<ErrorCode>(status), "The worker rejected the stream operation");
  }

  IoResult<Bytes> Sync(std::uint32_t slot, std::uint32_t operation, std::size_t count,
                       std::span<const std::byte> payload = {}) const {
    CheckSynchronousStream(Owner());
    struct Reply {
      std::mutex mutex;
      std::condition_variable ready;
      std::optional<IoResult<Bytes>> result;
      std::exception_ptr error;

      static void HPK_CALL Complete(void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
        auto& reply = *static_cast<Reply*>(user);
        std::lock_guard lock(reply.mutex);
        try {
          reply.result.emplace(ReceiveStreamReply(status, buffer));
        } catch (...) {
          reply.error = std::current_exception();
        }
        reply.ready.notify_one();
      }
    } reply;
    Start(slot, operation, count, payload, Reply::Complete, &reply);
    std::unique_lock lock(reply.mutex);
    reply.ready.wait(lock, [&] { return reply.result || reply.error; });
    if (reply.error) std::rethrow_exception(reply.error);
    return std::move(*reply.result);
  }

  Task<IoResult<Bytes>> Async(std::uint32_t slot, std::uint32_t operation, std::size_t count,
                              std::span<const std::byte> payload, std::stop_token stop) const {
    CheckAsyncStream(invocation_->execution, co_await CurrentTaskExecution{}, stop);
    using Completion = CallbackCompletion<IoResult<Bytes>>;
    const auto complete = +[](void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
      const std::unique_ptr<Completion> reply(static_cast<Completion*>(user));
      try {
        reply->Complete(ReceiveStreamReply(status, buffer));
      } catch (...) {
        reply->Fail(std::current_exception());
      }
    };
    auto result = co_await AwaitCallback<IoResult<Bytes>>([&](Completion reply) {
      auto* user = new Completion(std::move(reply));
      try {
        Start(slot, operation, count, payload, complete, user);
      } catch (...) {
        delete user;
        throw;
      }
      return std::function<void()>{};
    });
    if (stop.stop_requested()) throw Error(ErrorCode::Closed, "The stream operation was cancelled");
    co_return result;
  }

  std::shared_ptr<HostInvocation> invocation_;
};

} // namespace huxerui::pluginkit::detail
