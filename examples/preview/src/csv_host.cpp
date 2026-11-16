#include "csv_host.h"
#include "windowless_runtime.h"

#include "codec_record.bindings.h"
#include "codec_mapped.bindings.h"
#include "csv_contract.host.h"
#include "exporters/export_contract.host.h"
#include "services/import_status.bindings.h"

#include <huxerui/pluginkit.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <random>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using huxerui::File;
using huxerui::AsyncInputStream;
using huxerui::AsyncOutputStream;
using huxerui::IoResult;
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
    "settings <id> [locale], set-settings <id> <revision> <overrides-file>, "
    "enable <id>, disable <id>, status, import <file> [plugin-id] [timeout-ms], "
    "round-trip <csv-file> <ssv-file> <plugin-id>, convert <sync|async> <ssv-file> <csv-file> <plugin-id> [chunk-kib], "
    "convert-pair <sync|async> <first-input> <first-output> <second-input> <second-output> <plugin-id>, quit";

void CheckContractCodecs() {
  using namespace csv_import::codec_check;
  using huxerui::Bytes;
  using huxerui::pluginkit::Contract;
  using huxerui::pluginkit::detail::Decode;
  using huxerui::pluginkit::detail::Encode;

  static_assert(Contract<RecordColumns>::Info.id == Contract<ReorderedColumns>::Info.id);
  static_assert(Contract<RecordColumns>::Info.fingerprint != Contract<ReorderedColumns>::Info.fingerprint);

  const ColumnCounts counts{ColumnCount{19}};
  const auto record = Encode<RecordColumns>(counts);
  const auto mapped = Encode<MappedColumns>(counts);
  // The same nested value is a CBOR record in one contract and a scalar in the other.
  if (record != Bytes{std::byte{0x81}, std::byte{0x81}, std::byte{0x81}, std::byte{19}} ||
      mapped != Bytes{std::byte{0x81}, std::byte{0x81}, std::byte{19}} ||
      Decode<ColumnCounts, RecordColumns>(record).at(0).value().value != 19 ||
      Decode<ColumnCounts, MappedColumns>(mapped).at(0).value().value != 19) {
    throw Error(ErrorCode::PluginFailed, "The generated contracts did not retain their wire representations");
  }

  constexpr double scale = 1.25;
  if (Decode<double, MappedColumns>(Encode<MappedColumns>(scale)) != scale) {
    throw Error(ErrorCode::PluginFailed, "The explicit scalar mapping did not round-trip");
  }
}

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
  Task<void> OnRows(std::uint32_t count, std::uint64_t) override {
    const auto value = co_await huxerui::RunWorker([](std::uint32_t rows) { return rows; }, count);
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

  Task<void> OnRows(std::uint32_t, std::uint64_t) override {
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

struct CallOutcome {
  bool done = false;
  std::exception_ptr error;
};

PluginEventSubscription RecordPluginEvents(PluginManager& manager, std::string_view plugin,
                                           std::vector<PluginEvent>& events) {
  return manager.SubscribeEvents([id = std::string(plugin), &events](const PluginEvent& event) {
    if (event.package.id == id) events.push_back(event);
  });
}

template<class T>
Task<void> ObserveCall(Task<T> call, std::shared_ptr<CallOutcome> outcome) {
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

template<class T>
T RequireIo(huxerui::IoResult<T> result) {
  if (!result.Succeeded()) throw Error(ErrorCode::Io, result.Error().message);
  return std::move(result).Value();
}

huxerui::AsyncInputStream StringInput(std::string text) {
  return huxerui::AsyncInputStream::FromReader(
      [text = std::move(text), offset = std::size_t{}](std::stop_token, std::size_t count) mutable
          -> Task<huxerui::IoResult<huxerui::Bytes>> {
        const auto chunk = std::string_view(text).substr(offset, count);
        offset += chunk.size();
        huxerui::Bytes bytes(chunk.size());
        if (!chunk.empty()) std::memcpy(bytes.data(), chunk.data(), chunk.size());
        co_return huxerui::IoResult<huxerui::Bytes>(std::move(bytes));
      });
}

class ConversionProgress final : public csv_import::ImportProgress {
public:
  Task<void> OnRows(std::uint32_t count, std::uint64_t bytes) override {
    rows = count;
    input_bytes = bytes;
    before_eof = before_eof || !eof;
    co_return;
  }

  std::uint32_t rows = 0;
  std::uint64_t input_bytes = 0;
  bool before_eof = false;
  bool eof = false;
};

// Keep the staging directory alive through the last SDK or PluginKit I/O lease.
struct StagedOutput {
  File directory;
  File file;

  explicit StagedOutput(File dir) : directory(std::move(dir)), file(directory.Child("output.csv")) {}
  ~StagedOutput() {
    (void)file.Delete();
    (void)directory.Delete();
  }
};

std::shared_ptr<StagedOutput> StageOutput(const File& destination) {
  std::random_device random;
  const auto parent = destination.Parent().value_or(File("."));
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    auto directory = parent.Child(".csv-export-" + std::to_string(random()) + "-" + std::to_string(random()));
    std::error_code error;
    const auto path = directory.Path();
    if (fs::create_directory(fs::path(std::u8string(path.begin(), path.end())), error)) {
      return std::make_shared<StagedOutput>(std::move(directory));
    }
    if (error && error != std::errc::file_exists) {
      throw Error(ErrorCode::Io, "Cannot create a staging directory for the CSV output");
    }
  }
  throw Error(ErrorCode::Io, "Cannot create a staging directory for the CSV output");
}

struct StagedWriter {
  // The SDK stream must be released before its staging directory.
  std::shared_ptr<StagedOutput> stage;
  huxerui::OutputStream output;
};

huxerui::OutputStream OpenStaged(const std::shared_ptr<StagedOutput>& stage) {
  const auto writer = std::make_shared<StagedWriter>(StagedWriter{stage, RequireIo(stage->file.OpenWrite())});
  return huxerui::OutputStream::FromWriter(
      [writer](std::span<const std::byte> bytes) { return writer->output.Write(bytes); },
      [writer] { return writer->output.Close(); });
}

AsyncOutputStream OpenStagedAsync(const std::shared_ptr<StagedOutput>& stage, std::string_view fault) {
  const auto writer = std::make_shared<StagedWriter>(StagedWriter{stage, RequireIo(stage->file.OpenWrite())});
  return AsyncOutputStream::FromWriter(
      [writer, fail = fault == "write"](std::stop_token, huxerui::Bytes bytes) -> Task<IoResult<void>> {
        auto result = co_await huxerui::RunWorker([writer, bytes = std::move(bytes)] { return writer->output.Write(bytes); });
        if (fail && result.Succeeded()) co_return IoResult<void>::Failure({huxerui::IoErrorCode::NoSpace, "Fixture write error"});
        co_return result;
      },
      [writer, fail = fault == "close"](std::stop_token) -> Task<IoResult<void>> {
        if (fail) co_return IoResult<void>::Failure({huxerui::IoErrorCode::Io, "Fixture close error"});
        co_return co_await huxerui::RunWorker([writer] { return writer->output.Close(); });
      });
}

Json DescribeSettingText(const SettingText& text) {
  if (const auto resource = std::get_if<huxerui::StringResource>(&text)) return {{"resource", resource->ToString()}};
  return std::get<std::string>(text);
}

Json DescribeSettingValue(const SettingValue& value) {
  return std::visit([](const auto& item) -> Json { return item; }, value);
}

Json DescribeSettingsSchema(const std::vector<SettingDefinition>& schema) {
  constexpr std::array<std::string_view, 4> types{"boolean", "integer", "string", "enum"};
  auto described = Json::array();
  for (const auto& setting : schema) {
    Json entry{{"key", setting.key}, {"type", types.at(static_cast<std::size_t>(setting.type))},
               {"title", DescribeSettingText(setting.title)}, {"default", DescribeSettingValue(setting.default_value)}};
    if (setting.description) entry["description"] = DescribeSettingText(*setting.description);
    if (setting.minimum) entry["minimum"] = *setting.minimum;
    if (setting.maximum) entry["maximum"] = *setting.maximum;
    if (!setting.options.empty()) {
      auto options = Json::array();
      for (const auto& choice : setting.options) {
        options.push_back({{"value", choice.value}, {"title", DescribeSettingText(choice.title)}});
      }
      entry["options"] = std::move(options);
    }
    described.push_back(std::move(entry));
  }
  return described;
}

Json DescribeSettings(const SettingsSnapshot& settings) {
  const auto values = [](const SettingValues& source) {
    auto result = Json::object();
    for (const auto& [key, value] : source) result[key] = DescribeSettingValue(value);
    return result;
  };
  return {{"schema", DescribeSettingsSchema(settings.schema)}, {"effective", values(settings.effective)},
          {"overrides", values(settings.overrides)}, {"revision", settings.revision},
          {"active_revision", settings.active_revision ? Json(*settings.active_revision) : Json(nullptr)}};
}

SettingValues ReadSettingEdits(const File& file) try {
  const auto overrides = Json::parse(RequireIo(file.ReadString()));
  if (!overrides.is_object()) throw Error(ErrorCode::InvalidArgument, "Setting overrides must be a JSON object");
  SettingValues result;
  for (const auto& [key, value] : overrides.items()) {
    if (value.is_boolean()) result.emplace(key, value.get<bool>());
    else if (value.is_string()) result.emplace(key, value.get<std::string>());
    else if (value.is_number_integer() && (!value.is_number_unsigned() ||
             value.get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))) {
      result.emplace(key, value.get<std::int64_t>());
    } else {
      throw Error(ErrorCode::InvalidArgument, "Expected a boolean, signed 64-bit integer, or string at " + key);
    }
  }
  return result;
} catch (const Json::exception& error) {
  throw Error(ErrorCode::InvalidArgument, "Cannot parse setting overrides: " + std::string(error.what()));
}

Json Describe(const PackageInfo& package) {
  auto extensions = Json::array();
  for (const auto& extension : package.extensions) {
    auto entry = Json{{"point", extension.Point()}};
    if (extension.metadata_json) entry["metadata"] = Json::parse(*extension.metadata_json);
    extensions.push_back(std::move(entry));
  }
  return {{"id", package.id}, {"version", package.version}, {"digest", package.digest},
          {"extensions", std::move(extensions)}, {"settings_schema", DescribeSettingsSchema(package.settings_schema)}};
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
      auto entry = Json{{"plugin", extension.PluginId()}};
      if (extension.metadata_json) entry["metadata"] = Json::parse(*extension.metadata_json);
      result.push_back(std::move(entry));
    }
    return result;
  };
  return {{"plugins", std::move(plugins)}, {"importers", describe(Importers)}, {"exporters", describe(Exporters)}};
}

