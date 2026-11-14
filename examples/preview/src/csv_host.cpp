#include "csv_host.h"
#include "windowless_runtime.h"

#include "csv_contract.host.h"
#include "exporters/export_contract.host.h"
#include "services/import_status.bindings.h"

#include <huxerui/pluginkit.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using huxerui::File;
using huxerui::AwaitCallback;
using huxerui::Delay;
using huxerui::Task;
using huxerui::UseApplicationTaskScope;
using namespace huxerui::pluginkit;
using huxerui::pluginkit::detail::WindowlessRoot;
using huxerui::pluginkit::detail::WindowlessRuntime;

static_assert(!std::is_copy_constructible_v<PluginManager>);
static_assert(!std::is_copy_assignable_v<PluginManager>);

using csv_import::Exporters;
using csv_import::Importers;

constexpr char CommandHelp[] =
    "inspect <package>, install <package> <digest> <in_process|worker_process> [status], list, "
    "upgrade <package> <installed-digest> <approved-digest> [status], uninstall <id> <digest> [delete-data], "
    "enable <id>, disable <id>, status, import <file> [plugin-id] [timeout-ms], "
    "round-trip <csv-file> <ssv-file> <plugin-id>, quit";

void RegisterPoints(PluginManager& manager) {
  manager.RegisterExtensionPoint(Importers);
  manager.RegisterExtensionPoint(Exporters);
}

std::uint32_t ProcessId() {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::_getpid());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

class PreviewImportStatus final : public csv_import::ImportStatus {
public:
  explicit PreviewImportStatus(PluginManager& manager) : manager_(manager) {}

  std::uint32_t ImportedRows() const override { return rows.value_or(0); }

  Task<bool> SetImportedRows(csv_import::SetImportedRowsRequest request) override {
    co_await Delay(std::chrono::milliseconds::zero());
    if (std::this_thread::get_id() != owner_) {
      throw Error(ErrorCode::PluginFailed, "Import status must be updated on the preview thread");
    }
    if (manager_.ListPlugins().empty() || manager_.ListExtensions(Importers).empty()) {
      throw Error(ErrorCode::PluginFailed, "The host callback could not read the plugin catalog");
    }
    if (request.rows == UINT32_MAX) co_await manager_.SetEnabled("example.csv-faults", false);
    rows = request.rows;
    co_return true;
  }

  std::optional<std::uint32_t> rows;

private:
  PluginManager& manager_;
  const std::thread::id owner_ = std::this_thread::get_id();
};

class PreviewProgress final : public csv_import::ImportProgress {
public:
  Task<void> OnRows(csv_import::ImportedRows update) override {
    const auto value = co_await huxerui::RunWorker([](std::uint32_t rows) { return rows; }, update.rows);
    if (std::this_thread::get_id() != owner_) {
      throw Error(ErrorCode::PluginFailed, "Import progress must be handled on the preview thread");
    }
    rows = value;
  }

  std::optional<std::uint32_t> rows;

private:
  const std::thread::id owner_ = std::this_thread::get_id();
};

struct ProgressGate {
  std::optional<huxerui::CallbackCompletion<std::uint32_t>> complete;
  bool started = false;
  bool cancelled = false;
  std::uint32_t rows = 0;
};

class HeldProgress final : public csv_import::ImportProgress {
public:
  explicit HeldProgress(std::shared_ptr<ProgressGate> gate) : gate_(std::move(gate)) {}

  Task<void> OnRows(csv_import::ImportedRows) override {
    gate_->rows = co_await AwaitCallback<std::uint32_t>([gate = gate_](auto resolve) {
      gate->complete = std::move(resolve);
      gate->started = true;
      return [gate] { gate->cancelled = true; };
    });
  }

private:
  std::shared_ptr<ProgressGate> gate_;
};

template<class Predicate>
Task<void> WaitFor(Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + DefaultTimeout;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) throw Error(ErrorCode::PluginFailed, "CSV fixture timed out");
    co_await Delay(std::chrono::milliseconds(1));
  }
}

