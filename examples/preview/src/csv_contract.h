#pragma once

/// @file
/// @brief Published contract for the CSV importer example.

#include "receivers/import_progress.h"

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace csv_import {

/// @brief Proxy for progress notifications during an import invocation.
using ImportReceiver = huxerui::pluginkit::Receiver<ImportProgress>;

/// @brief Input passed by value across the worker boundary.
struct ImportRequest {
  /// UTF-8 CSV contents supplied by the host.
  std::string source;
};

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
class TableImporter {
public:
  virtual ~TableImporter() = default;

  /// @brief Parses the supplied CSV.
  /// @param request CSV input for this call.
  /// @param receiver Receives progress for this invocation.
  /// @return Task producing parsed data or input diagnostics.
  virtual huxerui::Task<ImportResult> Import(ImportRequest request, ImportReceiver receiver) = 0;
};

} // namespace csv_import
