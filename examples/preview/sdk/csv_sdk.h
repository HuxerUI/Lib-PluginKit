#pragma once

/// @file
/// @brief Convenience functions provided by the CSV host SDK.
///
/// Include this header in plugin code to call the HPK_REMOTE import status service from a managed Task.

#include "services/import_status.bindings.h"

#include <huxerui/pluginkit/plugin.h>

namespace csv_import {

/// @brief Records the imported row count in the host for the current invocation.
/// @param rows Number of imported data rows, excluding the header.
/// @return Lazy Task yielding true if the host applied the update.
/// @throws huxerui::pluginkit::Error If the package lacks the service grant or the host call fails.
inline huxerui::Task<bool> SetImportedRows(std::uint32_t rows) {
  auto status = huxerui::pluginkit::GetService<ImportStatus>();
  co_return co_await status->SetImportedRows({rows});
}

} // namespace csv_import
