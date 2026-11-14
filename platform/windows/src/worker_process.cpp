#include "worker_process.h"

#include <future>
#include <thread>

namespace huxerui::pluginkit::detail {
namespace {

struct Pipe {
  Handle read;
  Handle write;
};

Pipe CreateWorkerPipe(bool child_reads) {
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read = nullptr;
  HANDLE write = nullptr;
  if (!CreatePipe(&read, &write, &security, 0)) WindowsError(ErrorCode::Io, "Cannot create worker pipe");
  Pipe pipe{Handle(read), Handle(write)};
  if (!SetHandleInformation(child_reads ? write : read, HANDLE_FLAG_INHERIT, 0)) {
    WindowsError(ErrorCode::Io, "Cannot restrict worker pipe inheritance");
  }
  return pipe;
}

class HandleList {
public:
  explicit HandleList(std::array<HANDLE, 2>& handles) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) WindowsError(ErrorCode::Io, "Cannot size worker handle list");
    storage_ = std::make_unique<std::byte[]>(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.get());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
      WindowsError(ErrorCode::Io, "Cannot initialize worker handle list");
    }
    list_.reset(list);
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof(handles),
                                   nullptr, nullptr)) {
      WindowsError(ErrorCode::Io, "Cannot set worker handle list");
    }
  }

  LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_.get(); }

private:
  struct DeleteList {
    void operator()(LPPROC_THREAD_ATTRIBUTE_LIST list) const noexcept { DeleteProcThreadAttributeList(list); }
  };
  std::unique_ptr<std::byte[]> storage_;
  std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, DeleteList> list_;
};

} // namespace

Worker::Worker(const std::filesystem::path& executable, const std::filesystem::path& module, const Handle&,
               std::vector<Service> services, TaskScope callbacks, PluginStartup plugin)
    : services_(std::move(services)), callbacks_(std::move(callbacks)), startup_(std::move(plugin)) {
  auto request = CreateWorkerPipe(true);
  auto response = CreateWorkerPipe(false);
  std::array handles{request.read.get(), response.write.get()};
  HandleList inherit(handles);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = inherit.Get();

  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job) WindowsError(ErrorCode::Io, "Cannot create worker job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    WindowsError(ErrorCode::Io, "Cannot configure worker job");
  }

  std::wstring command = L"\"" + executable.native() + L"\" " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[0])) + L" " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(handles[1])) + L" \"" + module.native() + L"\"";
  PROCESS_INFORMATION created{};
  const auto working_directory = executable.parent_path();
  // Attach the suspended worker to the kill-on-close job before it can load the plugin.
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                      working_directory.c_str(), &startup.StartupInfo, &created)) {
    WindowsError(ErrorCode::Io, "Cannot start hpk-worker");
  }
  Handle process(created.hProcess);
  Handle thread(created.hThread);
  if (!AssignProcessToJobObject(job.get(), process.get())) {
    const auto error = GetLastError();
    TerminateProcess(process.get(), 1);
    WindowsError(ErrorCode::Io, "Cannot attach hpk-worker to its job", error);
  }
  if (ResumeThread(thread.get()) == DWORD(-1)) WindowsError(ErrorCode::Io, "Cannot resume hpk-worker");

  pid_ = created.dwProcessId;
  job_ = std::move(job);
  process_ = std::move(process);
  to_worker_ = std::move(request.write);
  from_worker_ = std::move(response.read);
}

Worker::~Worker() = default;

std::optional<std::uint32_t> Worker::Pid() const noexcept {
  if (!active_ || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) return std::nullopt;
  return pid_;
}

void Worker::Stop() noexcept {
  std::lock_guard lock(stop_);
  {
    std::lock_guard queue_lock(requests_);
    if (!active_.exchange(false)) return;
  }
  request_ready_.notify_all();
  generation_stop_.request_stop();
  job_.reset();
}

void Worker::WaitForExit(std::chrono::milliseconds timeout) {
  const auto result = WaitForSingleObject(process_.get(), static_cast<DWORD>(timeout.count()));
  if (result == WAIT_TIMEOUT) throw Error(ErrorCode::DeadlineExceeded, "The retired plugin worker has not exited");
  if (result != WAIT_OBJECT_0) WindowsError(ErrorCode::Io, "Cannot wait for the retired plugin worker");
}

Frame Worker::Exchange(const Frame& request, Deadline deadline, std::stop_token stop,
                       const ReceiverRegistration* receiver) {
  // A separate pipe helper lets the request thread enforce the I/O deadline.
  return ExchangeFrames(request, deadline, stop, receiver, [this, deadline](const Frame& outgoing) {
    std::packaged_task<Frame()> exchange([this, &outgoing] {
      WriteFrame([this](std::span<const std::byte> bytes) {
        WriteAll(to_worker_.get(), bytes, ErrorCode::WorkerExited, "Cannot write worker frame");
      }, outgoing);
      return ReadFrame([this](std::span<std::byte> bytes) {
        ReadAll(from_worker_.get(), bytes, ErrorCode::WorkerExited, "Cannot read worker frame");
      });
    });
    auto result = exchange.get_future();
    std::jthread io(std::move(exchange));
    if (result.wait_until(deadline) != std::future_status::ready) {
      // Interrupt blocked pipe I/O so the thread can join before the frame goes out of scope.
      Stop();
      CancelSynchronousIo(io.native_handle());
      throw Error(ErrorCode::DeadlineExceeded, "The plugin worker exceeded the request deadline");
    }
    return result.get();
  });
}

} // namespace huxerui::pluginkit::detail