struct ImportOutcome {
  bool done = false;
  std::exception_ptr error;
};

Task<void> ObserveImport(Task<csv_import::ImportResult> call, std::shared_ptr<ImportOutcome> outcome) {
  try {
    (void)co_await std::move(call);
  } catch (...) {
    outcome->error = std::current_exception();
  }
  outcome->done = true;
}

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

std::string ReadCsv(const File& path) {
  auto opened = path.OpenRead();
  if (!opened.Succeeded()) throw Error(ErrorCode::Io, "Cannot open the CSV file");
  auto input = std::move(opened).Value();
  std::string contents;
  std::array<std::byte, 8192> buffer{};
  for (;;) {
    const auto read = input.Read(buffer);
    if (!read.Succeeded()) throw Error(ErrorCode::Io, "Cannot read the CSV file");
    const auto count = read.Value();
    if (!count) break;
    constexpr std::size_t max_csv = 32 * 1024 * 1024;
    if (count > max_csv - contents.size()) throw Error(ErrorCode::PayloadTooLarge, "CSV input exceeds the example limit");
    contents.append(reinterpret_cast<const char*>(buffer.data()), count);
  }
  return contents;
}

Json Describe(const PackageInfo& package) {
  auto provides = Json::array();
  for (const auto& contract : package.provides) {
    provides.push_back({{"id", contract.id}, {"version", contract.version}, {"fingerprint", contract.fingerprint}});
  }
  auto extensions = Json::array();
  for (const auto& extension : package.extensions) {
    extensions.push_back({{"point", extension.point}, {"contract", extension.contract},
                          {"metadata", {{"extensions", extension.file_extensions}}}});
  }
  return {{"id", package.id}, {"version", package.version}, {"digest", package.digest},
          {"provides", std::move(provides)}, {"extensions", std::move(extensions)}};
}

Json List(PluginManager& manager) {
  auto plugins = Json::array();
  for (const auto& plugin : manager.ListPlugins()) {
    auto record = Describe(plugin.package);
    record["enabled"] = plugin.enabled;
    record["mode"] = plugin.mode == ExecutionMode::InProcess ? "in_process" : "worker_process";
    record["worker_pid"] = plugin.worker_pid ? Json(*plugin.worker_pid) : Json(nullptr);
    record["pending_upgrade"] = plugin.pending_upgrade ? Describe(*plugin.pending_upgrade) : Json(nullptr);
    plugins.push_back(std::move(record));
  }
  const auto describe = [&manager](auto point) {
    auto result = Json::array();
    for (const auto& extension : manager.ListExtensions(point)) {
      result.push_back({{"plugin", extension.plugin_id}, {"extensions", extension.file_extensions}});
    }
    return result;
  };
  return {{"plugins", std::move(plugins)}, {"importers", describe(Importers)}, {"exporters", describe(Exporters)}};
}

Task<Json> Import(PluginManager& manager, const std::vector<std::string>& args) {
  const File path(args[1]);
  auto suffix = path.Extension();
  for (auto& character : suffix) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  std::optional<ExtensionInfo> selected;
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (args.size() >= 3 && extension.plugin_id != args[2]) continue;
    if (std::ranges::find(extension.file_extensions, suffix) == extension.file_extensions.end()) continue;
    if (selected) throw Error(ErrorCode::Ambiguous, "Several importers match this file");
    selected = extension;
  }
  if (!selected) throw Error(ErrorCode::NotFound, "No enabled importer matches the file");
  auto timeout = DefaultTimeout;
  if (args.size() == 4) {
    std::uint32_t milliseconds = 0;
    const auto end = args[3].data() + args[3].size();
    const auto result = std::from_chars(args[3].data(), end, milliseconds);
    if (result.ec != std::errc{} || result.ptr != end || milliseconds == 0 ||
        std::chrono::milliseconds(milliseconds) > std::chrono::hours(24)) {
      throw Error(ErrorCode::InvalidArgument, "Invalid timeout in milliseconds");
    }
    timeout = std::chrono::milliseconds(milliseconds);
  }
  auto source = co_await huxerui::RunWorker(ReadCsv, path);
  auto importer = manager.CreateExtension(Importers, *selected, timeout);
  auto progress = std::make_shared<PreviewProgress>();
  const auto delimiter = importer->Delimiter();
  const auto plugin_pid = importer->ProcessId();
  auto imported = co_await importer->Import({std::move(source)}, progress);

  auto issues = Json::array();
  for (const auto& issue : imported.issues) {
    issues.push_back({{"code", issue.code}, {"row", issue.row}, {"column", issue.column}, {"message", issue.message}});
  }
  Json table = nullptr;
  if (imported.table) table = {{"header", imported.table->header}, {"rows", imported.table->rows}};
  co_return Json{{"table", std::move(table)}, {"issues", std::move(issues)}, {"delimiter", delimiter},
          {"plugin_pid", plugin_pid},
          {"progress_rows", progress->rows ? Json(*progress->rows) : Json(nullptr)}};
}

