#include "worker_process.h"

#include "posix_socket.h"

#include <crt_externs.h>
#include <signal.h>
#include <spawn.h>

namespace huxerui::pluginkit::detail {
namespace {

void CheckSpawn(int error, std::string_view operation) {
  if (error) PosixError(ErrorCode::Io, operation, error);
}

struct SpawnActions {
  SpawnActions() { CheckSpawn(::posix_spawn_file_actions_init(&value), "Cannot initialize worker file actions"); }
  ~SpawnActions() { ::posix_spawn_file_actions_destroy(&value); }
  posix_spawn_file_actions_t value{};
};

struct SpawnAttributes {
  SpawnAttributes() { CheckSpawn(::posix_spawnattr_init(&value), "Cannot initialize worker spawn attributes"); }
  ~SpawnAttributes() { ::posix_spawnattr_destroy(&value); }
  posix_spawnattr_t value{};
};

pid_t Spawn(const std::filesystem::path& executable, int socket, const std::filesystem::path& module) {
  Handle socket_copy(::fcntl(socket, F_DUPFD_CLOEXEC, WorkerSocket + 1));
  if (!socket_copy) PosixError(ErrorCode::Io, "Cannot duplicate worker socket");

  SpawnActions actions;
  for (const auto fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    CheckSpawn(::posix_spawn_file_actions_addopen(&actions.value, fd, "/dev/null", O_RDWR, 0),
               "Cannot redirect worker standard streams");
  }
  CheckSpawn(::posix_spawn_file_actions_adddup2(&actions.value, socket_copy.get(), WorkerSocket),
             "Cannot pass worker socket");

  SpawnAttributes attributes;
  CheckSpawn(::posix_spawnattr_setpgroup(&attributes.value, 0), "Cannot set the worker process group");
  sigset_t mask{};
  ::sigemptyset(&mask);
  CheckSpawn(::posix_spawnattr_setsigmask(&attributes.value, &mask), "Cannot set the worker signal mask");
  constexpr short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT;
  CheckSpawn(::posix_spawnattr_setflags(&attributes.value, flags), "Cannot set worker spawn flags");

  auto program = executable.native();
  auto module_path = module.native();
  char* args[]{program.data(), module_path.data(), nullptr};

  // Use the approved package path without dyld environment overrides.
  std::vector<char*> environment;
  for (auto entry = *_NSGetEnviron(); *entry; ++entry) {
    const std::string_view value(*entry);
    if (!value.starts_with("DYLD_") && !value.starts_with("LD_LIBRARY_PATH=")) environment.push_back(*entry);
  }
  environment.push_back(nullptr);
  pid_t child = 0;
  CheckSpawn(::posix_spawn(&child, executable.c_str(), &actions.value, &attributes.value, args, environment.data()),
             "Cannot start hpk-worker");
  return child;
}

} // namespace

Worker::Worker(const std::filesystem::path& executable, const std::filesystem::path& module, const Handle&,
               std::vector<RegisteredService> services, TaskScope callbacks, PluginStartup startup)
    : services_(std::move(services)), callbacks_(std::move(callbacks)), startup_(std::move(startup)) {
  int sockets[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    PosixError(ErrorCode::Io, "Cannot create worker socket");
  }
  socket_.reset(sockets[0]);
  Handle peer(sockets[1]);
  const int enabled = 1;
  for (const auto fd : sockets) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
      PosixError(ErrorCode::Io, "Cannot configure worker socket");
    }
  }
  const auto child = Spawn(executable, peer.get(), module);
  peer.reset();
  StartReaper(child);
}

} // namespace huxerui::pluginkit::detail
