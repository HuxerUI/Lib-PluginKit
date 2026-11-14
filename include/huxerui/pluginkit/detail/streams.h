#pragma once

/// @file
/// @brief Invocation-owned sources and SDK stream adapters used by generated bindings.

#include <huxerui/pluginkit/detail/invocation.h>
#include <huxerui/stream.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <variant>

namespace huxerui::pluginkit::detail {

inline constexpr std::size_t MaxStreamChunk = HPK_MAX_STREAM_CHUNK;

using StreamInput = std::variant<std::monostate, InputStream, AsyncInputStream>;
using StreamOutput = std::variant<std::monostate, OutputStream, AsyncOutputStream>;

struct StreamSources {
  StreamInput input;
  StreamOutput output;
};

// An active operation retains the endpoints while the call revokes access to them.
class CallStreams {
public:
  CallStreams(StreamInput input, StreamOutput output)
      : slots_((input.index() ? static_cast<std::uint32_t>(HPK_INPUT_STREAM) : 0u) |
               (output.index() ? static_cast<std::uint32_t>(HPK_OUTPUT_STREAM) : 0u)),
        sources_(std::make_shared<StreamSources>(StreamSources{std::move(input), std::move(output)})) {}

  std::uint32_t Slots() const noexcept { return slots_; }

  void Revoke() noexcept {
    std::shared_ptr<StreamSources> released;
    {
      std::lock_guard lock(mutex_);
      released = std::move(sources_);
    }
  }

  void SetDeadline(std::chrono::steady_clock::time_point deadline) noexcept { deadline_ = deadline; }

  void CheckOutput() {
    std::lock_guard lock(mutex_);
    if (active_) throw Error(ErrorCode::Busy, "A stream operation is still active");
    if ((slots_ & HPK_OUTPUT_STREAM) && !output_closed_) {
      throw Error(ErrorCode::Io, "The plugin did not successfully close its output stream");
    }
  }

  IoResult<std::size_t> Read(std::span<std::byte> buffer) {
    auto access = Acquire(HPK_INPUT_STREAM);
    return Get<InputStream>(access.sources->input).Read(buffer);
  }

  Task<IoResult<Bytes>> ReadAsync(std::size_t count) {
    auto access = Acquire(HPK_INPUT_STREAM);
    co_return co_await Get<AsyncInputStream>(access.sources->input).ReadAsync(count);
  }

  IoResult<void> Write(std::span<const std::byte> bytes) {
    auto access = Acquire(HPK_OUTPUT_STREAM);
    auto result = Get<OutputStream>(access.sources->output).Write(bytes);
    SetOutputState(result, false);
    return result;
  }

  Task<IoResult<void>> WriteAsync(Bytes bytes) {
    auto access = Acquire(HPK_OUTPUT_STREAM);
    auto result = co_await Get<AsyncOutputStream>(access.sources->output).WriteAsync(std::move(bytes));
    SetOutputState(result, false);
    co_return result;
  }

  IoResult<void> Close() {
    auto access = Acquire(HPK_OUTPUT_STREAM);
    auto result = Get<OutputStream>(access.sources->output).Close();
    SetOutputState(result, true);
    return result;
  }

  Task<IoResult<void>> CloseAsync() {
    auto access = Acquire(HPK_OUTPUT_STREAM);
    auto result = co_await Get<AsyncOutputStream>(access.sources->output).CloseAsync();
    SetOutputState(result, true);
    co_return result;
  }

private:
  struct Access {
    Access(CallStreams& owner, std::shared_ptr<StreamSources> sources) : owner(owner), sources(std::move(sources)) {}
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;

    CallStreams& owner;
    std::shared_ptr<StreamSources> sources;
    ~Access() {
      std::lock_guard lock(owner.mutex_);
      owner.active_ = false;
    }
  };

  Access Acquire(std::uint32_t slot) {
    std::lock_guard lock(mutex_);
    if (!sources_ || !(slots_ & slot)) throw Error(ErrorCode::Closed, "The stream is outside its owning invocation");
    if (std::chrono::steady_clock::now() >= deadline_) {
      throw Error(ErrorCode::DeadlineExceeded, "The stream operation exceeded the invocation deadline");
    }
    if (active_) throw Error(ErrorCode::Busy, "The invocation already has an active stream operation");
    if (slot == HPK_OUTPUT_STREAM) {
      if (output_failed_ || output_closed_) throw Error(ErrorCode::Closed, "The invocation output stream is closed");
      // Only a completed write or close can clear this state, including when the backend throws.
      output_failed_ = true;
    }
    active_ = true;
    return {*this, sources_};
  }

