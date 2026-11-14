#include "csv_contract.bindings.h"
#include "csv_package.h"
#include "csv_sdk.h"

#include <array>

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

ImportResult Failure(std::string code, std::uint32_t row, std::uint32_t column, std::string message) {
  return {std::nullopt, {{std::move(code), row, column, std::move(message)}}};
}

template<char Separator>
class CsvImporter final : public TableImporter {
public:
  explicit CsvImporter(const PluginContext& context) {
    if (fixture::active_context != &context) throw std::runtime_error("Expected the activation's context");
    ++live_importers;
  }

  ~CsvImporter() override { --live_importers; }

  std::string Delimiter() const override { return std::string(1, Separator); }
  std::uint32_t ProcessId() const override { return csv_import::ProcessId(); }

  huxerui::Task<ImportResult> Import(ImportRequest request, ImportReceiver receiver) override {
    co_await huxerui::Delay(std::chrono::milliseconds(1));
    auto result = Parse(request, fixture::ReadCellLimit(*cell_limit));
    if (result.table) {
      const auto rows = static_cast<std::uint32_t>(result.table->rows.size());
      if (receiver) co_await receiver->OnRows({rows});
      if (!co_await SetImportedRows(rows)) {
        throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::PluginFailed, "The host declined the imported row count");
      }
    }
    co_return result;
  }

private:
  static ImportResult Parse(const ImportRequest& request, std::uint32_t limit) {
    std::string_view source = request.source;
    if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
    if (source.empty()) return Failure("csv.empty", 1, 1, "Expected a header row");
    constexpr std::array delimiters{Separator, '\r', '\n', '"'};

    Table table;
    std::size_t cursor = 0;
    std::size_t cells = 0;
    for (std::uint32_t row_number = 1; cursor < source.size(); ++row_number) {
      std::vector<std::string> row;
      for (std::uint32_t column = 1;; ++column) {
        if (++cells > limit) return Failure("csv.cell_limit", row_number, column, "Too many cells");
        std::string value;
        if (cursor < source.size() && source[cursor] == '"') {
          ++cursor;
          for (;;) {
            const auto quote = source.find('"', cursor);
            if (quote == source.npos) return Failure("csv.unclosed_quote", row_number, column, "Expected a closing quote");
            value.append(source.substr(cursor, quote - cursor));
            cursor = quote + 1;
            if (cursor == source.size() || source[cursor] != '"') break;
            value.push_back('"');
            ++cursor;
          }
          if (cursor < source.size() && source[cursor] != Separator && source[cursor] != '\r' && source[cursor] != '\n') {
            return Failure("csv.after_quote", row_number, column, "Expected a separator or line break after the closing quote");
          }
        } else {
          const auto end = source.find_first_of(std::string_view(delimiters.data(), delimiters.size()), cursor);
          if (end != source.npos && source[end] == '"') {
            return Failure("csv.unexpected_quote", row_number, column, "A quote must start a field");
          }
          value = source.substr(cursor, end == source.npos ? source.size() - cursor : end - cursor);
          cursor = end == source.npos ? source.size() : end;
        }
        row.push_back(std::move(value));
        if (cursor == source.size() || source[cursor] != Separator) break;
        ++cursor;
      }

      if (row_number == 1) {
        table.header = std::move(row);
      } else {
        if (row.size() != table.header.size()) {
          return Failure("csv.column_count", row_number, 1, "The row width differs from the header");
        }
        table.rows.push_back(std::move(row));
      }
      if (cursor == source.size()) break;
      const char newline = source[cursor++];
      if (newline == '\r' && cursor < source.size() && source[cursor] == '\n') ++cursor;
    }
    return {std::move(table), {}};
  }
};

} // namespace
} // namespace csv_import

using huxerui::pluginkit::BindExtension;

HPK_EXPORT_PLUGIN(csv_import::OnActivate, csv_import::OnDeactivate,
    BindExtension<csv_import::TableImporter, csv_import::CsvImporter<','>>(csv_import::Importers),
    BindExtension<csv_import::TableExporter, csv_import::fixture::CsvExporter>(csv_import::Exporters),
    BindExtension<csv_import::TableImporter, csv_import::CsvImporter<';'>>(csv_import::Importers))
