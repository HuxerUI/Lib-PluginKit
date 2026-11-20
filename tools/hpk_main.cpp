#include "hpk_commands.h"

#include <exception>
#include <iostream>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace huxerui::pluginkit::detail;

void PrintUsage(std::ostream& output) {
  output << "Usage:\n"
         << "  hpk pack [platform] [--target <name>] [huxerui build options]\n";
#if defined(HPK_HAS_GENERATOR)
  output << "  hpk gen <base-directory> <output-dir> <header-list> [-- <clang-options>]\n";
#endif
}

int Run(std::span<const fs::path> args) {
  if (args.size() >= 2 && (args[1] == "--help" || args[1] == "-h" ||
      (args[1] == "pack" && args.size() == 3 && args[2] == "--help"))) {
    PrintUsage(std::cout);
    return 0;
  }
  try {
    if (args.size() >= 2 && args[1] == "pack") {
      PackPlugin(args.subspan(2));
      return 0;
    }
#if defined(HPK_HAS_GENERATOR)
    if (args.size() >= 2 && args[1] == "abi") {
      GenerateAbi(args.subspan(2));
      return 0;
    }
    if (args.size() >= 2 && args[1] == "gen") {
      GenerateSdk(args.subspan(2));
      return 0;
    }
#endif
    PrintUsage(std::cerr);
  } catch (const std::exception& error) {
    std::cerr << "hpk: " << error.what() << '\n';
  }
  return 1;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  return Run(std::vector<fs::path>(argv, argv + argc));
}
