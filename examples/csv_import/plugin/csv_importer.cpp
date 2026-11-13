#include "csv_contract.bindings.h"
#include "services/import_status.bindings.h"

#include <huxerui/pluginkit/plugin.h>

namespace csv_import {
namespace {

ImportResult Failure(std::string code, std::uint32_t row, std::uint32_t column, std::string message) {
  return {std::nullopt, {{std::move(code), row, column, std::move(message)}}};
}

class CsvImporter final : public TableImporter {
public:
  explicit CsvImporter(const huxerui::pluginkit::PluginContext& context) : context_(context) {}

  huxerui::Task<ImportResult> Import(ImportRequest request, ImportReceiver receiver) override {
    co_await huxerui::Delay(std::chrono::milliseconds(1));
    const auto manifest = co_await context_.BundleDirectory().Child("plugin.json").ReadStringAsync();
    if (!manifest.Succeeded()) {
      throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::Io, "Cannot read the installed CSV plugin manifest");
    }
    auto result = Parse(request);
    if (result.table) {
      const auto rows = static_cast<std::uint32_t>(result.table->rows.size());
      co_await receiver.OnRows({rows});
      if (!co_await huxerui::pluginkit::GetService<ImportStatus>().SetImportedRows({rows})) {
        throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::PluginFailed, "The host declined the imported row count");
      }
    }
    co_return result;
  }

private:
  const huxerui::pluginkit::PluginContext& context_;

  static ImportResult Parse(const ImportRequest& request) {
    std::string_view source = request.source;
    if (source.starts_with("\xef\xbb\xbf")) source.remove_prefix(3);
    if (source.empty()) return Failure("csv.empty", 1, 1, "Expected a header row");

    Table table;
    std::size_t cursor = 0;
    std::size_t cells = 0;
    for (std::uint32_t row_number = 1; cursor < source.size(); ++row_number) {
      std::vector<std::string> row;
      for (std::uint32_t column = 1;; ++column) {
        if (++cells > 200000) return Failure("csv.cell_limit", row_number, column, "Too many cells");
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
          if (cursor < source.size() && source[cursor] != ',' && source[cursor] != '\r' && source[cursor] != '\n') {
            return Failure("csv.after_quote", row_number, column, "Expected a comma or line break after the closing quote");
          }
        } else {
          const auto end = source.find_first_of(",\r\n\"", cursor);
          if (end != source.npos && source[end] == '"') {
            return Failure("csv.unexpected_quote", row_number, column, "A quote must start a field");
          }
          value = source.substr(cursor, end == source.npos ? source.size() - cursor : end - cursor);
          cursor = end == source.npos ? source.size() : end;
        }
        row.push_back(std::move(value));
        if (cursor == source.size() || source[cursor] != ',') break;
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

HPK_EXPORT_PLUGIN(csv_import::TableImporter, csv_import::CsvImporter)
