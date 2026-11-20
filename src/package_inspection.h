#pragma once

#include <huxerui/pluginkit.h>

namespace huxerui::pluginkit::detail {

/// Reuses normal approval checks for developer-machine inspection of a cross-compiled package.
/// Installation always uses the executing platform's target instead.
PackageInfo InspectPackageForTarget(const File& directory, std::string_view target);

} // namespace huxerui::pluginkit::detail
