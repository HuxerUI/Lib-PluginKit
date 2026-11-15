#pragma once

#include "transport.h"

#include <huxerui/pluginkit.h>

namespace huxerui::pluginkit::detail {

struct RegisteredService {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;
  std::shared_ptr<void> implementation;
  ServiceDispatch dispatch;
};

// Looks up a granted service and checks its complete contract before either invocation path uses it.
const RegisteredService& RequireHostService(std::span<const RegisteredService> services, ContractInfo contract);

class LocalPlugin;
class PluginRun;
class Worker;

struct WorkerStart {
  std::shared_ptr<Worker> worker;
  bool started;
};

using WorkerStarter = std::function<WorkerStart(Deadline, std::stop_token)>;
using LocalStarter = std::function<std::shared_ptr<PluginRun>(PluginStartup&)>;

std::shared_ptr<LocalPlugin> CreateLocalPlugin(TaskScope tasks, std::function<const hpk_plugin_v1&()> load,
                                               PluginStartup startup, std::vector<RegisteredService> services,
                                               LocalStarter start, std::shared_ptr<PluginRun> run);
void BeginLocalShutdown(const std::shared_ptr<LocalPlugin>& plugin,
                        PluginStopReason reason = PluginStopReason::Shutdown);
Task<void> FinishLocalShutdown(std::shared_ptr<LocalPlugin> plugin);
void ValidateLocalPlugin(const std::shared_ptr<LocalPlugin>& plugin);

std::shared_ptr<ExtensionTarget> MakeLocalExtension(std::shared_ptr<LocalPlugin> plugin,
    std::string point, std::uint32_t ordinal, std::chrono::milliseconds timeout);
std::shared_ptr<ExtensionTarget> MakeWorkerExtension(WorkerStarter start, std::chrono::milliseconds timeout,
                                                     std::stop_token admission, Bytes factory);

} // namespace huxerui::pluginkit::detail