char LowercaseAscii(char character) {
  if (character >= 'A' && character <= 'Z') return static_cast<char>(character - 'A' + 'a');
  return character;
}

bool MatchesSuffix(const ExtensionInfo& candidate, std::string_view suffix) {
  if (suffix.empty() || !candidate.metadata_json) return false;
  const auto metadata = Json::parse(*candidate.metadata_json);
  const auto extensions = metadata.find("extensions");
  if (extensions == metadata.end() || !extensions->is_array() || extensions->size() > 32) return false;

  // A malformed suffix list makes the candidate ineligible for file-based selection.
  bool matches = false;
  for (const auto& item : *extensions) {
    const auto* value = item.get_ptr<const std::string*>();
    if (!value || value->size() < 2 || !value->starts_with('.') ||
        value->find_first_of("/\\") != std::string::npos || value->find('\0') != std::string::npos) return false;
    if (std::ranges::equal(*value, suffix, {}, LowercaseAscii, LowercaseAscii)) matches = true;
  }
  return matches;
}

Task<Json> Import(PluginManager& manager, const std::vector<std::string>& args) {
  const File path(args[1]);
  const auto suffix = path.Extension();
  std::optional<ExtensionInfo> selected;
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (args.size() >= 3 && extension.PluginId() != args[2]) continue;
    if (!MatchesSuffix(extension, suffix)) continue;
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
  const auto info = RequireIo(path.Stat());
  if (info.size > csv_import::MaxImportBytes) {
    throw Error(ErrorCode::PayloadTooLarge, "CSV input exceeds the example limit");
  }
  auto input = RequireIo(co_await path.OpenReadAsync());
  auto importer = manager.CreateExtension(Importers, *selected, timeout);
  auto progress = std::make_shared<PreviewProgress>();
  const auto delimiter = importer->Delimiter();
  const auto plugin_pid = importer->ProcessId();
  auto imported = co_await importer->Import(std::move(input), progress);

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
    return entry.PluginId() == plugin && MatchesSuffix(entry, ".ssv");
  });
  if (candidate == candidates.end()) throw Error(ErrorCode::NotFound, "No semicolon importer for this package");
  auto exporter = manager.CreateExtension(Exporters, plugin);
  const auto pid = importers[0]->ProcessId();
  if (importers[1]->ProcessId() != pid || exporter->ProcessId() != pid) {
    throw Error(ErrorCode::PluginFailed, "Factories from the same package must share a process");
  }

  const auto parse = [](auto& importer, huxerui::AsyncInputStream input) -> Task<csv_import::Table> {
    auto result = co_await importer->Import(std::move(input), nullptr);
    if (!result.table) throw Error(ErrorCode::PluginFailed, "Cannot parse the round-trip input");
    co_return std::move(*result.table);
  };
  const auto table = co_await parse(importers[0], RequireIo(co_await File(args[1]).OpenReadAsync()));
  {
    auto semicolon = manager.CreateExtension(Importers, *candidate);
    if (semicolon->Delimiter() != ";" || semicolon->ProcessId() != pid) {
      throw Error(ErrorCode::PluginFailed, "The candidate did not select the semicolon importer");
    }
    const auto other = co_await parse(semicolon, RequireIo(co_await File(args[2]).OpenReadAsync()));
    if (table.header != other.header || table.rows != other.rows) {
      throw Error(ErrorCode::PluginFailed, "CSV and semicolon input did not produce the same table");
    }
  }
  const csv_import::ExportRequest request{table.header, table.rows};
  std::string exported;
  auto output = huxerui::AsyncOutputStream::FromWriter(
      [&exported](std::stop_token, huxerui::Bytes bytes) -> Task<huxerui::IoResult<void>> {
        exported.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        co_return huxerui::IoResult<void>::Success();
      });
  co_await exporter->Export(request, std::move(output));
  const auto round_trip = co_await parse(importers[0], StringInput(exported));
  if (table.header != round_trip.header || table.rows != round_trip.rows) {
    throw Error(ErrorCode::PluginFailed, "The exported CSV did not preserve the input table");
  }
  co_return Json{{"all_importers", all_importers}, {"package_importers", importers.size()},
                 {"format", exporter->Format()}, {"plugin_pid", pid}, {"csv", std::move(exported)}};
}

