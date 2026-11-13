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
class ImportStatus {
public:
  virtual ~ImportStatus() = default;

  /// @brief Updates the number of rows recorded by the host.
  /// @param request Row count to record.
  /// @return Task yielding true if the host applied the update.
  virtual huxerui::Task<bool> SetImportedRows(SetImportedRowsRequest request) = 0;
};

} // namespace csv_import
