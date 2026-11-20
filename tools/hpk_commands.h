#pragma once

#include <filesystem>
#include <span>

namespace huxerui::pluginkit::detail {

void GenerateSdk(std::span<const std::filesystem::path> arguments);
void GenerateAbi(std::span<const std::filesystem::path> arguments);
void PackPlugin(std::span<const std::filesystem::path> arguments);

} // namespace huxerui::pluginkit::detail