Task<Json> Convert(PluginManager& manager, const std::vector<std::string>& args) {
  const bool synchronous = args[1] == "sync";
  if (!synchronous && args[1] != "async") throw Error(ErrorCode::InvalidArgument, "Expected sync or async conversion");
  const File source(args[2]), destination(args[3]);
  auto exporter = manager.CreateExtension(Exporters, args[4], std::chrono::seconds(30));
  const auto fault = args.front() == "check-output-failure" ? std::string_view(args[5]) : std::string_view{};
  csv_import::ConversionOptions options;
  if (args.front() == "convert" && args.size() == 6) {
    std::uint32_t kib = 0;
    const auto& value = args[5];
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, kib);
    if (result.ec != std::errc{} || result.ptr != end || !kib || kib > HPK_MAX_STREAM_CHUNK / 1024) {
      throw Error(ErrorCode::InvalidArgument, "Expected a chunk size between 1 and 1024 KiB");
    }
    options.read_bytes = kib * 1024;
  }
  const auto pid = exporter->ProcessId();
  auto stage = StageOutput(destination);
  csv_import::CsvSummary summary{};
  auto progress = std::make_shared<ConversionProgress>();
  if (synchronous) {
    auto input = RequireIo(source.OpenRead());
    auto output = OpenStaged(stage);
    summary = exporter->Convert(options, std::move(input), std::move(output));
  } else {
    auto original = std::make_shared<AsyncInputStream>(RequireIo(co_await source.OpenReadAsync()));
    auto input = AsyncInputStream::FromReader(
        [original, progress](std::stop_token, std::size_t count) -> Task<IoResult<huxerui::Bytes>> {
          auto result = co_await original->ReadAsync(count);
          if (result.Succeeded()) progress->eof = result.Value().empty();
          co_return result;
        });
    auto output = OpenStagedAsync(stage, fault);
    summary = co_await exporter->ConvertAsync(options, std::move(input), std::move(output), progress);
  }
  if (!(co_await stage->file.MoveToAsync(destination, true))) {
    throw Error(ErrorCode::Io, "Cannot publish CSV output");
  }
  co_return Json{{"rows", summary.rows}, {"input_bytes", summary.input_bytes}, {"output_bytes", summary.output_bytes},
                 {"plugin_pid", pid}, {"progress_before_eof", progress->before_eof}, {"progress_rows", progress->rows}};
}

Task<Json> ConvertPair(PluginManager& manager, const std::vector<std::string>& args) {
  const bool synchronous = args[1] == "sync";
  if (!synchronous && args[1] != "async") throw Error(ErrorCode::InvalidArgument, "Expected sync or async conversion");
  const File first_source(args[2]), first_destination(args[3]), second_source(args[4]), second_destination(args[5]);
  const auto option = args.size() == 8 ? std::string_view(args[7]) : std::string_view{};
  auto exporter = manager.CreateExtension(Exporters, args[6], std::chrono::seconds(30));
  auto first_stage = StageOutput(first_destination);
  auto second_stage = StageOutput(second_destination);
  constexpr std::uint32_t read_bytes = 512 * 1024;
  auto first_progress = !synchronous && option != "omit-first" ? std::make_shared<ConversionProgress>() : nullptr;
  auto second_progress = !synchronous && option != "omit-second" ? std::make_shared<ConversionProgress>() : nullptr;
  const auto pid = exporter->ProcessId();
  std::optional<csv_import::PairSummary> summary;
  if (synchronous) {
    auto first = RequireIo(first_source.OpenRead());
    auto second = RequireIo(second_source.OpenRead());
    summary.emplace(exporter->ConvertPair(std::move(first), csv_import::Separator(";"), OpenStaged(first_stage),
        read_bytes, OpenStaged(second_stage), std::move(second)));
  } else {
    auto first = RequireIo(co_await first_source.OpenReadAsync());
    auto second = RequireIo(co_await second_source.OpenReadAsync());
    const auto fault = option == "write-second" ? std::string_view("write") : std::string_view{};
    summary.emplace(co_await exporter->ConvertPairAsync(
        std::move(first), csv_import::Separator(";"), first_progress, OpenStagedAsync(first_stage, {}), read_bytes,
        OpenStagedAsync(second_stage, fault), second_progress, std::move(second)));
  }
  // Both staged outputs have closed successfully before the host begins either publication.
  if (!(co_await first_stage->file.MoveToAsync(first_destination, true)) ||
      !(co_await second_stage->file.MoveToAsync(second_destination, true))) {
    throw Error(ErrorCode::Io, "Cannot publish CSV output");
  }
  const auto counts = [](const csv_import::CsvSummary& converted, const std::shared_ptr<ConversionProgress>& progress) {
    return Json{{"rows", converted.rows}, {"input_bytes", converted.input_bytes}, {"output_bytes", converted.output_bytes},
                {"progress_rows", progress ? Json(progress->rows) : Json(nullptr)},
                {"progress_bytes", progress ? Json(progress->input_bytes) : Json(nullptr)}};
  };
  co_return Json{{"first", counts(summary->first, first_progress)}, {"second", counts(summary->second, second_progress)},
                 {"separator", summary->separator.Text()}, {"plugin_pid", pid}};
}