Task<Json> RoundTrip(PluginManager& manager, const std::vector<std::string>& args) {
  const auto& plugin = args[3];
  const auto all_importers = manager.CreateExtensions(Importers).size();
  auto importers = manager.CreateExtensions(Importers, plugin);
  if (importers.size() != 2 || importers[0]->Delimiter() != "," || importers[1]->Delimiter() != ";") {
    throw Error(ErrorCode::PluginFailed, "Expected comma and semicolon importers in manifest order");
  }
  try {
    (void)manager.CreateExtension(Importers, plugin);
    throw Error(ErrorCode::PluginFailed, "A plugin ID unexpectedly selected one of several factories");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::Ambiguous) throw;
  }
  const auto candidates = manager.ListExtensions(Importers);
  const auto candidate = std::ranges::find_if(candidates, [&](const auto& entry) {
    return entry.plugin_id == plugin && std::ranges::find(entry.file_extensions, ".ssv") != entry.file_extensions.end();
  });
  if (candidate == candidates.end()) throw Error(ErrorCode::NotFound, "No semicolon importer for this package");
  auto exporter = manager.CreateExtension(Exporters, plugin);
  const auto pid = importers[0]->ProcessId();
  if (importers[1]->ProcessId() != pid || exporter->ProcessId() != pid) {
    throw Error(ErrorCode::PluginFailed, "Factories from the same package must share a process");
  }

  const auto parse = [](auto& importer, std::string source) -> Task<csv_import::Table> {
    const csv_import::ImportRequest request{std::move(source)};
    auto result = co_await importer->Import(request, nullptr);
    if (!result.table) throw Error(ErrorCode::PluginFailed, "Cannot parse the round-trip input");
    co_return std::move(*result.table);
  };
  const auto csv = co_await huxerui::RunWorker(ReadCsv, File(args[1]));
  const auto ssv = co_await huxerui::RunWorker(ReadCsv, File(args[2]));
  const auto table = co_await parse(importers[0], csv);
  {
    auto semicolon = manager.CreateExtension(Importers, *candidate);
    if (semicolon->Delimiter() != ";" || semicolon->ProcessId() != pid) {
      throw Error(ErrorCode::PluginFailed, "The candidate did not select the semicolon importer");
    }
    const auto other = co_await parse(semicolon, ssv);
    if (table.header != other.header || table.rows != other.rows) {
      throw Error(ErrorCode::PluginFailed, "CSV and semicolon input did not produce the same table");
    }
  }
  const csv_import::ExportRequest request{table.header, table.rows};
  auto exported = co_await exporter->Export(request);
  const auto round_trip = co_await parse(importers[0], exported);
  if (table.header != round_trip.header || table.rows != round_trip.rows) {
    throw Error(ErrorCode::PluginFailed, "The exported CSV did not preserve the input table");
  }
  co_return Json{{"all_importers", all_importers}, {"package_importers", importers.size()},
                 {"format", exporter->Format()}, {"plugin_pid", pid}, {"csv", std::move(exported)}};
}

