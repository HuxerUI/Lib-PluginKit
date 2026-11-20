#pragma once

#include "plugin_module.h"
#include <condition_variable>
#include <functional>
#include <map>
#include <unordered_map>

namespace huxerui::pluginkit::detail::worker_server {

struct Instance {
  const hpk_extension_v1& factory;
  std::unique_ptr<void, decltype(hpk_extension_v1::close)> object;
};

struct Outgoing {
  Frame frame;
  hpk_complete_v1 complete = nullptr;
  void* user = nullptr;
};

struct ServiceChannel {
  explicit ServiceChannel(Frame incoming) : request(std::move(incoming)) {}

  void Send(Outgoing message) {
    std::lock_guard lock(mutex);
    if (callback_active) throw Error(ErrorCode::Busy, "The invocation already has an active callback");
    pending.emplace(std::move(message));
    callback_active = true;
    ready.notify_one();
  }

  void CompleteCallback() {
    std::lock_guard lock(mutex);
    callback_active = false;
  }

  void Reply(Frame reply) {
    std::lock_guard lock(mutex);
    if (callback_active) {
      reply = ErrorReply(Error(ErrorCode::Busy, "An invocation callback is still active"));
    }
    reply.kind = FrameKind::Reply;
    reply.call = request.call;
    final.emplace(std::move(reply));
    ready.notify_one();
  }

  Outgoing Receive() {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return pending || final; });
    if (!pending) return {std::move(*final)};
    auto message = std::move(*pending);
    pending.reset();
    return message;
  }

  const Frame request;
  std::uint32_t callback = 0;
  std::uint64_t invocation = 0;

private:
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<Outgoing> pending;
  std::optional<Frame> final;
  bool callback_active = false;
};

// Package Tasks can interleave on one application thread. The ABI invocation token selects the
// package channel independently of Task scheduling; wire call IDs remain local to each channel.
inline thread_local bool application_thread = false;
inline std::uint64_t next_invocation = 0;
inline std::map<std::uint64_t, ServiceChannel*> active_channels;

inline ServiceChannel* FindChannel(std::uint64_t invocation) {
  if (!application_thread) return nullptr;
  const auto found = active_channels.find(invocation);
  return found == active_channels.end() ? nullptr : found->second;
}

inline void Finish(ServiceChannel& channel, Frame reply) {
  // Publish the final reply after the plugin's dispatch Task releases its invocation context.
  if (channel.invocation) active_channels.erase(channel.invocation);
  channel.Reply(std::move(reply));
}

inline std::uint32_t HPK_CALL CallHost(std::uint64_t invocation, std::uint32_t receiver, const char* id, std::uint32_t version,
                              const char* fingerprint, std::uint32_t operation, const std::uint8_t* data,
                              std::uint32_t size, hpk_complete_v1 complete, void* user) noexcept {
  auto* channel = FindChannel(invocation);
  if (!channel) {
    return static_cast<std::uint32_t>(receiver ? ErrorCode::ReceiverClosed : ErrorCode::ServiceUnavailable);
  }
  if (channel->request.synchronous) return static_cast<std::uint32_t>(ErrorCode::CallbackUnavailable);
  try {
    if (!id || !fingerprint || !operation || (size && !data) || !complete || channel->callback == UINT32_MAX) {
      throw Error(ErrorCode::InvalidArgument, "Invalid host service call");
    }
    auto payload = ServicePayload({id, version, fingerprint}, std::as_bytes(std::span(data, size)));
    channel->Send({{0, channel->request.instance, operation, std::move(payload), FrameKind::ServiceCall,
                    channel->request.call, channel->callback + 1, receiver}, complete, user});
    ++channel->callback;
    return 0;
  } catch (const Error& error) {
    return static_cast<std::uint32_t>(error.Code());
  } catch (...) {
    return static_cast<std::uint32_t>(ErrorCode::PluginFailed);
  }
}

