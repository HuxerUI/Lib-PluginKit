#include "csv_contract.bindings.h"
#include "csv_package.h"
#include "csv_sdk.h"
#include <huxerui/app.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <thread>

namespace {

using csv_import::fixture::active_context;
using csv_import::fixture::Record;

std::uint32_t created_instances = 0;
std::uint32_t live_instances = 0;
std::optional<huxerui::RawAsset> cell_limit;

void OnActivate(const huxerui::pluginkit::PluginContext& context) {
  if (active_context || !context.BundleDirectory().Child("plugin.json").ReadString().Succeeded() ||
      context.DataDirectory().Child("fail-activation").Exists()) {
    throw std::runtime_error("Cannot activate the fixture");
  }
  auto limit = context.Resources().GetRawResource(csv_resources::raw::cell_limit_txt);
  (void)csv_import::fixture::ReadCellLimit(limit);
#if defined(CSV_IMPORT_UPGRADE)
  const auto& data = context.DataDirectory();
  const auto format = data.Child("data-format");
  const auto version = format.ReadString();
  if ((!version.Succeeded() || version.Value() != "2") && !format.WriteString("2")) {
    throw std::runtime_error("Cannot migrate CSV fixture data");
  }
  if (data.Child("fail-upgrade").Exists()) throw std::runtime_error("CSV candidate failed after migrating data");
  if (data.Child("pause-upgrade").Exists()) {
    if (!data.Child("upgrade-ready").WriteString("ready")) throw std::runtime_error("Cannot mark CSV candidate ready");
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
  if (data.Child("exit-upgrade").Exists()) std::_Exit(7);
#endif
  Record(context, "activate");
  cell_limit.emplace(std::move(limit));
  active_context = &context;
}

void OnDeactivate(const huxerui::pluginkit::PluginContext& context) {
  if (active_context != &context || live_instances != 0 || csv_import::fixture::live_exporters != 0) {
    throw std::runtime_error("Expected the original context after all fixture instances were destroyed");
  }
  (void)csv_import::fixture::ReadCellLimit(*cell_limit);
  Record(context, "deactivate");
  cell_limit.reset();
  active_context = nullptr;
}

class FaultImporter final : public csv_import::TableImporter {
public:
  explicit FaultImporter(const huxerui::pluginkit::PluginContext& context) : context_(context) {
    if (active_context != &context) throw std::runtime_error("Expected the activation's context");
    ++created_instances;
    ++live_instances;
  }

  ~FaultImporter() override { --live_instances; }

  std::string Delimiter() const override {
    if (context_.DataDirectory().Child("sync-service").Exists()) {
      (void)huxerui::pluginkit::GetService<csv_import::ImportStatus>()->ImportedRows();
    }
    return ",";
  }
  std::uint32_t ProcessId() const override { return csv_import::ProcessId(); }

  huxerui::Task<csv_import::ImportResult> Import(csv_import::ImportRequest request,
                                                  csv_import::ImportReceiver receiver) override {
    if (request.source == "exit") std::_Exit(5);
    if (request.source == "context") {
      const auto& info = context_.Info();
      co_return csv_import::ImportResult{csv_import::Table{
          {"id", "version", "digest", "bundle", "data", "created", "live", "cell_limit"},
          {{info.id, info.version, info.digest, context_.BundleDirectory().Path(), context_.DataDirectory().Path(),
            std::to_string(created_instances), std::to_string(live_instances),
            std::to_string(csv_import::fixture::ReadCellLimit(*cell_limit))}}}, {}};
    }
    if (request.source == "status") {
      co_await SetRows(40);
      co_await SetRows(41);
    }
    if (request.source == "reenter") co_await SetRows(UINT32_MAX);
    if (request.source == "background") {
      std::async(std::launch::async, [] {
        return huxerui::pluginkit::GetService<csv_import::ImportStatus>()->ImportedRows();
      }).get();
    }
    if (request.source == "other-task") {
      co_await huxerui::AwaitCallback<void>([](auto completion) {
        (void)huxerui::UseApplicationTaskScope().Launch([completion]() -> huxerui::Task<void> {
          try {
            co_await SetRows(99);
            completion.Fail(std::make_exception_ptr(std::runtime_error("Unrelated task reached the host")));
          } catch (...) {
            completion.Fail(std::current_exception());
          }
        });
        return std::function<void()>{};
      });
    }
    if (request.source == "remember") {
      saved_ = receiver;
      if (receiver) co_await receiver->OnRows({1});
    }
    if (request.source == "stale" && saved_) co_await saved_->OnRows({2});
    if (request.source == "status-wait") co_await SetRows(42);
    if (request.source == "background-stop") {
      co_await huxerui::pluginkit::RunInvocationWorker([this](std::stop_token stop) {
        Record(context_, "background-start");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        Record(context_, stop.stop_requested() ? "background-stopped" : "background-ended");
      });
    }
    if (request.source == "wait" || request.source == "status-wait") {
#if !defined(_WIN32)
      if (const auto* marker = std::getenv("HPK_CSV_WAIT_MARKER")) std::ofstream(marker).put('\n');
#endif
      co_await huxerui::Delay(std::chrono::minutes(1));
      std::_Exit(6);
    }
    if (request.source == "throw") throw std::runtime_error("Failure fixture");
    co_return csv_import::ImportResult{csv_import::Table{{"value"}, {{request.source}}}, {}};
  }

private:
  static huxerui::Task<void> SetRows(std::uint32_t rows) {
    if (!co_await csv_import::SetImportedRows(rows)) {
      throw std::runtime_error("The host declined the fixture row count");
    }
  }

  const huxerui::pluginkit::PluginContext& context_;
  csv_import::ImportReceiver saved_;
};

} // namespace

using huxerui::pluginkit::BindExtension;

HPK_EXPORT_PLUGIN(OnActivate, OnDeactivate,
    BindExtension<csv_import::TableImporter, FaultImporter>(csv_import::Importers),
    BindExtension<csv_import::TableExporter, csv_import::fixture::CsvExporter>(csv_import::Exporters))