Task<Json> CheckReceiverLifetime(PluginManager& manager, std::string plugin) {
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (extension.plugin_id != plugin) continue;
    const auto tasks = UseApplicationTaskScope();
    auto importer = manager.CreateExtension(Importers, extension);
    auto first = std::make_shared<PreviewProgress>();
    auto second = std::make_shared<PreviewProgress>();
    const csv_import::ImportRequest remember{"remember"};
    const csv_import::ImportRequest stale{"stale"};
    (void)co_await importer->Import(remember, first);
    try {
      (void)co_await importer->Import(stale, second);
      throw Error(ErrorCode::PluginFailed, "An expired receiver was accepted");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::ReceiverClosed) throw;
    }
    auto held = std::make_shared<ProgressGate>();
    auto running = std::make_shared<ImportOutcome>();
    (void)tasks.Launch(ObserveImport(importer->Import(remember, std::make_shared<HeldProgress>(held)), running));
    co_await WaitFor([held] { return held->started; });
    // The receiver is still suspended when this independent task reads the catalog.
    auto catalog_during_callback = List(manager);
    auto queued = std::make_shared<ImportOutcome>();
    const auto queued_task = tasks.Launch(ObserveImport(importer->Import(remember, second), queued));
    co_await Delay(std::chrono::milliseconds(20));
    queued_task.Cancel();
    try {
      auto short_call = manager.CreateExtension(Importers, extension, std::chrono::milliseconds(30));
      (void)co_await short_call->Import(remember, second);
      throw Error(ErrorCode::PluginFailed, "A queued invocation exceeded its deadline without failing");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::DeadlineExceeded) throw;
    }
    held->complete->Complete(1);
    co_await WaitFor([running] { return running->done; });
    if (running->error) std::rethrow_exception(running->error);
    if (queued->done) throw Error(ErrorCode::PluginFailed, "A cancelled queued task resumed");

    // A lazy call retains its arguments, while closing the instance revokes its call target.
    auto pending = [&] {
      auto unused = manager.CreateExtension(Importers, extension);
      return unused->Import(remember, second);
    }();
    try {
      (void)co_await std::move(pending);
      throw Error(ErrorCode::PluginFailed, "A closed instance ran a pending task");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::Closed) throw;
    }
    auto cancelled = std::make_shared<ProgressGate>();
    auto receiver = std::make_shared<HeldProgress>(cancelled);
    std::weak_ptr<HeldProgress> receiver_lifetime = receiver;
    auto outcome = std::make_shared<ImportOutcome>();
    const auto task = tasks.Launch(ObserveImport(importer->Import(remember, std::move(receiver)), outcome));
    co_await WaitFor([cancelled] { return cancelled->started; });
    task.Cancel();
    co_await WaitFor([&] { return cancelled->cancelled && receiver_lifetime.expired(); });
    cancelled->complete->Complete(9);
    if (outcome->done) throw Error(ErrorCode::PluginFailed, "A cancelled invocation resumed");

    auto restarted = manager.CreateExtension(Importers, extension, DefaultTimeout * 2);
    auto disabled = std::make_shared<ProgressGate>();
    auto interrupted = std::make_shared<ImportOutcome>();
    (void)tasks.Launch(ObserveImport(
        restarted->Import(remember, std::make_shared<HeldProgress>(disabled)), interrupted));
    co_await WaitFor([disabled] { return disabled->started; });
    co_await manager.SetEnabled(plugin, false);
    co_await WaitFor([&] { return interrupted->done && disabled->cancelled; });
    try {
      if (interrupted->error) std::rethrow_exception(interrupted->error);
      throw Error(ErrorCode::PluginFailed, "The disabled worker returned an invocation result");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::Closed) throw;
    }
    disabled->complete->Complete(9);
    if (!disabled->cancelled) throw Error(ErrorCode::PluginFailed, "Disabling the plugin left its receiver active");
    co_await manager.SetEnabled(plugin, true);
    co_return Json{{"catalog_during_callback", std::move(catalog_during_callback)},
                   {"first_rows", first->rows.value_or(0)}, {"active_rows", held->rows},
                   {"second_called", second->rows.has_value()}, {"cancelled_delivered", cancelled->rows != 0},
                   {"cancelled_released", receiver_lifetime.expired()}, {"late_rows", disabled->rows}};
  }
  throw Error(ErrorCode::NotFound, "The receiver test plugin is unavailable");
}