inline std::uint32_t HPK_CALL CallStream(std::uint64_t invocation, std::uint32_t slot, std::uint32_t operation,
                                 std::uint32_t count, hpk_buffer_v1 payload, hpk_complete_v1 complete,
                                 void* user) noexcept {
  auto* channel = FindChannel(invocation);
  if (!channel || !slot) {
    return static_cast<std::uint32_t>(ErrorCode::Closed);
  }
  try {
    if (!complete || (payload.size && !payload.data) || channel->callback == UINT32_MAX ||
        !IsStreamOperation(operation, count, payload.size)) {
      throw Error(ErrorCode::InvalidArgument, "Invalid invocation stream operation");
    }
    Bytes bytes;
    if (payload.size) {
      const auto data = std::as_bytes(std::span(payload.data, payload.size));
      bytes.assign(data.begin(), data.end());
    }
    channel->Send({{0, channel->request.instance, operation, std::move(bytes), FrameKind::StreamCall,
                    channel->request.call, channel->callback + 1, slot, false, count}, complete, user});
    ++channel->callback;
    return 0;
  } catch (const Error& error) {
    return static_cast<std::uint32_t>(error.Code());
  } catch (...) {
    return static_cast<std::uint32_t>(ErrorCode::PluginFailed);
  }
}

inline void HPK_CALL FinishInvoke(void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
  auto& channel = *static_cast<ServiceChannel*>(user);
  Frame reply{};
  try {
    CheckPluginStatus(static_cast<std::int32_t>(status));
    if (buffer.size > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "The plugin response exceeds the payload limit");
    reply.instance = channel.request.instance;
    if (buffer.size) {
      if (!buffer.data) throw Error(ErrorCode::Protocol, "The plugin returned an invalid buffer");
      const auto bytes = std::as_bytes(std::span(buffer.data, buffer.size));
      reply.payload.assign(bytes.begin(), bytes.end());
    }
  } catch (const Error& error) {
    reply = ErrorReply(error);
  } catch (...) {
    reply = ErrorReply(Error(ErrorCode::PluginFailed, "Cannot encode the plugin result"));
  }
  Finish(channel, std::move(reply));
}

inline constexpr hpk_host_v1 HostCallbacks{CallHost, CallStream};

class Server {
public:
  explicit Server(std::filesystem::path path, int module_fd = -1) : path_(std::move(path)), module_fd_(module_fd) {}

  std::optional<Frame> Dispatch(ServiceChannel& channel) {
    application_thread = true;
    const auto& request = channel.request;
    if (request.slot) InvalidRequest();
    if (request.code == static_cast<std::uint32_t>(Command::Shutdown)) {
      if (request.instance || request.operation || request.synchronous || !request.payload.empty()) InvalidRequest();
      Shutdown();
      return Frame{};
    }
    if (request.code == static_cast<std::uint32_t>(Command::Activate)) {
      if (binding_ || request.instance || request.operation || request.synchronous) InvalidRequest();
      Activate(PluginStartup::Decode(request.payload));
      return Frame{};
    }
    if (request.code == static_cast<std::uint32_t>(Command::Create)) {
      if (!binding_ || request.instance || request.operation || request.synchronous) InvalidRequest();
      Reader reader(request.payload);
      reader.Record(2);
      const auto point = reader.Text();
      const auto ordinal = Codec<std::uint32_t>::Read(reader);
      reader.Finish();
      const auto* factory = FindExtension(*binding_, point, ordinal);
      if (!factory) InvalidRequest();
      if (instances_.size() >= 256 || next_instance_ == 0) {
        throw Error(ErrorCode::PluginFailed, "Worker instance limit reached");
      }
      void* raw = nullptr;
      const auto id = next_instance_++;
      const auto status = factory->create(activation_, &raw);
      Instance instance{*factory, {raw, factory->close}};
      CheckPluginStatus(status);
      if (!instance.object) throw Error(ErrorCode::Protocol, "The plugin returned an empty instance");
      instances_.emplace(id, std::move(instance));
      return Frame{0, id, 0, {}};
    }

    const auto instance = instances_.find(request.instance);
    if (instance == instances_.end()) throw Error(ErrorCode::Closed, "The worker instance is closed");
    if (request.code == static_cast<std::uint32_t>(Command::Close)) {
      if (request.operation || request.synchronous || !request.payload.empty()) InvalidRequest();
      instances_.erase(instance);
      return Frame{0, request.instance, 0, {}};
    }
    if (request.code != static_cast<std::uint32_t>(Command::Invoke) || !request.operation) {
      InvalidRequest();
    }

    // Operation numbers are local to the instance's contract.
    auto& target = instance->second;
    if (next_invocation == UINT64_MAX) throw Error(ErrorCode::Busy, "Worker invocation tokens exhausted");
    channel.invocation = ++next_invocation;
    active_channels.emplace(channel.invocation, &channel);
    CheckPluginStatus(target.factory.invoke(target.object.get(), &HostCallbacks, channel.invocation, request.operation,
        reinterpret_cast<const std::uint8_t*>(request.payload.data()), static_cast<std::uint32_t>(request.payload.size()),
        FinishInvoke, &channel));
    return std::nullopt;
  }

private:
  [[noreturn]] static void InvalidRequest() { throw Error(ErrorCode::Protocol, "Invalid worker request"); }

