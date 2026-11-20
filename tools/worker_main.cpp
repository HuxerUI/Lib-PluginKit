#include "worker_server.h"
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

namespace huxerui::pluginkit::detail {
namespace {

using namespace worker_server;

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
  if (argc != 4) return 1;
  try {
    Handle input(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[1]), "input handle")));
    Handle output(reinterpret_cast<HANDLE>(ParseInteger<std::uintptr_t>(Utf8(argv[2]), "output handle")));
    Server server(argv[3]);
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
int main(int argc, [[maybe_unused]] char** argv) {
  using namespace huxerui::pluginkit;
  using namespace huxerui::pluginkit::detail;
  if (::getpid() != ::getpgrp() || ::fcntl(WorkerSocket, F_SETFD, FD_CLOEXEC) < 0) return 1;
  try {
#if defined(__APPLE__)
    if (argc != 2) return 1;
    std::filesystem::path module(argv[1]);
    if (!module.is_absolute()) return 1;
    StartHostWatch();
#else
    if (argc != 1 || ::fcntl(WorkerModule, F_SETFD, FD_CLOEXEC) < 0) return 1;
    const auto module = "/proc/self/fd/" + std::to_string(WorkerModule);
    std::thread(WatchHost).detach();
#endif
    Server server(module);
    const SocketStream stream(WorkerSocket);
    Run([&stream](std::span<std::byte> bytes) { stream.Read(bytes); },
        [&stream](std::span<const std::byte> bytes) { stream.Write(bytes); }, server);
  } catch (...) {
    ::_exit(1);
  }
}
#endif
