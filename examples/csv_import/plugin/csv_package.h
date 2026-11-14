#pragma once

#include "exporters/export_contract.bindings.h"
#include "process_id.h"

#include <csv_resources_resources.h>
#include <huxerui/pluginkit/plugin.h>

#include <charconv>
#include <stdexcept>

namespace csv_import::fixture {

inline const huxerui::pluginkit::PluginContext* active_context = nullptr;
inline std::uint32_t live_exporters = 0;

inline std::uint32_t ReadCellLimit(const huxerui::RawAsset& asset) {
  const auto text = asset.ReadString();
  std::string_view value_text(text);
  while (!value_text.empty() && (value_text.back() == '\r' || value_text.back() == '\n')) value_text.remove_suffix(1);
  std::uint32_t value = 0;
  const auto end = value_text.data() + value_text.size();
  const auto parsed = std::from_chars(value_text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !value) {
    throw std::runtime_error("Invalid CSV cell limit resource");
  }
  return value;
}

inline void Record(const huxerui::pluginkit::PluginContext& context, std::string_view event) {
  if (!context.DataDirectory().Child("lifecycle.log").AppendString(std::string(event) + '\n')) {
    throw std::runtime_error("Cannot write the CSV package lifecycle log");
  }
}

class CsvExporter final : public TableExporter {
public:
  explicit CsvExporter(const huxerui::pluginkit::PluginContext& context) {
    if (active_context != &context) throw std::runtime_error("Expected the activation's context");
#if defined(CSV_IMPORT_UPGRADE)
    if (context.DataDirectory().Child("fail-exporter").Exists()) {
      throw std::runtime_error("The CSV candidate could not construct its exporter");
    }
#endif
    ++live_exporters;
  }

  ~CsvExporter() override { --live_exporters; }

  std::string Format() const override { return "csv"; }
  std::uint32_t ProcessId() const override { return csv_import::ProcessId(); }

  huxerui::Task<std::string> Export(ExportRequest request) override {
    std::string output;
    const auto write = [&](const std::vector<std::string>& row) {
      if (row.size() != request.header.size() || row.empty()) {
        throw std::runtime_error("CSV records must match the header width");
      }
      // Quoting every field preserves empty cells and embedded separators and line breaks.
      for (std::size_t column = 0; column < row.size(); ++column) {
        if (column != 0) output.push_back(',');
        output.push_back('"');
        for (const auto character : row[column]) {
          if (character == '"') output.push_back('"');
          output.push_back(character);
        }
        output.push_back('"');
      }
      output += "\r\n";
    };
    write(request.header);
    for (const auto& row : request.rows) write(row);
    co_return output;
  }
};

} // namespace csv_import::fixture
