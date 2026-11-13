#pragma once

/// @file
/// @brief Published contract for the CSV importer example.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace csv_import {

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
  /// @return Parsed data on success or input diagnostics.
  virtual ImportResult Import(const ImportRequest& request) = 0;
};

} // namespace csv_import
