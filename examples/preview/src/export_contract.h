#pragma once

/// @file
/// @brief Published contract for the CSV exporter example.

#include "receivers/import_progress.h"

#include <huxerui/pluginkit/contract.h>
#include <huxerui/stream.h>
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

/// @brief Size and record count of a completed semicolon-to-comma conversion.
struct CsvSummary {
  /// Data records written, excluding the header.
  std::uint32_t rows;
  /// Bytes consumed from the input stream.
  std::uint64_t input_bytes;
  /// Bytes written to the output stream.
  std::uint64_t output_bytes;
};

/// @brief Maximum amount the converter asks the source to produce for one read.
struct ConversionOptions {
  /// Positive read size in the range 1 through 1 MiB, with a default of 512 KiB.
  std::uint32_t read_bytes = 512 * 1024;
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
  /// @param output Destination for the CSV text, closed before the Task succeeds.
  /// @return Lazy Task that completes after writing and closing the destination.
  virtual huxerui::Task<void> Export(ExportRequest request, huxerui::AsyncOutputStream output) = 0;

  /// @brief Converts semicolon-delimited UTF-8 to CSV in the calling thread.
  /// @param options Read size for the incremental conversion.
  /// @param input Sequential source for this invocation.
  /// @param output Sequential destination, closed before returning.
  /// @return Counts for the completed conversion.
  virtual CsvSummary Convert(ConversionOptions options, huxerui::InputStream input, huxerui::OutputStream output) = 0;

  /// @brief Converts semicolon-delimited UTF-8 to CSV in a Task.
  /// @param options Read size for the incremental conversion.
  /// @param input Sequential source for this invocation.
  /// @param output Sequential destination, closed before the Task succeeds.
  /// @param progress Optional receiver for row counts reported as data is written.
  /// @return Lazy Task yielding counts for the completed conversion.
  virtual huxerui::Task<CsvSummary> ConvertAsync(ConversionOptions options, huxerui::AsyncInputStream input,
      huxerui::AsyncOutputStream output, huxerui::pluginkit::Receiver<ImportProgress> progress) = 0;
};

/// @brief Extension point for exporters that encode a table as text.
inline constexpr huxerui::pluginkit::ExtensionPoint<TableExporter> Exporters{"example.table-exporters"};

} // namespace csv_import
