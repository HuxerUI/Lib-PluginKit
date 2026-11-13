#include "csv_host.h"
#include "windowless_runtime.h"

#include "csv_contract.host.h"
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

constexpr ExtensionPoint<csv_import::TableImporter> Importers{"example.table-importers"};
constexpr char CommandHelp[] =
    "inspect <package>, install <package> <digest>, list, enable <id>, disable <id>, "
    "provide-status <digest>, status, import <file> [plugin-id] [timeout-ms], quit";

class PreviewImportStatus final : public csv_import::ImportStatus {
public:
  Task<bool> SetImportedRows(csv_import::SetImportedRowsRequest request) override {
    co_await Delay(std::chrono::milliseconds::zero());
    if (std::this_thread::get_id() != owner_) {
      throw Error(ErrorCode::PluginFailed, "Import status must be updated on the preview thread");
    }
    rows = request.rows;
    co_return true;
  }

  std::optional<std::uint32_t> rows;

private:
  const std::thread::id owner_ = std::this_thread::get_id();
};

class ReentryStatus final : public csv_import::ImportStatus {
public:
  ReentryStatus(PluginManager& manager, std::shared_ptr<PreviewImportStatus> status)
      : manager_(manager), status_(std::move(status)) {}

  Task<bool> SetImportedRows(csv_import::SetImportedRowsRequest request) override {
    co_await Delay(std::chrono::milliseconds(1));
    if (manager_.ListPlugins().empty() || manager_.ListExtensions(Importers).empty()) {
      throw Error(ErrorCode::PluginFailed, "The host callback could not read the plugin catalog");
    }
    if (request.rows == UINT32_MAX) co_await manager_.SetEnabled("example.csv-faults", false);
    co_return co_await status_->SetImportedRows(request);
  }

private:
  PluginManager& manager_;
  std::shared_ptr<PreviewImportStatus> status_;
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
    if (count > MaxPayload - contents.size()) throw Error(ErrorCode::PayloadTooLarge, "CSV input exceeds the payload limit");
    contents.append(reinterpret_cast<const char*>(buffer.data()), count);
  }
  return contents;
}

Json Describe(const PackageInfo& package) {
  return {{"id", package.id}, {"version", package.version}, {"digest", package.digest},
          {"contribution", package.contribution}, {"extensions", package.extensions}};
}

Json List(PluginManager& manager) {
  auto plugins = Json::array();
  for (const auto& plugin : manager.ListPlugins()) {
    auto record = Describe(plugin.package);
    record["enabled"] = plugin.enabled;
    record["worker_pid"] = plugin.worker_pid ? Json(*plugin.worker_pid) : Json(nullptr);
    plugins.push_back(std::move(record));
  }
  auto importers = Json::array();
  for (const auto& extension : manager.ListExtensions(Importers)) {
    importers.push_back({{"plugin", extension.id.plugin}, {"id", extension.id.contribution},
                         {"extensions", extension.extensions}});
  }
  return {{"plugins", std::move(plugins)}, {"importers", std::move(importers)}};
}

Task<Json> Import(PluginManager& manager, const std::vector<std::string>& args) {
  const File path(args[1]);
  auto suffix = path.Extension();
  for (auto& character : suffix) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  std::optional<ExtensionId> selected;
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (args.size() >= 3 && extension.id.plugin != args[2]) continue;
    if (std::ranges::find(extension.extensions, suffix) == extension.extensions.end()) continue;
    if (selected) throw Error(ErrorCode::InvalidArgument, "Several importers match this file. Specify a plugin ID.");
    selected = extension.id;
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
  auto importer = co_await manager.CreateExtension(Importers, *selected);
  auto progress = std::make_shared<PreviewProgress>();
  const csv_import::ImportRequest request{std::move(source)};
  auto imported = co_await importer.Import(request, progress, timeout);
  co_await importer.Close();

  auto issues = Json::array();
  for (const auto& issue : imported.issues) {
    issues.push_back({{"code", issue.code}, {"row", issue.row}, {"column", issue.column}, {"message", issue.message}});
  }
  Json table = nullptr;
  if (imported.table) table = {{"header", imported.table->header}, {"rows", imported.table->rows}};
  co_return Json{{"table", std::move(table)}, {"issues", std::move(issues)},
          {"progress_rows", progress->rows ? Json(*progress->rows) : Json(nullptr)}};
}

