#pragma once

/// @file
/// @brief Callback contract for one CSV import.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>

namespace csv_import {

/// @brief Number of data rows parsed by the importer.
struct ImportedRows {
  /// Number of parsed records after the header row.
  std::uint32_t rows;
};

/// @brief Receives row count updates for one invocation.
class HPK_REMOTE ImportProgress {
public:
  virtual ~ImportProgress() = default;

  /// @brief Handles a row count update before the import continues.
  /// @param update Parsed row count for this invocation.
  /// @return Task that completes after the receiver handles the update on the host application thread.
  virtual huxerui::Task<void> OnRows(ImportedRows update) = 0;
};

} // namespace csv_import
