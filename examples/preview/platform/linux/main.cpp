#include "csv_host.h"

#include <huxerui/pluginkit/contract.h>

#include <cstdlib>
#include <vector>

namespace csv_import::preview {

namespace fs = std::filesystem;

fs::path DefaultProfile() {
  fs::path data;
  if (const auto* value = std::getenv("XDG_DATA_HOME")) data = value;
  if (!data.is_absolute()) {
    const auto* home = std::getenv("HOME");
    if (!home || !fs::path(home).is_absolute()) {
      throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::Io, "Cannot locate local application data");
    }
    data = fs::path(home) / ".local" / "share";
  }
  return data / "huxerui" / "lib-pluginkit" / "preview";
}

fs::path WorkerPath() { return fs::read_symlink("/proc/self/exe").parent_path() / "hpk-worker"; }

} // namespace csv_import::preview

int main(int argc, char** argv) {
  return csv_import::preview::Run(std::vector<std::filesystem::path>(argv + 1, argv + argc));
}