Task<Json> CheckPluginContext(PluginManager& manager, std::string plugin, bool disable) {
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (extension.plugin_id != plugin) continue;
    auto second = manager.CreateExtension(Importers, extension);
    const auto context = [](auto& importer) -> Task<std::vector<std::string>> {
      const csv_import::ImportRequest request{"context"};
      auto result = co_await importer->Import(request, std::make_shared<PreviewProgress>());
      if (!result.table || result.table->rows.size() != 1) {
        throw Error(ErrorCode::PluginFailed, "Expected the fixture's plugin context");
      }
      co_return std::move(result.table->rows.front());
    };
    std::vector<std::string> first_context;
    std::vector<std::string> second_context;
    {
      auto first = manager.CreateExtension(Importers, extension);
      (void)first->Delimiter();
      (void)second->Delimiter();
      first_context = co_await context(first);
      second_context = co_await context(second);
    }
    // Remote release is asynchronous. Query the surviving instance until it observes the destructor.
    auto remaining_context = co_await context(second);
    const auto deadline = std::chrono::steady_clock::now() + DefaultTimeout;
    while (remaining_context.back() != "1" && std::chrono::steady_clock::now() < deadline) {
      co_await Delay(std::chrono::milliseconds(1));
      remaining_context = co_await context(second);
    }
    if (disable) {
      // Leave the second instance alive so shutdown has to destroy it before calling OnDeactivate.
      const csv_import::ImportRequest request{"context"};
      auto pending = second->Import(request, std::make_shared<PreviewProgress>());
      co_await manager.SetEnabled(plugin, false);
      try {
        (void)co_await std::move(pending);
        throw Error(ErrorCode::PluginFailed, "A disabled plugin ran a pending task");
      } catch (const Error& error) {
        if (error.Code() != ErrorCode::Closed) throw;
      }
    }
    co_return Json{{"first", std::move(first_context)}, {"second", std::move(second_context)},
            {"after_close", std::move(remaining_context)}};
  }
  throw Error(ErrorCode::NotFound, "The context test plugin is unavailable");
}

Task<Json> CheckDisableDuringCreate(const File& profile, const File& package) {
  const auto tasks = UseApplicationTaskScope();
  auto manager = std::make_shared<PluginManager>(profile, std::nullopt, tasks);
  RegisterPoints(*manager);
  const auto info = PluginManager::InspectPackage(package);
  try {
    manager->InstallPackage(package, info.digest);
    throw Error(ErrorCode::PluginFailed, "The in-process-only host installed a worker package");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::UnsupportedMode) throw;
  }
  manager->InstallPackage(package, info.digest, {.mode = ExecutionMode::InProcess, .services = {}});

  const auto candidate = manager->ListExtensions(Importers).front();
  for (int attempt = 0; attempt < 20; ++attempt) {
    struct Outcome {
      std::optional<ExtensionHandle<csv_import::TableImporter>> handle;
      std::exception_ptr error;
      bool created = false;
      bool disabled = false;
    };
    const auto outcome = std::make_shared<Outcome>();
    (void)tasks.Launch([manager, outcome, id = info.id]() -> Task<void> {
      try {
        co_await manager->SetEnabled(id, false);
      } catch (...) {
        outcome->error = std::current_exception();
      }
      outcome->disabled = true;
    });
    // The second post lets the disable Task start before this call tries to create the first handle.
    tasks.Post([tasks, manager, outcome, candidate] {
      tasks.Post([manager, outcome, candidate] {
        try {
          outcome->handle.emplace(manager->CreateExtension(Importers, candidate));
        } catch (const Error& error) {
          if (error.Code() != ErrorCode::Disabled) outcome->error = std::current_exception();
        } catch (...) {
          outcome->error = std::current_exception();
        }
        outcome->created = true;
      });
    });
    co_await WaitFor([outcome] { return outcome->disabled && outcome->created; });
    if (outcome->error) std::rethrow_exception(outcome->error);
    if (!manager->CreateExtensions(Importers).empty() || !manager->CreateExtensions(Exporters, info.id).empty()) {
      throw Error(ErrorCode::PluginFailed, "The disabled CSV plugin is still listed");
    }
    if (outcome->handle) {
      try {
        (void)(*outcome->handle)->Delimiter();
        throw Error(ErrorCode::PluginFailed, "A handle created during disable can still call the CSV plugin");
      } catch (const Error& error) {
        if (error.Code() != ErrorCode::Closed) throw;
      }
    }
    co_await manager->SetEnabled(info.id, true);
  }
  auto importer = manager->CreateExtension(Importers, candidate);
  const auto delimiter = importer->Delimiter();
  co_await manager->Shutdown();
  co_return Json{{"delimiter", delimiter}};
}

