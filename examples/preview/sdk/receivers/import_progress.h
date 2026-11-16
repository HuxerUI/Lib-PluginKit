#pragma once

/// @file
/// @brief Callback contract for one CSV import.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>

namespace csv_import {

/// @brief Receives row count updates for one invocation.
class HPK_REMOTE ImportProgress {
public:
  virtual ~ImportProgress() = default;

  /// @brief Handles a row count update before the import continues.
  /// @param rows Parsed row count for this invocation.
  /// @param input_bytes Bytes consumed from the source.
  /// @return Task that completes after the receiver handles the update on the host application thread.
  virtual huxerui::Task<void> OnRows(std::uint32_t rows, std::uint64_t input_bytes) = 0;
};

} // namespace csv_import
