#pragma once

/// @file
/// @brief Published contract for the CSV importer example.

#include "receivers/import_progress.h"

#include <huxerui/pluginkit/contract.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace csv_import {

/// @brief Maximum input size for an import that materializes a table.
inline constexpr std::size_t MaxImportBytes = 32 * 1024 * 1024;

/// @brief Parsed CSV data with a header and ordered records.
struct Table {
  /// Column names from the first record.
  std::vector<std::string> header;
  /// Records in source order, with one field per header column.
  std::vector<std::vector<std::string>> rows;
};

/// @brief CSV input diagnostic with a source location.
struct Issue {
  /// Stable diagnostic identifier for the import failure.
  std::string code;
  /// One-based record number.
  std::uint32_t row;
  /// One-based field number.
  std::uint32_t column;
  /// Human-readable diagnostic text.
  std::string message;
};

/// @brief Result of an import operation.
struct ImportResult {
  /// Parsed table on success.
  std::optional<Table> table;
  /// Diagnostics reported by the importer.
  std::vector<Issue> issues;
};

/// @brief CSV import contract published to independently built plugins.
class HPK_REMOTE TableImporter {
public:
  virtual ~TableImporter() = default;

  /// @brief Queries the CSV field separator synchronously.
  /// @return Delimiter used by this importer, such as a comma.
  virtual std::string Delimiter() const = 0;

  /// @brief Queries the importer process ID synchronously for preview diagnostics.
  /// @return Host process ID for in-process execution or the worker process ID for worker execution.
  virtual std::uint32_t ProcessId() const = 0;

  /// @brief Parses the supplied CSV.
  /// @param input Sequential UTF-8 CSV source owned by the Task, up to MaxImportBytes.
  /// @param receiver Optional host object that receives progress before the import returns.
  /// @return Lazy Task yielding a parsed table or input diagnostics.
  /// @throws huxerui::pluginkit::Error If activation or execution fails, including failure to update the host status.
  virtual huxerui::Task<ImportResult> Import(huxerui::AsyncInputStream input, std::shared_ptr<ImportProgress> receiver) = 0;
};

/// @brief Extension point for importers that produce a table from delimited text.
inline constexpr huxerui::pluginkit::ExtensionPoint<TableImporter> Importers{"example.table-importers"};

} // namespace csv_import