Task<UpgradeStatus> Upgrade(PluginManager& manager, const std::vector<std::string>& args) {
  UpgradeApproval approval{args[2], args[3], {}};
  if (args.size() == 5 && args[4] == "status") approval.services.emplace_back(Contract<csv_import::ImportStatus>::Info.id);
  else if (args.size() != 4) throw Error(ErrorCode::InvalidArgument, CommandHelp);
  return manager.UpgradePackage(File(args[1]), std::move(approval));
}

Task<Json> CheckWorkerUpgrade(PluginManager& manager, const std::vector<std::string>& args) {
  const auto package = PluginManager::InspectPackage(File(args[1]));
  const auto& id = package.id;
  const auto exporters = manager.ListExtensions(Exporters);
  const auto candidate = std::ranges::find(exporters, id, &ExtensionInfo::plugin_id);
  if (candidate == exporters.end()) throw Error(ErrorCode::NotFound, "The upgrade fixture has no exporter");
  auto previous = manager.CreateExtension(Importers, id);
  auto previous_exporter = manager.CreateExtension(Exporters, *candidate);
  auto pending = manager.CreateExtension(Importers, id);
  const auto old_pid = previous->ProcessId();
  if (previous_exporter->ProcessId() != old_pid) throw Error(ErrorCode::PluginFailed, "Expected one worker per package");
  if (co_await Upgrade(manager, args) != UpgradeStatus::Applied) {
    throw Error(ErrorCode::PluginFailed, "The worker upgrade requires a restart");
  }
  const auto check_closed = [](auto& old) {
    try {
      (void)old->ProcessId();
      throw Error(ErrorCode::PluginFailed, "An old handle followed the worker upgrade");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::Closed) throw;
    }
  };
  check_closed(previous);
  check_closed(previous_exporter);
  check_closed(pending);
  try {
    (void)manager.CreateExtension(Exporters, *candidate);
    throw Error(ErrorCode::PluginFailed, "An old candidate selected an exporter in a new release");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::PackageChanged) throw;
  }
  auto next = manager.CreateExtension(Importers, id);
  auto exporter = manager.CreateExtension(Exporters, id);
  const auto new_pid = next->ProcessId();
  if (exporter->ProcessId() != new_pid) throw Error(ErrorCode::PluginFailed, "Expected one replacement worker");
  co_return Json{{"old_pid", old_pid}, {"new_pid", new_pid}};
}

