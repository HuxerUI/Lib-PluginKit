#pragma once

/// @file
/// @brief Published contract for the CSV exporter example.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>
#include <string>
#include <vector>

namespace csv_import {

/// @brief Table data supplied by the host for export.
struct ExportRequest {
  /// Column names to write as the first record.
  std::vector<std::string> header;
  /// Data records in output order.
  std::vector<std::vector<std::string>> rows;
};

/// @brief Contract for exporting a table to a text format.
class HPK_REMOTE TableExporter {
public:
  virtual ~TableExporter() = default;

  /// @brief Queries the format produced by this exporter synchronously.
  /// @return The exporter format, such as "csv".
  virtual std::string Format() const = 0;

  /// @brief Queries the exporter process ID synchronously for preview diagnostics.
  /// @return Host process ID for in-process execution or the worker process ID for worker execution.
  virtual std::uint32_t ProcessId() const = 0;

  /// @brief Encodes the supplied table as text.
  /// @param request Table contents owned by the Task.
  /// @return Lazy Task yielding the encoded contents.
  virtual huxerui::Task<std::string> Export(ExportRequest request) = 0;
};

/// @brief Extension point for exporters that encode a table as text.
inline constexpr huxerui::pluginkit::ExtensionPoint<TableExporter> Exporters{"example.table-exporters"};

} // namespace csv_import
