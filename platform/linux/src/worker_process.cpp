#include "worker_process.h"

#include "posix_socket.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

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

Handle DuplicateForSpawn(int fd) {
  // Keep the source descriptors above the destination slots used by the spawn actions.
  Handle copy(::fcntl(fd, F_DUPFD_CLOEXEC, WorkerModule + 1));
  if (!copy) PosixError(ErrorCode::Io, "Cannot duplicate worker file descriptor");
  return copy;
}

pid_t Spawn(const std::filesystem::path& executable, int socket, int module, ContractInfo contract) {
  const auto socket_copy = DuplicateForSpawn(socket);
  const auto module_copy = DuplicateForSpawn(module);
  SpawnActions actions;
  for (const auto fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    CheckSpawn(::posix_spawn_file_actions_addopen(&actions.value, fd, "/dev/null", O_RDWR, 0),
               "Cannot redirect worker standard streams");
  }
  CheckSpawn(::posix_spawn_file_actions_adddup2(&actions.value, socket_copy.get(), WorkerSocket),
             "Cannot pass worker socket");
  CheckSpawn(::posix_spawn_file_actions_adddup2(&actions.value, module_copy.get(), WorkerModule),
             "Cannot pass verified plugin file");
  CheckSpawn(::posix_spawn_file_actions_addclosefrom_np(&actions.value, WorkerModule + 1),
             "Cannot restrict worker file descriptors");

  SpawnAttributes attributes;
  CheckSpawn(::posix_spawnattr_setpgroup(&attributes.value, 0), "Cannot set the worker process group");
  sigset_t mask{};
  ::sigemptyset(&mask);
  CheckSpawn(::posix_spawnattr_setsigmask(&attributes.value, &mask), "Cannot set the worker signal mask");
  CheckSpawn(::posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK),
             "Cannot set worker spawn flags");

  auto program = executable.native();
  std::string id(contract.id);
  auto version = std::to_string(contract.version);
  std::string fingerprint(contract.fingerprint);
  char* args[]{program.data(), id.data(), version.data(), fingerprint.data(), nullptr};
  pid_t child = 0;
  CheckSpawn(::posix_spawn(&child, executable.c_str(), &actions.value, &attributes.value, args, environ),
             "Cannot start hpk-worker");
  return child;
}

} // namespace

Worker::Worker(const std::filesystem::path& executable, const std::filesystem::path&, const Handle& verified_module,
               ContractInfo contract, std::vector<Service> services, TaskScope callbacks, PluginStartup startup)
    : services_(std::move(services)), callbacks_(std::move(callbacks)), startup_(std::move(startup)) {
  int sockets[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    PosixError(ErrorCode::Io, "Cannot create worker socket");
  }
  socket_.reset(sockets[0]);
  Handle peer(sockets[1]);
  const auto child = Spawn(executable, peer.get(), verified_module.get(), contract);
  peer.reset();
  StartReaper(child);
}

} // namespace huxerui::pluginkit::detail