Task<Json> CheckInProcessUpgrade(const File& profile, const std::vector<std::string>& args) {
  const auto package = PluginManager::InspectPackage(File(args[1]));
  const auto& id = package.id;
  const auto tasks = UseApplicationTaskScope();
  {
    PluginManager manager(profile, std::nullopt, tasks);
    RegisterPoints(manager);
    auto handle = manager.CreateExtension(Importers, id);
    auto exporter = manager.CreateExtension(Exporters, id);
    (void)handle->Delimiter();
    (void)exporter->Format();
    if (co_await Upgrade(manager, args) != UpgradeStatus::RestartRequired) {
      throw Error(ErrorCode::PluginFailed, "The in-process upgrade applied without a restart");
    }
    (void)handle->Delimiter();
    (void)exporter->Format();
    co_await manager.Shutdown();
  }
  PluginManager reopened(profile, std::nullopt, tasks);
  RegisterPoints(reopened);
  const auto changes = co_await reopened.ApplyPendingChanges();
  if (!changes.empty()) throw Error(ErrorCode::PluginFailed, "Reopening the manager applied the in-process upgrade");
  const auto installed = reopened.ListPlugins();
  const auto plugin = std::ranges::find_if(installed, [&](const auto& entry) { return entry.package.id == package.id; });
  if (plugin == installed.end() || !plugin->pending_upgrade || plugin->pending_upgrade->digest != package.digest) {
    throw Error(ErrorCode::PluginFailed, "The approved in-process upgrade was lost");
  }
  auto handle = reopened.CreateExtension(Importers, id);
  const csv_import::ImportRequest request{"context"};
  const auto context = co_await handle->Import(request, nullptr);
  if (!context.table || context.table->rows.at(0).at(2) != args[2]) {
    throw Error(ErrorCode::PluginFailed, "The reopened manager selected the replacement too early");
  }
  co_await reopened.Shutdown();
  co_return Json{{"result", "restart_required"}, {"current_digest", args[2]}, {"pending_digest", package.digest}};
}

Task<Json> Command(PluginManager& manager, const std::shared_ptr<PreviewImportStatus>& status,
             const std::vector<std::string>& args) {
  const auto& name = args.front();
  if (name == "pid" && args.size() == 1) co_return Json{{"pid", ProcessId()}};
  if (name == "reset-status" && args.size() == 1) {
    status->rows.reset();
    co_return Json::object();
  }
  if (name == "check-receiver-lifetime" && args.size() == 2) co_return co_await CheckReceiverLifetime(manager, args[1]);
  if (name == "check-worker-upgrade" && (args.size() == 4 || args.size() == 5)) {
    co_return co_await CheckWorkerUpgrade(manager, args);
  }
  if (name == "check-plugin-context" && (args.size() == 2 || (args.size() == 3 && args[2] == "disable"))) {
    co_return co_await CheckPluginContext(manager, args[1], args.size() == 3);
  }
  if (name == "status" && args.size() == 1) {
    co_return Json{{"last_imported_rows", status->rows ? Json(*status->rows) : Json(nullptr)}};
  }
  if (name == "inspect" && args.size() == 2) co_return Describe(PluginManager::InspectPackage(File(args[1])));
  if (name == "install" && (args.size() == 4 || (args.size() == 5 && args[4] == "status"))) {
    InstallOptions options;
    if (args[3] == "in_process") options.mode = ExecutionMode::InProcess;
    else if (args[3] != "worker_process") throw Error(ErrorCode::InvalidArgument, "Invalid execution mode");
    if (args.size() == 5) options.services.emplace_back(Contract<csv_import::ImportStatus>::Info.id);
    manager.InstallPackage(File(args[1]), args[2], std::move(options));
    co_return Json::object();
  }
  if (name == "upgrade" && (args.size() == 4 || args.size() == 5)) {
    const auto result = co_await Upgrade(manager, args);
    co_return Json{{"result", result == UpgradeStatus::Applied ? "applied" : "restart_required"}};
  }
  if (name == "uninstall" && (args.size() == 3 || (args.size() == 4 && args[3] == "delete-data"))) {
    const auto policy = args.size() == 4 ? PluginDataPolicy::Delete : PluginDataPolicy::Keep;
    const auto result = co_await manager.UninstallPackage(args[1], args[2], policy);
    co_return Json{{"result", result == UninstallStatus::Removed ? "removed" : "cleanup_pending"}};
  }
  if ((name == "enable" || name == "disable") && args.size() == 2) {
    co_await manager.SetEnabled(args[1], name == "enable");
    co_return Json::object();
  }
  if (name == "list" && args.size() == 1) co_return List(manager);
  if (name == "import" && args.size() >= 2 && args.size() <= 4) co_return co_await Import(manager, args);
  if (name == "round-trip" && args.size() == 4) co_return co_await RoundTrip(manager, args);
  throw Error(ErrorCode::InvalidArgument, CommandHelp);
}