  void Shutdown() {
    if (!binding_) return;
    // Instance destructors and the plugin-wide hook can both borrow the context before the library unloads.
    instances_.clear();
    const auto status = binding_->deactivate(std::exchange(activation_, nullptr));
    binding_ = nullptr;

    CheckPluginStatus(status);
  }

  void Activate(const PluginStartup& startup) {
    const auto& binding = LoadPluginModule(path_, module_fd_);
    activation_ = ActivatePlugin(binding, startup, HPK_WORKER_PROCESS);
    binding_ = &binding;
  }

  std::filesystem::path path_;
  int module_fd_ = -1;
  const hpk_plugin_v1* binding_ = nullptr;
  void* activation_ = nullptr;
  std::unordered_map<std::uint32_t, Instance> instances_;
  std::uint32_t next_instance_ = 1;
};

template<class Read, class Write>
void Exchange(Read& read, Write& write, const TaskScope& tasks, Server& server) {
  for (;;) {
    auto request = ReadFrame(read);
    if (request.kind != FrameKind::Request || !request.call || request.callback) {
      throw Error(ErrorCode::Protocol, "Invalid host request frame");
    }
    auto channel = std::make_shared<ServiceChannel>(std::move(request));
    auto dispatch = [channel, &server] {
      try {
        if (auto reply = server.Dispatch(*channel)) Finish(*channel, std::move(*reply));
      } catch (const Error& error) {
        Finish(*channel, ErrorReply(error));
      } catch (...) {
        Finish(*channel, ErrorReply(Error(ErrorCode::PluginFailed, "Worker dispatch failed")));
      }
    };
    tasks.Post(std::move(dispatch));

    for (;;) {
      auto message = channel->Receive();
      WriteFrame(write, message.frame);
      if (message.frame.kind == FrameKind::Reply) break;
      const auto& outgoing = message.frame;
      const auto reply = ReadFrame(read);
      const bool stream = outgoing.kind == FrameKind::StreamCall;
      if (reply.kind != (stream ? FrameKind::StreamReply : FrameKind::ServiceReply) ||
          reply.call != outgoing.call || reply.callback != outgoing.callback || reply.instance != outgoing.instance ||
          reply.slot != outgoing.slot || reply.synchronous ||
          reply.operation != (stream ? outgoing.operation : 0) ||
          (reply.code && !IsUtf8(AsText(reply.payload))) ||
          (!stream && reply.code && !IsErrorCode(reply.code)) ||
          (stream && !reply.code && reply.payload.size() > outgoing.read_limit)) {
        throw Error(ErrorCode::Protocol, "Invalid host callback response");
      }
      channel->CompleteCallback();
      message.complete(message.user, reply.code, {reinterpret_cast<const std::uint8_t*>(reply.payload.data()),
                                                 static_cast<std::uint32_t>(reply.payload.size())});
    }
    if (channel->request.code == static_cast<std::uint32_t>(Command::Shutdown)) return;
  }
}

} // namespace huxerui::pluginkit::detail::worker_server
