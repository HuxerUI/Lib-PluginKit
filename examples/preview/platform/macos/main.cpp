#include "csv_host.h"

#include <huxerui/pluginkit/contract.h>

#include <mach-o/dyld.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace csv_import::preview {

namespace fs = std::filesystem;
using huxerui::pluginkit::Error;
using huxerui::pluginkit::ErrorCode;

fs::path DefaultProfile() {
  const auto* home = std::getenv("HOME");
  if (!home || !fs::path(home).is_absolute()) throw Error(ErrorCode::Io, "Cannot locate application support");
  return fs::path(home) / "Library" / "Application Support" / "HuxerUI" / "Lib-PluginKit" / "Preview";
}

fs::path WorkerPath() {
  std::string path(1024, '\0');
  auto length = static_cast<std::uint32_t>(path.size());
  if (::_NSGetExecutablePath(path.data(), &length) != 0) {
    path.resize(length);
    if (::_NSGetExecutablePath(path.data(), &length) != 0) {
      throw Error(ErrorCode::Io, "Cannot locate the sample host executable");
    }
  }
  return fs::canonical(path.c_str()).parent_path() / "hpk-worker";
}

} // namespace csv_import::preview

int main(int argc, char** argv) {
  return csv_import::preview::Run(std::vector<std::filesystem::path>(argv + 1, argv + argc));
}