struct HeldStreamOperation {
  template<class T>
  Task<IoResult<T>> Hold(std::stop_token token) {
    std::stop_callback on_stop(token, [this] { stop_requested = true; });
    co_return co_await AwaitCallback<IoResult<T>>([this](auto reply) {
      late = [reply] { reply.Fail(std::make_exception_ptr(std::runtime_error("Late stream completion"))); };
      started = true;
      return [this] { cancelled = true; };
    });
  }

  std::function<void()> late;
  bool started = false;
  bool cancelled = false;
  bool stop_requested = false;
};

Task<Json> CheckStreamInterruption(PluginManager& manager, std::string_view plugin, bool writing, bool deadline) {
  auto exporter = manager.CreateExtension(Exporters, plugin, deadline ? std::chrono::milliseconds(750) : DefaultTimeout);
  (void)exporter->ProcessId();
  auto gate = std::make_shared<HeldStreamOperation>();
  auto input = writing ? StringInput("name;city\nAlice;Paris\n") : AsyncInputStream::FromReader(
      [gate](std::stop_token stop, std::size_t) { return gate->Hold<huxerui::Bytes>(stop); });
  auto output = AsyncOutputStream::FromWriter(
      [gate, writing](std::stop_token stop, huxerui::Bytes) -> Task<IoResult<void>> {
        if (writing) co_return co_await gate->Hold<void>(stop);
        co_return IoResult<void>::Success();
      });
  auto first_output = AsyncOutputStream::FromWriter(
      [](std::stop_token, huxerui::Bytes) -> Task<IoResult<void>> { co_return IoResult<void>::Success(); });
  auto first_progress = std::make_shared<ConversionProgress>();
  auto second_progress = std::make_shared<ConversionProgress>();
  const std::weak_ptr<ConversionProgress> first_callback = first_progress;
  const std::weak_ptr<ConversionProgress> second_callback = second_progress;
  auto outcome = std::make_shared<CallOutcome>();
  const auto handle = UseApplicationTaskScope().Launch(ObserveCall(
      exporter->ConvertPairAsync(StringInput("x;y\n1;2\n"), csv_import::Separator(";"), std::move(first_progress),
          std::move(first_output), 512 * 1024, std::move(output), std::move(second_progress), std::move(input)), outcome));
  co_await WaitFor([gate] { return gate->started; });
  if (!deadline) handle.Cancel();
  co_await WaitFor([&] { return gate->cancelled && first_callback.expired() && second_callback.expired(); });
  gate->late();
  if (deadline) {
    co_await WaitFor([outcome] { return outcome->done; });
    if (!outcome->error) throw Error(ErrorCode::PluginFailed, "The invocation ignored its deadline");
    try {
      std::rethrow_exception(outcome->error);
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::DeadlineExceeded) throw;
    }
  } else if (outcome->done) {
    throw Error(ErrorCode::PluginFailed, "A cancelled stream invocation delivered a result");
  }
  co_return Json{{"cancelled", gate->cancelled}, {"stop_requested", gate->stop_requested}, {"deadline", deadline}};
}

Task<Json> CheckLocalStreamReentry(PluginManager& manager, std::string_view plugin, bool synchronous) {
  auto exporter = manager.CreateExtension(Exporters, plugin);
  if (exporter->ProcessId() != ProcessId()) throw Error(ErrorCode::InvalidArgument, "Expected an in-process exporter");
  constexpr std::string_view sample = "name;city\nAlice;Paris\n";
  struct Checked {
    bool read = false;
    bool write = false;
    bool close = false;
  };
  auto checked = std::make_shared<Checked>();
  if (synchronous) {
    const auto check = [&exporter] {
      try {
        (void)exporter->ProcessId();
      } catch (const Error& error) {
        if (error.Code() == ErrorCode::ReentrantCall) return;
        throw;
      }
      throw Error(ErrorCode::PluginFailed, "The synchronous stream callback reentered PluginKit");
    };
    auto input = huxerui::InputStream::FromReader(
        [check, checked, source = sample](std::span<std::byte> buffer) mutable {
          check();
          checked->read = true;
          const auto count = std::min(source.size(), buffer.size());
          if (count) std::memcpy(buffer.data(), source.data(), count);
          source.remove_prefix(count);
          return IoResult<std::size_t>(count);
        });
    auto output = huxerui::OutputStream::FromWriter(
        [check, checked](std::span<const std::byte>) {
          check();
          checked->write = true;
          return IoResult<void>::Success();
        },
        [check, checked] {
          check();
          checked->close = true;
          return IoResult<void>::Success();
        });
    (void)exporter->Convert({}, std::move(input), std::move(output));
  } else {
    const auto check = [&manager, id = std::string(plugin)]() -> Task<void> {
      co_await Delay(std::chrono::milliseconds(1));
      try {
        co_await manager.SetEnabled(id, true);
      } catch (const Error& error) {
        if (error.Code() == ErrorCode::ReentrantCall) co_return;
        throw;
      }
      throw Error(ErrorCode::PluginFailed, "The asynchronous stream callback reentered PluginKit");
    };
    auto input = AsyncInputStream::FromReader(
        [check, checked, source = std::make_shared<AsyncInputStream>(StringInput(std::string(sample)))]
        (std::stop_token, std::size_t count) -> Task<IoResult<huxerui::Bytes>> {
          co_await check();
          checked->read = true;
          co_return co_await source->ReadAsync(count);
        });
    auto output = AsyncOutputStream::FromWriter(
        [check, checked](std::stop_token, huxerui::Bytes) -> Task<IoResult<void>> {
          co_await check();
          checked->write = true;
          co_return IoResult<void>::Success();
        },
        [check, checked](std::stop_token) -> Task<IoResult<void>> {
          co_await check();
          checked->close = true;
          co_return IoResult<void>::Success();
        });
    (void)co_await exporter->ConvertAsync({}, std::move(input), std::move(output), nullptr);
  }
  co_return Json{{"read", checked->read}, {"write", checked->write}, {"close", checked->close}};
}

void TerminateFixtureWorker(std::uint32_t pid) {
#if defined(_WIN32)
  const auto process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (process) {
    const auto terminated = ::TerminateProcess(process, 1);
    ::CloseHandle(process);
    if (terminated) return;
  }
#else
  if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0) return;
#endif
  throw Error(ErrorCode::PluginFailed, "Cannot terminate the CSV fixture worker");
}

