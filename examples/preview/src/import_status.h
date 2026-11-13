#pragma once

/// @file
/// @brief Host service contract for the CSV preview.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>

namespace csv_import {

/// @brief Number of data rows imported by the CSV plugin.
struct SetImportedRowsRequest {
  /// Number of rows to record.
  std::uint32_t rows;
};

/// @brief Host-owned import status exposed to approved CSV plugins.
class HPK_REMOTE ImportStatus {
public:
  virtual ~ImportStatus() = default;

  /// @brief Reads the current count from a synchronous in-process invocation.
  /// @return Last recorded row count. The preview returns zero before recording a count.
  virtual std::uint32_t ImportedRows() const = 0;

  /// @brief Updates the number of rows recorded by the host.
  /// @param request Row count to record.
  /// @return Lazy Task yielding true if the host applied the update.
  virtual huxerui::Task<bool> SetImportedRows(SetImportedRowsRequest request) = 0;
};

} // namespace csv_import
