#pragma once

#include "worker_process.h"

namespace huxerui::pluginkit::detail {

struct RegisteredService {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;
  std::shared_ptr<void> implementation;
  ServiceDispatch dispatch;
};

class LocalPlugin;

std::shared_ptr<LocalPlugin> CreateLocalPlugin(TaskScope tasks, std::function<const hpk_plugin_v1&()> load,
                                               PluginStartup startup, std::vector<RegisteredService> services);
void BeginLocalShutdown(const std::shared_ptr<LocalPlugin>& plugin);
Task<void> FinishLocalShutdown(std::shared_ptr<LocalPlugin> plugin);
void ValidateLocalPlugin(const std::shared_ptr<LocalPlugin>& plugin);

std::shared_ptr<ExtensionTarget> MakeLocalExtension(std::shared_ptr<LocalPlugin> plugin,
                                                     std::chrono::milliseconds timeout);
std::shared_ptr<ExtensionTarget> MakeWorkerExtension(
    std::function<std::shared_ptr<Worker>(Deadline, std::stop_token)> start, std::chrono::milliseconds timeout,
    std::stop_token admission);

} // namespace huxerui::pluginkit::detail