  template<class Stream, class Sources>
  static Stream& Get(Sources& sources) {
    const auto stream = std::get_if<Stream>(&sources);
    if (!stream) throw Error(ErrorCode::Protocol, "The stream type differs from the invoked contract");
    return *stream;
  }

  void SetOutputState(const IoResult<void>& result, bool close) {
    std::lock_guard lock(mutex_);
    output_failed_ = !result.Succeeded();
    output_closed_ = close && result.Succeeded();
  }

  const std::uint32_t slots_;
  std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::time_point::max();
  std::mutex mutex_;
  std::shared_ptr<StreamSources> sources_;
  bool active_ = false;
  bool output_failed_ = false;
  bool output_closed_ = false;
};

struct RevokeStreams {
  std::shared_ptr<CallStreams> streams;
  ~RevokeStreams() {
    if (streams) streams->Revoke();
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

InputStream LocalInput(const std::shared_ptr<CallStreams>& streams);
OutputStream LocalOutput(const std::shared_ptr<CallStreams>& streams);
AsyncInputStream LocalInput(const std::shared_ptr<CallStreams>& streams, TaskExecution execution);
AsyncOutputStream LocalOutput(const std::shared_ptr<CallStreams>& streams, TaskExecution execution);

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

  InputStream Input() const {
    return InputStream::FromReader([self = *this](std::span<std::byte> buffer) -> IoResult<std::size_t> {
      const auto count = (std::min)(buffer.size(), MaxStreamChunk);
      auto data = self.Sync(HPK_INPUT_STREAM, HPK_READ_STREAM, count);
      if (!data.Succeeded()) return IoResult<std::size_t>(std::move(data).Error());
      const auto& bytes = data.Value();
      if (bytes.size() > count) throw Error(ErrorCode::Protocol, "The host returned more than the requested bytes");
      std::copy(bytes.begin(), bytes.end(), buffer.begin());
      return IoResult<std::size_t>(bytes.size());
    });
  }

  OutputStream Output() const {
    return OutputStream::FromWriter(
        [self = *this](std::span<const std::byte> bytes) -> IoResult<void> {
          while (!bytes.empty()) {
            const auto chunk = bytes.first((std::min)(bytes.size(), MaxStreamChunk));
            auto result = Acknowledge(self.Sync(HPK_OUTPUT_STREAM, HPK_WRITE_STREAM, 0, chunk));
            if (!result.Succeeded()) return result;
            bytes = bytes.subspan(chunk.size());
          }
          return IoResult<void>::Success();
        },
        [self = *this] { return Acknowledge(self.Sync(HPK_OUTPUT_STREAM, HPK_CLOSE_STREAM, 0)); });
  }

  AsyncInputStream AsyncInput() const {
    return AsyncInputStream::FromReader(
        [self = *this](std::stop_token stop, std::size_t count) -> Task<IoResult<Bytes>> {
          count = (std::min)(count, MaxStreamChunk);
          auto result = co_await self.Async(HPK_INPUT_STREAM, HPK_READ_STREAM, count, {}, stop);
          if (result.Succeeded() && result.Value().size() > count) {
            throw Error(ErrorCode::Protocol, "The host returned more than the requested bytes");
          }
          co_return result;
        });
  }

  AsyncOutputStream AsyncOutput() const {
    return AsyncOutputStream::FromWriter(
        [self = *this](std::stop_token stop, Bytes bytes) -> Task<IoResult<void>> {
          auto remaining = std::span<const std::byte>(bytes);
          while (!remaining.empty()) {
            const auto chunk = remaining.first((std::min)(remaining.size(), MaxStreamChunk));
            auto response = co_await self.Async(HPK_OUTPUT_STREAM, HPK_WRITE_STREAM, 0, chunk, stop);
            auto result = Acknowledge(std::move(response));
            if (!result.Succeeded()) co_return result;
            remaining = remaining.subspan(chunk.size());
          }
          co_return IoResult<void>::Success();
        },
        [self = *this](std::stop_token stop) -> Task<IoResult<void>> {
          co_return Acknowledge(co_await self.Async(HPK_OUTPUT_STREAM, HPK_CLOSE_STREAM, 0, {}, stop));
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
