#include "platform_files.h"
#include "transport.h"

#include <pluginkit/contract.h>

#if !defined(_WIN32)
#include "posix_socket.h"

#include <dlfcn.h>
#include <signal.h>
#include <thread>
#endif
#if defined(__APPLE__)
#include "worker_watch.h"
#endif

#include <memory>
#include <unordered_map>

namespace pluginkit::detail {
namespace {

#if defined(_WIN32)
struct UnloadModule {
  void operator()(HMODULE module) const noexcept { ::FreeLibrary(module); }
};
using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, UnloadModule>;
#else
struct UnloadModule {
  void operator()(void* module) const noexcept { ::dlclose(module); }
};
using Module = std::unique_ptr<void, UnloadModule>;
#endif

using Instance = std::unique_ptr<void, decltype(hpk_plugin_v1::close)>;

void CheckStatus(std::int32_t status) {
  switch (status) {
    case HPK_OK: return;
    case HPK_INVALID_REQUEST: throw Error(ErrorCode::Protocol, "The plugin rejected the request payload");
    case HPK_PAYLOAD_TOO_LARGE: throw Error(ErrorCode::PayloadTooLarge, "The plugin response exceeds the payload limit");
    case HPK_PLUGIN_FAILED: throw Error(ErrorCode::PluginFailed, "The plugin implementation failed");
    default: throw Error(ErrorCode::Protocol, "The plugin returned an invalid status");
  }
}

class Server {
public:
  Server(std::filesystem::path path, ContractInfo contract) : path_(std::move(path)), contract_(contract) {}

  Frame Dispatch(const Frame& request) {
    if (request.code == static_cast<std::uint32_t>(Command::Open)) {
      if (request.instance || request.operation || !request.payload.empty()) InvalidRequest();
      if (!binding_) Load();
      if (instances_.size() >= 256 || next_instance_ == 0) {
        throw Error(ErrorCode::PluginFailed, "Worker instance limit reached");
      }
      void* raw = nullptr;
      const auto status = binding_->create(&raw);
      Instance instance(raw, binding_->close);
      CheckStatus(status);
      if (!instance) throw Error(ErrorCode::Protocol, "The plugin returned an empty instance");
      const auto id = next_instance_++;
      instances_.emplace(id, std::move(instance));
      return {0, id, 0, {}};
    }

    const auto instance = instances_.find(request.instance);
    if (instance == instances_.end()) throw Error(ErrorCode::Closed, "The worker instance is closed");
    if (request.code == static_cast<std::uint32_t>(Command::Close)) {
      if (request.operation || !request.payload.empty()) InvalidRequest();
      instances_.erase(instance);
      return {0, request.instance, 0, {}};
    }
    if (request.code != static_cast<std::uint32_t>(Command::Invoke) || !request.operation) InvalidRequest();

    struct Response {
      const hpk_plugin_v1& binding;
      hpk_buffer_v1 buffer{};
      ~Response() {
        if (buffer.data || buffer.owner) binding.release(buffer);
      }
    } response{*binding_};
    CheckStatus(binding_->invoke(instance->second.get(), request.operation, request.payload.data(),
                                 static_cast<std::uint32_t>(request.payload.size()), &response.buffer));
    const auto& buffer = response.buffer;
    if (buffer.size > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "The plugin response exceeds the payload limit");
    if (!buffer.size || !buffer.data) throw Error(ErrorCode::Protocol, "The plugin returned an invalid buffer");
    return {0, request.instance, 0, Bytes(buffer.data, buffer.data + buffer.size)};
  }

private:
  [[noreturn]] static void InvalidRequest() { throw Error(ErrorCode::Protocol, "Invalid worker request"); }

  void Load() {
#if defined(_WIN32)
    Module module(LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!module) WindowsError(ErrorCode::InvalidPackage, "Cannot load plugin.dll");
    const auto address = GetProcAddress(module.get(), "pluginkit_query_v1");
#else
    int mode = RTLD_NOW | RTLD_LOCAL;
#if defined(__APPLE__)
    mode |= RTLD_FIRST;
#endif
    Module module(::dlopen(path_.c_str(), mode));
    if (!module) {
      auto message = std::string("Cannot load ") + ModuleName;
      if (const auto* detail = ::dlerror(); detail && IsUtf8(detail)) message += ": " + std::string(detail);
      throw Error(ErrorCode::InvalidPackage, message);
    }
    const auto address = ::dlsym(module.get(), "pluginkit_query_v1");
#endif
    if (!address) throw Error(ErrorCode::ContractMismatch, "Missing pluginkit_query_v1 export");
    const auto query = reinterpret_cast<hpk_query_v1>(address);
    const auto* binding = query();
    if (!binding || binding->struct_size != sizeof(hpk_plugin_v1) || binding->abi_version != HPK_ABI_VERSION ||
        !binding->contract_id || !binding->contract_fingerprint || contract_.id != binding->contract_id ||
        contract_.version != binding->contract_version || contract_.fingerprint != binding->contract_fingerprint ||
        !binding->create || !binding->invoke || !binding->release || !binding->close) {
      throw Error(ErrorCode::ContractMismatch, "The module descriptor does not match the installed contract");
    }
    module_ = std::move(module);
    binding_ = binding;
  }

  std::filesystem::path path_;
  ContractInfo contract_;
  // Instance deleters run before this member unloads the plugin library.
  Module module_;
  const hpk_plugin_v1* binding_ = nullptr;
  std::unordered_map<std::uint32_t, Instance> instances_;
  std::uint32_t next_instance_ = 1;
};

template<class Read, class Write>
void Run(Read&& read, Write&& write, Server& server) {
  for (;;) {
    auto request = ReadFrame(read);
    Frame reply{};
    try {
      reply = server.Dispatch(request);
    } catch (const Error& error) {
      const auto message = AsBytes(error.what());
      reply = {static_cast<std::uint32_t>(error.Code()), 0, 0, Bytes(message.begin(), message.end())};
    }
    WriteFrame(write, reply);
  }
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
} // namespace pluginkit::detail

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
  using namespace pluginkit;
  using namespace pluginkit::detail;
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  if (argc != 7) return 1;
  try {
    Handle input(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[1]), "input handle")));
    Handle output(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[2]), "output handle")));
    const auto id = Utf8(argv[4]);
    const auto version = ParseInteger<std::uint32_t>(Utf8(argv[5]), "contract version");
    const auto fingerprint = Utf8(argv[6]);
    Server server(argv[3], {id, version, fingerprint});
    Run([&input](std::span<std::uint8_t> bytes) {
      ReadAll(input.get(), bytes, ErrorCode::WorkerExited, "Cannot read worker frame");
    }, [&output](ByteView bytes) {
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
  using namespace pluginkit;
  using namespace pluginkit::detail;
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
    Run([&stream](std::span<std::uint8_t> bytes) { stream.Read(bytes); },
        [&stream](ByteView bytes) { stream.Write(bytes); }, server);
  } catch (...) {
    ::_exit(1);
  }
}
#endif