Task<Json> CheckReceiverLifetime(PluginManager& manager, std::string plugin) {
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (extension.id.plugin != plugin) continue;
    const auto tasks = UseApplicationTaskScope();
    auto importer = co_await manager.CreateExtension(Importers, extension.id);
    auto first = std::make_shared<PreviewProgress>();
    auto second = std::make_shared<PreviewProgress>();
    const csv_import::ImportRequest remember{"remember"};
    const csv_import::ImportRequest stale{"stale"};
    (void)co_await importer.Import(remember, first);
    try {
      (void)co_await importer.Import(stale, second);
      throw Error(ErrorCode::PluginFailed, "An expired receiver was accepted");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::ReceiverClosed) throw;
    }
    auto held = std::make_shared<ProgressGate>();
    auto running = std::make_shared<ImportOutcome>();
    (void)tasks.Launch(ObserveImport(importer.Import(remember, std::make_shared<HeldProgress>(held)), running));
    co_await WaitFor([held] { return held->started; });
    // The receiver is still suspended when this independent task reads the catalog.
    auto catalog_during_callback = List(manager);
    auto queued = std::make_shared<ImportOutcome>();
    const auto queued_task = tasks.Launch(ObserveImport(importer.Import(remember, second), queued));
    co_await Delay(std::chrono::milliseconds(20));
    queued_task.Cancel();
    try {
      (void)co_await importer.Import(remember, second, std::chrono::milliseconds(30));
      throw Error(ErrorCode::PluginFailed, "A queued invocation exceeded its deadline without failing");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::DeadlineExceeded) throw;
    }
    held->complete->Complete(1);
    co_await WaitFor([running] { return running->done; });
    if (running->error) std::rethrow_exception(running->error);
    if (queued->done) throw Error(ErrorCode::PluginFailed, "A cancelled queued task resumed");

    // A lazy call retains its arguments, while closing the instance revokes its call target.
    auto unused = co_await manager.CreateExtension(Importers, extension.id);
    auto pending = unused.Import(remember, second);
    co_await unused.Close();
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
    const auto task = tasks.Launch(ObserveImport(importer.Import(remember, std::move(receiver)), outcome));
    co_await WaitFor([cancelled] { return cancelled->started; });
    task.Cancel();
    co_await WaitFor([&] { return cancelled->cancelled && receiver_lifetime.expired(); });
    cancelled->complete->Complete(9);
    if (outcome->done) throw Error(ErrorCode::PluginFailed, "A cancelled invocation resumed");

    auto restarted = co_await manager.CreateExtension(Importers, extension.id);
    auto disabled = std::make_shared<ProgressGate>();
    auto interrupted = std::make_shared<ImportOutcome>();
    (void)tasks.Launch(ObserveImport(
        restarted.Import(remember, std::make_shared<HeldProgress>(disabled), DefaultTimeout * 2), interrupted));
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
    if (extension.id.plugin != plugin) continue;
    auto first = co_await manager.CreateExtension(Importers, extension.id);
    auto second = co_await manager.CreateExtension(Importers, extension.id);
    const auto context = [](auto& importer) -> Task<std::vector<std::string>> {
      const csv_import::ImportRequest request{"context"};
      auto result = co_await importer.Import(request, std::make_shared<PreviewProgress>());
      if (!result.table || result.table->rows.size() != 1) {
        throw Error(ErrorCode::PluginFailed, "Expected the fixture's plugin context");
      }
      co_return std::move(result.table->rows.front());
    };
    auto first_context = co_await context(first);
    auto second_context = co_await context(second);
    co_await first.Close();
    auto remaining_context = co_await context(second);
    if (disable) {
      // Leave the second instance alive so shutdown has to destroy it before calling Deactivate.
      const csv_import::ImportRequest request{"context"};
      auto pending = second.Import(request, std::make_shared<PreviewProgress>());
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

Task<Json> Command(PluginManager& manager, const std::shared_ptr<PreviewImportStatus>& status,
             const std::vector<std::string>& args) {
  const auto& name = args.front();
  if (name == "provide-status" && args.size() == 2) {
    manager.ProvideService<csv_import::ImportStatus>(status, args[1]);
    co_return Json::object();
  }
  if (name == "provide-reentry-status" && args.size() == 2) {
    manager.ProvideService<csv_import::ImportStatus>(std::make_shared<ReentryStatus>(manager, status), args[1]);
    co_return Json::object();
  }
  if (name == "check-receiver-lifetime" && args.size() == 2) co_return co_await CheckReceiverLifetime(manager, args[1]);
  if (name == "check-plugin-context" && (args.size() == 2 || (args.size() == 3 && args[2] == "disable"))) {
    co_return co_await CheckPluginContext(manager, args[1], args.size() == 3);
  }
  if (name == "status" && args.size() == 1) {
    co_return Json{{"last_imported_rows", status->rows ? Json(*status->rows) : Json(nullptr)}};
  }
  if (name == "inspect" && args.size() == 2) co_return Describe(PluginManager::InspectPackage(File(args[1])));
  if (name == "install" && args.size() == 3) {
    manager.InstallPackage(File(args[1]), args[2]);
    co_return Json::object();
  }
  if ((name == "enable" || name == "disable") && args.size() == 2) {
    co_await manager.SetEnabled(args[1], name == "enable");
    co_return Json::object();
  }
  if (name == "list" && args.size() == 1) co_return List(manager);
  if (name == "import" && args.size() >= 2 && args.size() <= 4) co_return co_await Import(manager, args);
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
    PluginManager manager(File(profile.u8string()), File(csv_import::preview::WorkerPath().u8string()),
                          UseApplicationTaskScope());
    auto status = std::make_shared<PreviewImportStatus>();
    manager.RegisterExtensionPoint(Importers);
    if (args.size() > 1) {
      std::vector<std::string> command;
      for (const auto& arg : args.subspan(1)) command.push_back(Utf8(arg));
      co_await Report(manager, status, command);
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