Task<void> CheckWorkerExitDuringCallback(PluginManager& manager, std::string_view plugin, bool during_receiver) {
  // Leave a longer invocation deadline than the fixture's wait for a worker-exit response.
  auto exporter = manager.CreateExtension(Exporters, plugin, DefaultTimeout * 3);
  const auto pid = exporter->ProcessId();
  const auto plugins = manager.ListPlugins();
  if (!pid || pid == ProcessId() || !std::ranges::any_of(plugins, [&](const auto& entry) {
        return entry.package.id == plugin && entry.worker_pid == pid;
      })) {
    throw Error(ErrorCode::InvalidArgument, "Expected a worker exporter");
  }
  auto stream = std::make_shared<HeldStreamOperation>();
  auto progress = std::make_shared<ProgressGate>();
  auto receiver = std::make_shared<HeldProgress>(progress);
  const std::weak_ptr<HeldProgress> receiver_lifetime = receiver;
  auto input = during_receiver ? StringInput("name;city\nAlice;Paris\n") : AsyncInputStream::FromReader(
      [stream](std::stop_token stop, std::size_t) { return stream->Hold<huxerui::Bytes>(stop); });
  auto written = std::make_shared<std::size_t>(0);
  const std::weak_ptr<std::size_t> output_lifetime = written;
  const auto make_output = [&written] {
    return AsyncOutputStream::FromWriter(
        [written](std::stop_token, huxerui::Bytes bytes) -> Task<IoResult<void>> {
          *written += bytes.size();
          co_return IoResult<void>::Success();
        });
  };
  auto first_output = make_output();
  auto output = make_output();
  written.reset();
  auto first_progress = std::make_shared<ConversionProgress>();
  const std::weak_ptr<ConversionProgress> first_lifetime = first_progress;
  const auto outcome = std::make_shared<CallOutcome>();
  (void)UseApplicationTaskScope().Launch(ObserveCall(
      exporter->ConvertPairAsync(StringInput("x;y\n1;2\n"), csv_import::Separator(";"), std::move(first_progress),
          std::move(first_output), 512 * 1024, std::move(output), std::move(receiver), std::move(input)), outcome));
  co_await WaitFor([&] { return during_receiver ? progress->started : stream->started; });
  TerminateFixtureWorker(pid);
  co_await WaitFor([&] {
    const bool cancelled = during_receiver ? progress->cancelled : stream->cancelled && stream->stop_requested;
    return cancelled && outcome->done && first_lifetime.expired() && receiver_lifetime.expired() && output_lifetime.expired();
  });
  if (during_receiver) progress->complete->Complete(9);
  else stream->late();
  if (!outcome->error) throw Error(ErrorCode::PluginFailed, "The exited worker returned a conversion result");
  try {
    std::rethrow_exception(outcome->error);
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::WorkerExited) throw;
  }
}

Task<Json> CheckWorkerEvents(PluginManager& manager, const std::vector<std::string>& args) {
  const auto& plugin = args[1];
  const bool idle = args[2] == "idle";
  auto other = manager.CreateExtension(Exporters, args[3]);
  const auto other_pid = other->ProcessId();
  std::vector<PluginEvent> events;
  std::optional<PluginDiagnostics> faulted;
  std::optional<ExtensionHandle<csv_import::TableExporter>> recovered;
  std::exception_ptr callback_error;
  auto subscription = manager.SubscribeEvents([&](const PluginEvent& event) {
    if (event.package.id != plugin) return;
    events.push_back(event);
    if (event.kind != PluginEventKind::WorkerExited) return;
    try {
      faulted = manager.Diagnostics(plugin);
      const auto catalog = manager.ListPlugins();
      const auto listed = std::ranges::find_if(catalog, [&](const auto& entry) { return entry.package.id == plugin; });
      if (listed == catalog.end() || listed->worker_pid) {
        throw Error(ErrorCode::PluginFailed, "The exited worker is still listed");
      }
      recovered.emplace(manager.CreateExtension(Exporters, plugin));
      (void)(*recovered)->ProcessId();
    } catch (...) {
      callback_error = std::current_exception();
    }
  });
  auto exporter = manager.CreateExtension(Exporters, plugin);
  const auto pid = exporter->ProcessId();
  if (!pid || pid == ProcessId()) throw Error(ErrorCode::InvalidArgument, "Expected a CSV worker");
  co_await WaitFor([&] { return !events.empty(); });

  if (idle) TerminateFixtureWorker(pid);
  else co_await CheckWorkerExitDuringCallback(manager, plugin, args[2] == "receiver");
  co_await WaitFor([&] { return events.size() >= 2; });
  if (callback_error) std::rethrow_exception(callback_error);
  if (events[0].kind != PluginEventKind::Activated ||
      !events[0].generation || events[1].kind != PluginEventKind::WorkerExited ||
      events[1].generation != events[0].generation || events[1].worker_pid != pid ||
      !events[1].failure || events[1].failure->code != ErrorCode::WorkerExited ||
      (!events[1].failure->exit_code && !events[1].failure->signal) ||
      !faulted || !faulted->plugin.enabled || faulted->plugin.worker_pid ||
      faulted->state != PluginRuntimeState::Faulted || !faulted->last_failure ||
      faulted->last_failure->sequence != events[1].sequence || other->ProcessId() != other_pid) {
    throw Error(ErrorCode::PluginFailed, "Unexpected worker exit diagnostics");
  }
  auto output = AsyncOutputStream::FromWriter([](std::stop_token, huxerui::Bytes) -> Task<IoResult<void>> {
    co_return IoResult<void>::Success();
  });
  const auto summary = co_await (*recovered)->ConvertAsync({}, StringInput("name;city\nAlice;Paris\n"),
                                                           std::move(output), nullptr);
  co_await WaitFor([&] { return events.size() >= 3; });
  const auto active = manager.Diagnostics(plugin);
  if (summary.rows != 1 || events.size() != 3 || events[2].kind != PluginEventKind::Activated ||
      events[2].generation <= events[0].generation || !active || active->state != PluginRuntimeState::Active ||
      active->generation != events[2].generation || !active->last_failure ||
      active->last_failure->sequence != events[1].sequence) {
    throw Error(ErrorCode::PluginFailed, "Worker recovery did not preserve the last failure");
  }
  co_return Json{{"recovered", true}};
}

