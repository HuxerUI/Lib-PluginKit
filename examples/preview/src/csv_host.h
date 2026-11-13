#pragma once

#include <filesystem>
#include <span>

namespace csv_import::preview {

std::filesystem::path DefaultProfile();
std::filesystem::path WorkerPath();
int Run(std::span<const std::filesystem::path> args);

} // namespace csv_import::preview