void Print(const Json& response) {
  std::cout << response.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
}

void PrintError(const std::exception& error) {
  const auto* kit_error = dynamic_cast<const Error*>(&error);
  Print({{"ok", false}, {"error", kit_error ? ErrorName(kit_error->Code()) : "io"}, {"message", error.what()}});
}

Task<void> Report(PluginManager& manager, const std::shared_ptr<PreviewImportStatus>& status,
            const std::vector<std::string>& args) {
  auto response = co_await Command(manager, status, args);
  response["ok"] = true;
  Print(response);
}

std::vector<std::string> Split(const std::string& line) {
  std::istringstream input(line);
  std::vector<std::string> args;
  while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
    std::string value;
    if (!(input >> std::quoted(value, '"', '\0'))) throw Error(ErrorCode::InvalidArgument, "Unclosed quoted argument");
    args.push_back(std::move(value));
  }
  return args;
}

Task<void> RunCommands(std::span<const fs::path> args, WindowlessRuntime& runtime, int& exit_code) {
  try {
    const auto profile = args.empty() ? csv_import::preview::DefaultProfile() : args.front();
    if (args.size() == 3 && args[1] == "check-disable-during-create") {
      auto response = co_await CheckDisableDuringCreate(File(profile.u8string()), File(args[2].u8string()));
      response["ok"] = true;
      Print(response);
      runtime.RequestShutdown();
      co_return;
    }
    if (args.size() == 5 && args[1] == "check-in-process-upgrade") {
      const std::vector<std::string> command{Utf8(args[1]), Utf8(args[2]), Utf8(args[3]), Utf8(args[4])};
      auto response = co_await CheckInProcessUpgrade(File(profile.u8string()), command);
      response["ok"] = true;
      Print(response);
      runtime.RequestShutdown();
      co_return;
    }
    PluginManager manager(File(profile.u8string()), File(csv_import::preview::WorkerPath().u8string()),
                          UseApplicationTaskScope());
    auto status = std::make_shared<PreviewImportStatus>(manager);
    manager.ProvideService<csv_import::ImportStatus>(status);
    RegisterPoints(manager);
    for (const auto& change : co_await manager.ApplyPendingChanges()) {
      if (!change.error) continue;
      std::cerr << "Plugin upgrade failed for " << change.plugin_id << " (" << ErrorName(*change.error) << ") "
                << change.message << '\n';
      if (change.restart_required) std::cerr << "Restart the host to load the previous plugin version.\n";
    }
    if (args.size() > 1) {
      try {
        std::vector<std::string> command;
        for (const auto& arg : args.subspan(1)) command.push_back(Utf8(arg));
        co_await Report(manager, status, command);
      } catch (const std::exception& error) {
        PrintError(error);
        exit_code = 1;
      }
    } else {
      if (args.empty()) std::cerr << "Enter one command per line.\n" << CommandHelp << '\n';
      for (;;) {
        auto line = co_await huxerui::RunWorker([]() -> std::optional<std::string> {
          std::string value;
          if (!std::getline(std::cin, value)) return std::nullopt;
          return value;
        });
        if (!line) break;
        try {
          const auto command = Split(*line);
          if (command.empty()) continue;
          if (command.size() == 1 && command.front() == "quit") break;
          co_await Report(manager, status, command);
        } catch (const std::exception& error) {
          PrintError(error);
        }
      }
    }
    co_await manager.Shutdown();
  } catch (const std::exception& error) {
    PrintError(error);
    exit_code = 1;
  }
  runtime.RequestShutdown();
}

} // namespace

int csv_import::preview::Run(std::span<const fs::path> args) {
  const huxerui::Application application(WindowlessRoot);
  WindowlessRuntime runtime;
  int exit_code = 0;
  runtime.Run(application, RunCommands(args, runtime, exit_code));
  return exit_code;
}