Task<Json> CheckCatalogEvents(PluginManager& manager, const std::string& plugin) {
  std::vector<PluginEvent> events;
  bool valid = true;
  const auto thread = std::this_thread::get_id();
  auto throwing = manager.SubscribeEvents([](const PluginEvent&) { throw std::runtime_error("Observer fixture"); });
  auto subscription = manager.SubscribeEvents([&](const PluginEvent& event) {
    if (event.package.id != plugin) return;
    valid = valid && std::this_thread::get_id() == thread &&
        (events.empty() || event.sequence > events.back().sequence);
    events.push_back(event);
    if (event.change == PluginCatalogChange::Disabled) {
      const auto diagnostic = manager.Diagnostics(plugin);
      const auto candidates = manager.ListExtensions(Exporters);
      valid = valid && diagnostic && !diagnostic->plugin.enabled &&
          std::ranges::find(candidates, plugin, &ExtensionInfo::PluginId) == candidates.end();
    }
  });
  PluginEventSubscription once;
  unsigned once_calls = 0;
  once = manager.SubscribeEvents([&](const PluginEvent&) {
    ++once_calls;
    once = {};
  });
  unsigned released_calls = 0;
  auto exporter = manager.CreateExtension(Exporters, plugin);
  {
    auto released = manager.SubscribeEvents([&](const PluginEvent&) { ++released_calls; });
    (void)exporter->ProcessId();
  }
  unsigned later_calls = 0;
  auto later = manager.SubscribeEvents([&](const PluginEvent&) { ++later_calls; });
  if (!events.empty()) throw Error(ErrorCode::PluginFailed, "A plugin event was delivered inline");
  co_await WaitFor([&] { return !events.empty(); });
  if (later_calls) throw Error(ErrorCode::PluginFailed, "An event was replayed to a new subscriber");
  co_await manager.SetEnabled(plugin, false);
  co_await manager.SetEnabled(plugin, false);
  co_await manager.SetEnabled(plugin, true);
  co_await WaitFor([&] { return !events.empty() && events.back().change == PluginCatalogChange::Enabled; });
  if (!valid || once_calls != 1 || released_calls || later_calls != 3 || events.size() != 4 ||
      events[0].kind != PluginEventKind::Activated || events[1].change != PluginCatalogChange::Disabled ||
      events[2].kind != PluginEventKind::Stopped || events[2].stop_reason != PluginStopReason::Disabled ||
      events[2].generation != events[0].generation || events[2].failure) {
    throw Error(ErrorCode::PluginFailed, "Unexpected catalog events or subscription lifetime");
  }
  co_return Json{{"catalog_events", true}};
}

Task<Json> CheckStaleStream(PluginManager& manager, std::string_view plugin) {
  auto exporter = manager.CreateExtension(Exporters, plugin);
  int reads = 0;
  const auto source = [&reads] {
    return AsyncInputStream::FromReader([&reads](std::stop_token, std::size_t) -> Task<IoResult<huxerui::Bytes>> {
      ++reads;
      co_return IoResult<huxerui::Bytes>(huxerui::Bytes{std::byte{'x'}});
    });
  };
  const auto sink = [] {
    return AsyncOutputStream::FromWriter([](std::stop_token, huxerui::Bytes) -> Task<IoResult<void>> {
      co_return IoResult<void>::Success();
    });
  };
  (void)co_await exporter->ConvertAsync({}, source(), sink(), nullptr);
  try {
    (void)co_await exporter->ConvertAsync({}, source(), sink(), nullptr);
    throw Error(ErrorCode::PluginFailed, "The stale stream was not rejected");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::Closed) throw;
  }
  co_return Json{{"old_source_reads", reads}};
}

Task<Json> CheckSynchronousStreamDeadline(PluginManager& manager, std::string_view plugin, bool writing) {
  using namespace std::chrono_literals;
  auto exporter = manager.CreateExtension(Exporters, plugin, 400ms);
  (void)exporter->ProcessId();
  struct Pending {
    std::mutex mutex;
    std::condition_variable release;
    bool started = false;
    bool finished = false;
  };
  auto pending = std::make_shared<Pending>();
  const auto hold = [pending] {
    std::unique_lock lock(pending->mutex);
    pending->started = true;
    pending->release.wait_for(lock, 3s, [&] { return pending->finished; });
  };
  auto completed = std::make_shared<std::atomic_bool>(false);
  auto input = huxerui::InputStream::FromReader(
      [hold, writing, completed, sent = false](std::span<std::byte> bytes) mutable -> IoResult<std::size_t> {
        if (!writing) {
          hold();
          *completed = true;
          return IoResult<std::size_t>(0);
        }
        if (std::exchange(sent, true)) return IoResult<std::size_t>(0);
        constexpr std::string_view source = "name;city\nAlice;Paris\n";
        std::memcpy(bytes.data(), source.data(), source.size());
        return IoResult<std::size_t>(source.size());
      });
  auto output = huxerui::OutputStream::FromWriter([hold, writing, completed](std::span<const std::byte>) {
    if (writing) {
      hold();
      *completed = true;
    }
    return IoResult<void>::Success();
  });
  const auto error = [&]() -> std::optional<ErrorCode> {
    try {
      (void)exporter->Convert({}, std::move(input), std::move(output));
      return std::nullopt;
    } catch (const Error& failure) {
      return failure.Code();
    }
  }();
  bool still_running;
  {
    std::lock_guard lock(pending->mutex);
    still_running = pending->started && !completed->load();
    pending->finished = true;
  }
  pending->release.notify_one();
  if (error != ErrorCode::DeadlineExceeded || !still_running) {
    throw Error(ErrorCode::PluginFailed, "The synchronous stream did not time out independently of its I/O");
  }
  co_await WaitFor([completed] { return completed->load(); });
  co_return Json{{"deadline_while_io_pending", true}};
}

