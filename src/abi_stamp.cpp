#include <huxerui/pluginkit/plugin.h>

#include <filesystem>
#include <fstream>

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 2) return 1;
  std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
  output << huxerui::pluginkit::detail::CppAbi;
  output.close();
  return output ? 0 : 1;
}
