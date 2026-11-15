#include "csv_contract.bindings.h"
#include "csv_package.h"
#include "csv_sdk.h"

namespace csv_import {
namespace {

using huxerui::pluginkit::PluginContext;
std::uint32_t live_importers = 0;
std::optional<huxerui::RawAsset> cell_limit;

void OnActivate(const PluginContext& context) {
  if (fixture::active_context) throw std::runtime_error("The CSV package is already active");
  auto limit = context.Resources().GetRawResource(csv_resources::raw::cell_limit_txt);
  fixture::Record(context, "activate");
  cell_limit.emplace(std::move(limit));
  fixture::active_context = &context;
}

void OnDeactivate(const PluginContext& context) {
  if (fixture::active_context != &context || live_importers || fixture::live_exporters) {
    throw std::runtime_error("Expected the original context after all CSV instances were destroyed");
  }
  (void)fixture::ReadCellLimit(*cell_limit);
  fixture::Record(context, "deactivate");
  cell_limit.reset();
  fixture::active_context = nullptr;
}

template<char Separator>
class CsvImporter final : public TableImporter {
public:
  explicit CsvImporter(const PluginContext& context) : context_(context) {
    if (fixture::active_context != &context) throw std::runtime_error("Expected the activation's context");
    ++live_importers;
  }

  ~CsvImporter() override { --live_importers; }

  std::string Delimiter() const override { return std::string(1, Separator); }
  std::uint32_t ProcessId() const override { return csv_import::ProcessId(); }

  huxerui::Task<ImportResult> Import(huxerui::AsyncInputStream input, std::shared_ptr<ImportProgress> receiver) override {
    using huxerui::pluginkit::Error;
    using huxerui::pluginkit::ErrorCode;
    co_await huxerui::Delay(std::chrono::milliseconds(1));
    fixture::CsvRecords parser(Separator, fixture::ReadCellLimit(*cell_limit),
                               std::get<std::int64_t>(context_.Settings().at("max_rows")));
    Table table;
    const auto append = [&table](fixture::Row row, bool header) {
      if (header) table.header = std::move(row);
      else table.rows.push_back(std::move(row));
    };
    auto remaining = MaxImportBytes;
    for (;;) {
      const auto bytes = co_await fixture::ReadImportChunk(input, remaining);
      if (bytes.empty()) break;
      if (!parser.Feed(bytes, append)) co_return ImportResult{std::nullopt, {parser.Error()}};
    }
    if (!parser.Finish(append)) co_return ImportResult{std::nullopt, {parser.Error()}};
    const auto rows = parser.Rows();
    if (receiver) co_await receiver->OnRows(rows, MaxImportBytes - remaining);
    if (!co_await SetImportedRows(rows)) {
      throw Error(ErrorCode::PluginFailed, "The host declined the imported row count");
    }
    co_return ImportResult{std::move(table), {}};
  }

private:
  const PluginContext& context_;
};

} // namespace
} // namespace csv_import

using huxerui::pluginkit::BindExtension;

HPK_EXPORT_PLUGIN(csv_import::OnActivate, csv_import::OnDeactivate,
    BindExtension<csv_import::TableImporter, csv_import::CsvImporter<','>>(csv_import::Importers),
    BindExtension<csv_import::TableExporter, csv_import::fixture::CsvExporter>(csv_import::Exporters),
    BindExtension<csv_import::TableImporter, csv_import::CsvImporter<';'>>(csv_import::Importers))
