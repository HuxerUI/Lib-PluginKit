#pragma once

#include "transport.h"

#include <huxerui/pluginkit/detail/extension.h>

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
class Worker;

std::shared_ptr<LocalPlugin> CreateLocalPlugin(TaskScope tasks, std::function<const hpk_plugin_v1&()> load,
                                               PluginStartup startup, std::vector<RegisteredService> services);
void BeginLocalShutdown(const std::shared_ptr<LocalPlugin>& plugin);
Task<void> FinishLocalShutdown(std::shared_ptr<LocalPlugin> plugin);
void ValidateLocalPlugin(const std::shared_ptr<LocalPlugin>& plugin);

std::shared_ptr<ExtensionTarget> MakeLocalExtension(std::shared_ptr<LocalPlugin> plugin,
    std::string point, std::uint32_t ordinal, std::chrono::milliseconds timeout);
std::shared_ptr<ExtensionTarget> MakeWorkerExtension(
    std::function<std::shared_ptr<Worker>(Deadline, std::stop_token)> start, std::chrono::milliseconds timeout,
    std::stop_token admission, Bytes factory);

} // namespace huxerui::pluginkit::detail