Task<Json> CheckReceiverLifetime(PluginManager& manager, std::string plugin) {
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (extension.PluginId() != plugin) continue;
    const auto tasks = UseApplicationTaskScope();
    auto importer = manager.CreateExtension(Importers, extension);
    auto first = std::make_shared<PreviewProgress>();
    auto second = std::make_shared<PreviewProgress>();
    (void)co_await importer->Import(StringInput("remember"), first);
    try {
      (void)co_await importer->Import(StringInput("stale"), second);
      throw Error(ErrorCode::PluginFailed, "An expired receiver was accepted");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::ReceiverClosed) throw;
    }
    auto held = std::make_shared<ProgressGate>();
    auto running = std::make_shared<CallOutcome>();
    (void)tasks.Launch(ObserveCall(importer->Import(StringInput("remember"), std::make_shared<HeldProgress>(held)), running));
    co_await WaitFor([held] { return held->started; });
    // The receiver is still suspended when this independent task reads the catalog.
    auto catalog_during_callback = List(manager);
    auto queued = std::make_shared<CallOutcome>();
    const auto queued_task = tasks.Launch(ObserveCall(importer->Import(StringInput("remember"), second), queued));
    co_await Delay(std::chrono::milliseconds(20));
    queued_task.Cancel();
    try {
      auto short_call = manager.CreateExtension(Importers, extension, std::chrono::milliseconds(30));
      (void)co_await short_call->Import(StringInput("remember"), second);
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
      return unused->Import(StringInput("remember"), second);
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
    auto outcome = std::make_shared<CallOutcome>();
    const auto task = tasks.Launch(ObserveCall(importer->Import(StringInput("remember"), std::move(receiver)), outcome));
    co_await WaitFor([cancelled] { return cancelled->started; });
    task.Cancel();
    co_await WaitFor([&] { return cancelled->cancelled && receiver_lifetime.expired(); });
    cancelled->complete->Complete(9);
    if (outcome->done) throw Error(ErrorCode::PluginFailed, "A cancelled invocation resumed");

    auto restarted = manager.CreateExtension(Importers, extension, DefaultTimeout * 2);
    auto disabled = std::make_shared<ProgressGate>();
    auto interrupted = std::make_shared<CallOutcome>();
    (void)tasks.Launch(ObserveCall(
        restarted->Import(StringInput("remember"), std::make_shared<HeldProgress>(disabled)), interrupted));
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
    if (extension.PluginId() != plugin) continue;
    auto second = manager.CreateExtension(Importers, extension);
    const auto context = [](auto& importer) -> Task<std::vector<std::string>> {
      auto result = co_await importer->Import(StringInput("context"), std::make_shared<PreviewProgress>());
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
      auto pending = second->Import(StringInput("context"), std::make_shared<PreviewProgress>());
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
  const auto candidate = std::ranges::find_if(info.extensions, [](const ExtensionInfo& extension) {
    return extension.Point() == Importers.id && MatchesSuffix(extension, ".ssv");
  });
  if (candidate == info.extensions.end()) throw Error(ErrorCode::NotFound, "No semicolon importer in the package");
  try {
    (void)manager->CreateExtension(Importers, *candidate);
    throw Error(ErrorCode::PluginFailed, "Created an extension before its package was installed");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::NotFound) throw;
  }
  try {
    manager->InstallPackage(package, info.digest);
    throw Error(ErrorCode::PluginFailed, "The in-process-only host installed a worker package");
  } catch (const Error& error) {
    if (error.Code() != ErrorCode::UnsupportedMode) throw;
  }
  manager->InstallPackage(package, info.digest, {.mode = ExecutionMode::InProcess, .services = {}});

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
    tasks.Post([tasks, manager, outcome, candidate = *candidate] {
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
  auto importer = manager->CreateExtension(Importers, *candidate);
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

Task<Json> CheckUpgradeFailureEvents(PluginManager& manager, std::string plugin,
                                     const std::vector<std::string>* command = nullptr) {
  std::vector<PluginEvent> events;
  auto subscription = RecordPluginEvents(manager, plugin, events);
  if (command) {
    try {
      (void)co_await Upgrade(manager, *command);
      throw std::runtime_error("The CSV upgrade unexpectedly succeeded");
    } catch (const Error& error) {
      if (error.Code() != ErrorCode::PluginFailed) throw;
    }
  } else {
    const auto changes = co_await manager.ApplyPendingChanges();
    if (changes.size() != 1 || changes[0].plugin_id != plugin || changes[0].error != ErrorCode::PluginFailed ||
        !changes[0].restart_required) {
      throw Error(ErrorCode::PluginFailed, "Expected an unsuccessful pending CSV upgrade");
    }
  }
  co_await WaitFor([&] { return !events.empty(); });
  const auto diagnostics = manager.Diagnostics(plugin);
  if (events.size() != 1 || events[0].kind != PluginEventKind::UpgradeFailed ||
      !diagnostics || !diagnostics->last_failure || diagnostics->last_failure->sequence != events[0].sequence ||
      diagnostics->plugin.pending_upgrade || !events[0].failure || events[0].failure->code != ErrorCode::PluginFailed) {
    throw Error(ErrorCode::PluginFailed, "Unexpected upgrade failure diagnostics");
  }
  if (!command && (std::ranges::count(manager.ListExtensions(Importers), plugin, &ExtensionInfo::PluginId) ||
                   std::ranges::count(manager.ListExtensions(Exporters), plugin, &ExtensionInfo::PluginId))) {
    throw Error(ErrorCode::PluginFailed, "A failed in-process replacement still exposes factories before restart");
  }
  co_return Json{{"installed", diagnostics->plugin.package.digest}, {"candidate", events[0].package.digest},
                 {"failed_at_activation", events[0].failure->stage == PluginFailureStage::Activate},
                 {"failed_at_factory", events[0].failure->stage == PluginFailureStage::ValidateFactories},
                 {"restart_required", diagnostics->restart_required}};
}

Task<Json> CheckWorkerUpgrade(PluginManager& manager, const std::vector<std::string>& args) {
  const auto package = PluginManager::InspectPackage(File(args[1]));
  const auto& id = package.id;
  const auto exporters = manager.ListExtensions(Exporters);
  const auto candidate = std::ranges::find(exporters, id, &ExtensionInfo::PluginId);
  const auto replacement = std::ranges::find(package.extensions, Exporters.id, &ExtensionInfo::Point);
  if (candidate == exporters.end() || replacement == package.extensions.end()) {
    throw Error(ErrorCode::NotFound, "The upgrade fixture has no exporter");
  }
  auto previous = manager.CreateExtension(Importers, id);
  auto previous_exporter = manager.CreateExtension(Exporters, *candidate);
  auto pending = manager.CreateExtension(Importers, id);
  const auto old_pid = previous->ProcessId();
  if (previous_exporter->ProcessId() != old_pid) throw Error(ErrorCode::PluginFailed, "Expected one worker per package");
  std::vector<PluginEvent> promoted;
  bool listed = false;
  auto subscription = manager.SubscribeEvents([&](const PluginEvent& event) {
    if (event.package.digest != package.digest) return;
    promoted.push_back(event);
    if (event.change == PluginCatalogChange::Upgraded) {
      listed = std::ranges::count(manager.ListExtensions(Exporters), id, &ExtensionInfo::PluginId) == 1;
    }
  });
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
  auto exporter = manager.CreateExtension(Exporters, *replacement);
  const auto new_pid = next->ProcessId();
  if (exporter->ProcessId() != new_pid) throw Error(ErrorCode::PluginFailed, "Expected one replacement worker");
  co_await WaitFor([&] { return promoted.size() >= 2; });
  if (!listed || promoted.size() != 2 || promoted[0].change != PluginCatalogChange::Upgraded ||
      promoted[1].kind != PluginEventKind::Activated || !promoted[1].generation || promoted[1].worker_pid != new_pid) {
    throw Error(ErrorCode::PluginFailed, "The replacement was announced before it became available");
  }
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
    std::vector<PluginEvent> events;
    auto subscription = RecordPluginEvents(manager, id, events);
    if (co_await Upgrade(manager, args) != UpgradeStatus::RestartRequired) {
      throw Error(ErrorCode::PluginFailed, "The in-process upgrade applied without a restart");
    }
    co_await WaitFor([&] { return !events.empty(); });
    if (events.size() != 1 || events[0].change != PluginCatalogChange::UpgradePending ||
        events[0].package.digest != package.digest) {
      throw Error(ErrorCode::PluginFailed, "Unexpected pending upgrade notification");
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
  const auto context = co_await handle->Import(StringInput("context"), nullptr);
  if (!context.table || context.table->rows.at(0).at(2) != args[2]) {
    throw Error(ErrorCode::PluginFailed, "The reopened manager selected the replacement too early");
  }
  co_await reopened.Shutdown();
  co_return Json{{"result", "restart_required"}, {"current_digest", args[2]}, {"pending_digest", package.digest}};
}

Task<Json> CheckSettingsActivation(PluginManager& manager, const std::string& plugin) {
  const auto original = manager.GetSettings(plugin);
  auto importers = manager.CreateExtensions(Importers, plugin);
  if (original.active_revision || importers.size() != 2) {
    throw Error(ErrorCode::PluginFailed, "Expected a fresh activation and two CSV importers");
  }
  std::optional<std::string> notified;
  const auto subscription = manager.SubscribeEvents([&](const PluginEvent& event) {
    if (event.package.id == plugin && event.change == PluginCatalogChange::SettingsChanged) {
      notified = manager.GetSettings(plugin).revision;
    }
  });
  auto overrides = original.overrides;
  overrides.insert_or_assign("max_rows", std::int64_t{1});
  co_await manager.SetSettingsAsync(plugin, original.revision, std::move(overrides));
  const auto saved = manager.GetSettings(plugin);
  if (saved.active_revision || saved.revision == original.revision) {
    throw Error(ErrorCode::PluginFailed, "Saving an override must commit without activating the plugin");
  }
  for (auto& importer : importers) {
    const auto result = co_await importer->Import(StringInput("heading\nfirst\nsecond\n"), nullptr);
    if (result.table || result.issues.size() != 1 || result.issues[0].code != "csv.row_limit") {
      throw Error(ErrorCode::PluginFailed, "The CSV importer did not use the saved row limit");
    }
  }
  const auto activated = manager.GetSettings(plugin);
  co_await WaitFor([&] { return notified.has_value(); });
  if (activated.active_revision != saved.revision || notified != saved.revision) {
    throw Error(ErrorCode::PluginFailed, "The activation and settings event must observe the committed revision");
  }
  co_return DescribeSettings(activated);
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
  if (name == "settings" && (args.size() == 2 || args.size() == 3)) {
    const auto locale = args.size() == 3 ? huxerui::Locale::FromLanguageTag(args[2]) : huxerui::Locale::Default();
    co_return DescribeSettings(manager.GetSettings(args[1], locale));
  }
  if (name == "set-settings" && args.size() == 4) {
    co_await manager.SetSettingsAsync(args[1], args[2], ReadSettingEdits(File(args[3])));
    co_return DescribeSettings(manager.GetSettings(args[1]));
  }
  if (name == "check-settings-activation" && args.size() == 2) {
    co_return co_await CheckSettingsActivation(manager, args[1]);
  }
  if (name == "check-contract-codecs" && args.size() == 1) {
    CheckContractCodecs();
    co_return Json::object();
  }
  if (name == "list" && args.size() == 1) co_return List(manager);
  if (name == "import" && args.size() >= 2 && args.size() <= 4) co_return co_await Import(manager, args);
  if (name == "round-trip" && args.size() == 4) co_return co_await RoundTrip(manager, args);
  if ((name == "convert" && (args.size() == 5 || args.size() == 6)) ||
      (name == "check-output-failure" && args.size() == 6)) {
    co_return co_await Convert(manager, args);
  }
  if (name == "convert-pair" && (args.size() == 7 || args.size() == 8)) co_return co_await ConvertPair(manager, args);
  if (name == "check-stream-interruption" && args.size() == 4) {
    co_return co_await CheckStreamInterruption(manager, args[1], args[2] == "write", args[3] == "deadline");
  }
  if (name == "check-local-stream-reentry" && args.size() == 3) {
    co_return co_await CheckLocalStreamReentry(manager, args[1], args[2] == "sync");
  }
  if (name == "check-worker-events" && args.size() == 4) {
    co_return co_await CheckWorkerEvents(manager, args);
  }
  if (name == "check-catalog-events" && args.size() == 2) {
    co_return co_await CheckCatalogEvents(manager, args[1]);
  }
  if (name == "check-upgrade-failure-events" && args.size() == 4) {
    co_return co_await CheckUpgradeFailureEvents(manager, PluginManager::InspectPackage(File(args[1])).id, &args);
  }
  if (name == "check-clean-diagnostics" && args.size() == 2) {
    const auto diagnostic = manager.Diagnostics(args[1]);
    co_return Json{{"installed", diagnostic.has_value()}, {"clean", !diagnostic || !diagnostic->last_failure}};
  }
  if (name == "check-stale-stream" && args.size() == 2) co_return co_await CheckStaleStream(manager, args[1]);
  if (name == "check-sync-stream-deadline" && args.size() == 3) {
    co_return co_await CheckSynchronousStreamDeadline(manager, args[1], args[2] == "write");
  }
  throw Error(ErrorCode::InvalidArgument, CommandHelp);
}

void Print(const Json& response) {
  std::cout << response.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
}

void PrintError(const std::exception& error) {
  const auto* kit_error = dynamic_cast<const Error*>(&error);
  Json response{{"ok", false}, {"error", kit_error ? ErrorName(kit_error->Code()) : "io"}, {"message", error.what()}};
  if (const auto* validation = dynamic_cast<const SettingsValidationError*>(&error)) {
    constexpr std::array<std::string_view, 5> reasons{
        "unknown_key", "type_mismatch", "out_of_range", "unknown_option", "invalid_text"};
    auto fields = Json::array();
    for (const auto& issue : validation->Issues()) {
      fields.push_back({{"key", issue.key}, {"reason", reasons.at(static_cast<std::size_t>(issue.kind))}});
    }
    response["fields"] = std::move(fields);
  }
  Print(response);
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
    if (args.size() == 3 && args[1] == "check-factory-contract") {
      manager.RegisterExtensionPoint(Importers);
      // Simulate a host SDK that binds the exporter point to another contract after a restart.
      manager.RegisterExtensionPoint(ExtensionPoint<csv_import::TableImporter>{Exporters.id});
      auto importers = manager.CreateExtensions(Importers, Utf8(args[2]));
      if (importers.empty()) throw Error(ErrorCode::NotFound, "The contract check needs an installed importer");
      const auto delimiter = importers.front()->Delimiter();
      co_await manager.Shutdown();
      Print({{"ok", true}, {"delimiter", delimiter}});
      runtime.RequestShutdown();
      co_return;
    }
    auto status = std::make_shared<PreviewImportStatus>(manager);
    manager.ProvideService<csv_import::ImportStatus>(status);
    RegisterPoints(manager);
    if (args.size() == 3 && args[1] == "check-pending-upgrade-events") {
      auto result = co_await CheckUpgradeFailureEvents(manager, Utf8(args[2]));
      result["ok"] = true;
      Print(result);
      co_await manager.Shutdown();
      runtime.RequestShutdown();
      co_return;
    }
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
