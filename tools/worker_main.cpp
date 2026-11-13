#include "plugin_module.h"
#include "windowless_runtime.h"

#include <huxerui/pluginkit/contract.h>

#if !defined(_WIN32)
#include "posix_socket.h"

#include <signal.h>
#include <thread>
#endif
#if defined(__APPLE__)
#include "worker_watch.h"
#endif

#include <exception>
#include <functional>
#include <memory>
#include <unordered_map>

namespace huxerui::pluginkit::detail {
namespace {

using Instance = std::unique_ptr<void, decltype(hpk_plugin_v1::close)>;

struct Outgoing {
  Frame frame;
  hpk_complete_v1 complete = nullptr;
  void* user = nullptr;
};

struct ServiceChannel {
  explicit ServiceChannel(Frame incoming) : request(std::move(incoming)) {}

  void Send(Outgoing message) {
    std::lock_guard lock(mutex);
    if (pending) throw Error(ErrorCode::Protocol, "A worker frame is already queued");
    pending.emplace(std::move(message));
    ready.notify_one();
  }

  Outgoing Receive() {
    std::unique_lock lock(mutex);
    ready.wait(lock, [&] { return pending.has_value(); });
    auto message = std::move(*pending);
    pending.reset();
    return message;
  }

  const Frame request;
  std::uint32_t callback = 0;

private:
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<Outgoing> pending;
};

// Only the worker's application thread sets this pointer. The transport owns the channel until its final reply.
thread_local ServiceChannel* current_service_channel = nullptr;

void Finish(ServiceChannel& channel, Frame reply) {
  // Publish the final reply after the plugin's dispatch Task releases its invocation context.
  current_service_channel = nullptr;
  reply.kind = FrameKind::Reply;
  reply.call = channel.request.call;
  channel.Send({std::move(reply)});
}

std::uint32_t HPK_CALL CallHost(std::uint64_t invocation, std::uint32_t receiver, const char* id, std::uint32_t version,
                              const char* fingerprint, std::uint32_t operation, const std::uint8_t* data,
                              std::uint32_t size, hpk_complete_v1 complete, void* user) noexcept {
  auto* channel = current_service_channel;
  if (!channel || channel->request.call != invocation) {
    return static_cast<std::uint32_t>(ErrorCode::ServiceUnavailable);
  }
  if (channel->request.synchronous) return static_cast<std::uint32_t>(ErrorCode::CallbackUnavailable);
  try {
    if (!id || !fingerprint || !operation || (size && !data) || !complete || channel->callback == UINT32_MAX) {
      throw Error(ErrorCode::InvalidArgument, "Invalid host service call");
    }
    if (receiver && receiver != channel->request.receiver) {
      throw Error(ErrorCode::ReceiverClosed, "The receiver is outside its owning invocation");
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

void HPK_CALL FinishInvoke(void* user, std::uint32_t status, hpk_buffer_v1 buffer) noexcept {
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

constexpr hpk_host_v1 HostCallbacks{CallHost};

class Server {
public:
  Server(std::filesystem::path path, ContractInfo contract) : path_(std::move(path)), contract_(contract) {}

  std::optional<Frame> Dispatch(ServiceChannel& channel) {
    const auto& request = channel.request;
    if (request.code == static_cast<std::uint32_t>(Command::Shutdown)) {
      if (request.instance || request.operation || request.receiver || request.synchronous || !request.payload.empty()) InvalidRequest();
      Shutdown();
      return Frame{};
    }
    if (request.code == static_cast<std::uint32_t>(Command::Create)) {
      const bool first_creation = !binding_;
      if (request.instance || request.operation || request.receiver || request.synchronous ||
          (first_creation ? request.payload.empty() : !request.payload.empty())) {
        InvalidRequest();
      }
      if (first_creation) Load(PluginStartup::Decode(request.payload));
      if (instances_.size() >= 256 || next_instance_ == 0) {
        throw Error(ErrorCode::PluginFailed, "Worker instance limit reached");
      }
      void* raw = nullptr;
      const auto id = next_instance_++;
      const auto status = binding_->create(activation_, &raw);
      Instance instance(raw, binding_->close);
      if (status != HPK_OK && first_creation) {
        instance.reset();
        Shutdown();
      }
      CheckPluginStatus(status);
      if (!instance) throw Error(ErrorCode::Protocol, "The plugin returned an empty instance");
      instances_.emplace(id, std::move(instance));
      return Frame{0, id, 0, {}};
    }

    const auto instance = instances_.find(request.instance);
    if (instance == instances_.end()) throw Error(ErrorCode::Closed, "The worker instance is closed");
    if (request.code == static_cast<std::uint32_t>(Command::Close)) {
      if (request.operation || request.receiver || request.synchronous || !request.payload.empty()) InvalidRequest();
      instances_.erase(instance);
      return Frame{0, request.instance, 0, {}};
    }
    if (request.code != static_cast<std::uint32_t>(Command::Invoke) || !request.operation || request.receiver > 1 ||
        (request.synchronous && request.receiver)) {
      InvalidRequest();
    }

    current_service_channel = &channel;
    CheckPluginStatus(binding_->invoke(instance->second.get(), &HostCallbacks, request.call, request.receiver,
                                 request.operation, reinterpret_cast<const std::uint8_t*>(request.payload.data()),
                                 static_cast<std::uint32_t>(request.payload.size()), FinishInvoke, &channel));
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

  void Load(const PluginStartup& startup) {
    PreparePluginData(startup);
    const auto& binding = LoadPluginModule(path_, contract_, HPK_WORKER_PROCESS);
    activation_ = ActivatePlugin(binding, startup);
    binding_ = &binding;
  }

  std::filesystem::path path_;
  ContractInfo contract_;
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
    tasks.Post([channel, &server] {
      try {
        if (auto reply = server.Dispatch(*channel)) Finish(*channel, std::move(*reply));
      } catch (const Error& error) {
        Finish(*channel, ErrorReply(error));
      } catch (...) {
        Finish(*channel, ErrorReply(Error(ErrorCode::PluginFailed, "Worker dispatch failed")));
      }
    });

    for (;;) {
      auto message = channel->Receive();
      WriteFrame(write, message.frame);
      if (message.frame.kind == FrameKind::Reply) break;
      const auto& outgoing = message.frame;
      const auto reply = ReadFrame(read);
      if (reply.kind != FrameKind::ServiceReply || reply.call != outgoing.call || reply.callback != outgoing.callback ||
          reply.instance != outgoing.instance || reply.operation || reply.receiver != outgoing.receiver || reply.synchronous ||
          (reply.code && (!IsErrorCode(reply.code) || !IsUtf8(AsText(reply.payload))))) {
        throw Error(ErrorCode::Protocol, "Invalid host service response");
      }
      message.complete(message.user, reply.code, {reinterpret_cast<const std::uint8_t*>(reply.payload.data()),
                                                 static_cast<std::uint32_t>(reply.payload.size())});
    }
    if (channel->request.code == static_cast<std::uint32_t>(Command::Shutdown)) return;
  }
}

template<class Read, class Write>
Task<void> StartTransport(Read read, Write write, Server& server, WindowlessRuntime& runtime, std::thread& transport) {
  auto tasks = UseApplicationTaskScope();
  try {
    transport = std::thread([&, read = std::move(read), write = std::move(write), tasks]() mutable {
      try {
        Exchange(read, write, tasks, server);
        tasks.Post([&runtime] { runtime.RequestShutdown(); });
      } catch (const Error& error) {
        std::_Exit(error.Code() == ErrorCode::WorkerExited ? 0 : 1);
      } catch (...) {
        std::_Exit(1);
      }
    });
  } catch (...) {
    std::_Exit(1);
  }
  co_return;
}

template<class Read, class Write>
void Run(Read read, Write write, Server& server) {
  const Application application(WindowlessRoot);
  WindowlessRuntime runtime;
  std::thread transport;
  runtime.Run(application, StartTransport(std::move(read), std::move(write), server, runtime, transport));
  if (transport.joinable()) transport.join();
}


#if defined(__linux__)
void WatchHost() {
  // Watch for disconnects while the request thread is inside plugin code.
  pollfd socket{WorkerSocket, POLLRDHUP, 0};
  int ready;
  do {
    ready = ::poll(&socket, 1, -1);
  } while (ready < 0 && errno == EINTR);
  ::kill(-::getpid(), SIGKILL);
  ::_exit(1);
}
#endif

} // namespace
} // namespace huxerui::pluginkit::detail

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
  using namespace huxerui::pluginkit;
  using namespace huxerui::pluginkit::detail;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (argc != 7) return 1;
  try {
    Handle input(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[1]), "input handle")));
    Handle output(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[2]), "output handle")));
    const auto id = Utf8(argv[4]);
    const auto version = ParseInteger<std::uint32_t>(Utf8(argv[5]), "contract version");
    const auto fingerprint = Utf8(argv[6]);
    Server server(argv[3], {id, version, fingerprint});
    Run([&input](std::span<std::byte> bytes) {
      ReadAll(input.get(), bytes, ErrorCode::WorkerExited, "Cannot read worker frame");
    }, [&output](std::span<const std::byte> bytes) {
      WriteAll(output.get(), bytes, ErrorCode::WorkerExited, "Cannot write worker frame");
    }, server);
  } catch (const Error& error) {
    return error.Code() == ErrorCode::WorkerExited ? 0 : 1;
  } catch (...) {
    return 1;
  }
}
#else
int main(int argc, char** argv) {
  using namespace huxerui::pluginkit;
  using namespace huxerui::pluginkit::detail;
  if (::getpid() != ::getpgrp() || ::fcntl(WorkerSocket, F_SETFD, FD_CLOEXEC) < 0) return 1;
  try {
#if defined(__APPLE__)
    if (argc != 5) return 1;
    std::filesystem::path module(argv[4]);
    if (!module.is_absolute()) return 1;
    StartHostWatch();
#else
    if (argc != 4 || ::fcntl(WorkerModule, F_SETFD, FD_CLOEXEC) < 0) return 1;
    const auto module = "/proc/self/fd/" + std::to_string(WorkerModule);
    std::thread(WatchHost).detach();
#endif
    const auto version = ParseInteger<std::uint32_t>(argv[2], "contract version");
    Server server(module, {argv[1], version, argv[3]});
    const SocketStream stream(WorkerSocket);
    Run([&stream](std::span<std::byte> bytes) { stream.Read(bytes); },
        [&stream](std::span<const std::byte> bytes) { stream.Write(bytes); }, server);
  } catch (...) {
    ::_exit(1);
  }
}
#endif
