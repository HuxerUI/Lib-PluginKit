#pragma once

/// @file
/// @brief Progress callback for a single photo effect invocation.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/task.h>

#include <cstdint>

namespace photo_studio {

/// @brief Receives progress after the plugin writes each block of output.
class HPK_REMOTE FilterProgress {
public:
  virtual ~FilterProgress() = default;

  /// @brief Reports the number of complete output rows written so far.
  /// @param rows Monotonically increasing row count, up to the requested height.
  virtual huxerui::Task<void> OnRows(std::uint32_t rows) = 0;
};

} // namespace photo_studio
