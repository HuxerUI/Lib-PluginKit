#include "csv_host.h"

#include <pluginkit/error.h>
#include <windows.h>

#include <vector>

namespace csv_import::preview {

namespace fs = std::filesystem;
using pluginkit::Error;
using pluginkit::ErrorCode;

fs::path DefaultProfile() {
  std::wstring path(32768, L'\0');
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) throw Error(ErrorCode::Io, "Cannot locate local application data");
  path.resize(length);
  return fs::path(path) / L"HuxerUI" / L"Lib-PluginKit" / L"Preview";
}

fs::path WorkerPath() {
  std::wstring path(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size()) throw Error(ErrorCode::Io, "Cannot locate the sample host executable");
  path.resize(length);
  return fs::path(path).parent_path() / L"hpk-worker.exe";
}

} // namespace csv_import::preview

int wmain(int argc, wchar_t** argv) {
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
  return csv_import::preview::Run(std::vector<std::filesystem::path>(argv + 1, argv + argc));
}
