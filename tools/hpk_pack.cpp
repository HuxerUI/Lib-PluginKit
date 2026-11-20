#include "digest.h"
#include "hpk_commands.h"
#include "package_inspection.h"

#include <huxerui/pluginkit.h>
#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

#include <algorithm>
#include <iostream>
#include <optional>
#include <vector>

namespace huxerui::pluginkit::detail {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
// Quote a native argument for the Windows C runtime's command-line parser.
std::wstring QuoteArgument(std::wstring_view value) {
  std::wstring quoted{L"\""};
  std::size_t backslashes = 0;
  for (const auto character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') backslashes = backslashes * 2 + 1;
    quoted.append(backslashes, L'\\');
    quoted.push_back(character);
    backslashes = 0;
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

unsigned long Build(std::span<const fs::path> arguments, const std::string& invocation, bool native = false) {
  if (!SetEnvironmentVariableW(L"HPK_PACK_INVOCATION", fs::path(invocation).c_str())) {
    WindowsError(ErrorCode::Io, "Cannot mark the hpk build invocation");
  }
  std::wstring command_line = native ? L"cmake.exe" : L"huxerui.exe build";
  for (const auto& argument : arguments) {
    command_line.push_back(L' ');
    command_line += QuoteArgument(argument.native());
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) {
    WindowsError(ErrorCode::Io, "Cannot start huxerui build");
  }
  const Handle process(child.hProcess);
  CloseHandle(child.hThread);
  if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0) {
    WindowsError(ErrorCode::Io, "Cannot wait for huxerui build");
  }
  DWORD status = 0;
  if (!GetExitCodeProcess(process.get(), &status)) WindowsError(ErrorCode::Io, "Cannot read huxerui build exit code");
  return status;
}
#else
int Build(std::span<const fs::path> arguments, const std::string& invocation, bool native = false) {
  if (::setenv("HPK_PACK_INVOCATION", invocation.c_str(), 1) != 0) {
    PosixError(ErrorCode::Io, "Cannot mark the hpk build invocation");
  }
  std::vector<std::string> command = native ? std::vector<std::string>{"cmake"} : std::vector<std::string>{"huxerui", "build"};
  for (const auto& argument : arguments) command.push_back(argument.native());
  std::vector<char*> argv;
  for (auto& argument : command) argv.push_back(argument.data());
  argv.push_back(nullptr);
#if defined(__APPLE__)
  auto** environment = *_NSGetEnviron();
#else
  auto** environment = environ;
#endif
  pid_t child = 0;
  const int spawn_error = ::posix_spawnp(&child, argv.front(), nullptr, nullptr, argv.data(), environment);
  if (spawn_error) PosixError(ErrorCode::Io, "Cannot start huxerui build", spawn_error);
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) PosixError(ErrorCode::Io, "Cannot wait for huxerui build");
  }
  if (WIFSIGNALED(status)) {
    throw Error(ErrorCode::Io, "huxerui build terminated by signal " + std::to_string(WTERMSIG(status)));
  }
  return WEXITSTATUS(status);
}
#endif

std::vector<fs::path> ScanPackages(const fs::path& build_root, const std::string& marker_suffix) {
  std::vector<fs::path> packages;
  if (!fs::is_directory(build_root)) return packages;

  // HuxerUI keeps each SDK, platform/ABI, and build configuration in a separate tree.
  for (fs::recursive_directory_iterator item(build_root), end; item != end; ++item) {
    if (item.depth() == 0 && !PathText(item->path().filename()).starts_with("home-")) {
      item.disable_recursion_pending();
    }
    if (item.depth() != 2) continue;
    item.disable_recursion_pending();
    if (!item->is_directory()) continue;
    const auto root = item->path() / "packages";
    if (!fs::is_directory(root)) continue;
    for (const auto& target : fs::directory_iterator(root)) {
      if (!target.is_directory()) continue;
      for (const std::string_view config : {"Debug", "Release"}) {
        const auto marker = target.path() / (std::string(config) + marker_suffix);
        if (!fs::is_regular_file(marker)) continue;
        packages.push_back(target.path() / config);
        std::error_code error;
        fs::remove(marker, error);
      }
    }
  }
  std::sort(packages.begin(), packages.end());
  return packages;
}

std::vector<fs::path> FindPackages(const std::string& invocation) {
  const auto marker_suffix = ".hpk-" + invocation;
  for (auto directory = fs::current_path();; directory = directory.parent_path()) {
    const auto android = directory / "platform/android/app/.cxx";
    if (fs::is_directory(android)) {
      std::vector<fs::path> packages;
      for (fs::recursive_directory_iterator item(android), end; item != end; ++item) {
        if (item->is_directory() && item->path().filename() == "packages") {
          for (const auto& target : fs::directory_iterator(item->path())) {
            if (!target.is_directory()) continue;
            for (const auto config : {"Debug", "Release"}) {
              const auto marker = target.path() / (std::string(config) + marker_suffix);
              if (fs::is_regular_file(marker)) {
                packages.push_back(target.path() / config);
                fs::remove(marker);
              }
            }
          }
          item.disable_recursion_pending();
        } else if (item.depth() >= 3) item.disable_recursion_pending();
      }
      if (!packages.empty()) { std::sort(packages.begin(), packages.end()); return packages; }
    }
    auto packages = ScanPackages(directory / ".huxerui" / "build", marker_suffix);
    if (!packages.empty()) return packages;
    packages = ScanPackages(directory / "examples" / "preview" / ".huxerui" / "build", marker_suffix);
    if (!packages.empty() || directory == directory.root_path()) return packages;
  }
}

void BuildAndroidPackages(const std::vector<fs::path>& arguments, const std::optional<fs::path>& selected,
                          const std::string& invocation) {
  std::string config = "release";
  for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
    if (arguments[i] == "--config") config = PathText(arguments[i + 1]);
  }
  const auto capitalized = config == "debug" ? "Debug" : "Release";
  for (auto directory = fs::current_path();; directory = directory.parent_path()) {
    const auto app = directory / "platform/android/app/build";
    const auto integration = app / "generated/huxerui/integration" / config;
    if (fs::is_directory(integration)) {
      // The merged ABI directories belong to the APK just built, excluding stale CMake configurations.
      const auto merged = app / "intermediates/merged_native_libs" / config /
          (std::string("merge") + capitalized + "NativeLibs/out/lib");
      if (fs::is_directory(merged)) for (const auto& abi : fs::directory_iterator(merged)) {
        const auto path = integration / abi.path().filename() / "pluginkit.json";
        const auto app_plan = integration / abi.path().filename() / "app.json";
        if (!fs::is_regular_file(path) || !fs::is_regular_file(app_plan)) continue;
        const auto plan = nlohmann::json::parse(AsText(ReadFileBytes(path, 64 * 1024)));
        const auto application = nlohmann::json::parse(AsText(ReadFileBytes(app_plan, 64 * 1024)));
        if (plan.at("schema") != 1) throw Error(ErrorCode::InvalidArgument, "Invalid Android package build plan");
        const fs::path artifact(application.at("artifact").get<std::string>());
        const fs::path build_directory(plan.at("build_directory").get<std::string>());
        if (!fs::is_regular_file(abi.path() / artifact.filename()) ||
            build_directory.parent_path().filename() != artifact.parent_path().parent_path().parent_path().filename()) continue;
        std::vector<fs::path> command{"--build", plan.at("build_directory").get<std::string>(), "--config", capitalized, "--target"};
        for (const auto& value : plan.at("targets")) {
          const auto target = value.get<std::string>();
          if (!selected || target == PathText(*selected) + "-package") command.emplace_back(target);
        }
        if (command.size() > 5 && Build(command, invocation, true) != 0) {
          throw Error(ErrorCode::Io, "Android plugin package target failed");
        }
      }
      return;
    }
    if (directory == directory.root_path()) return;
  }
}

} // namespace

void PackPlugin(std::span<const fs::path> arguments) {
  std::optional<fs::path> target;
  std::vector<fs::path> build_arguments;
  bool config_selected = false;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument == "--target") {
      if (target || ++i == arguments.size() || arguments[i].empty() || arguments[i].has_parent_path() ||
          arguments[i] == "." || arguments[i] == ".." || PathText(arguments[i]).starts_with("--")) {
        throw Error(ErrorCode::InvalidArgument, "Specify one CMake target name with --target");
      }
      target = arguments[i];
    } else {
      if (argument == "--config") config_selected = true;
      build_arguments.push_back(argument);
    }
  }
  if (!config_selected) build_arguments.insert(build_arguments.end(), {"--config", "release"});

  const auto invocation = NewToken();
  const auto status = Build(build_arguments, invocation);
  if (status == 0 && std::ranges::find(build_arguments, fs::path("android")) != build_arguments.end()) {
    BuildAndroidPackages(build_arguments, target, invocation);
  }
  auto packages = FindPackages(invocation);
  if (status != 0) throw Error(ErrorCode::Io, "huxerui build failed with exit code " + std::to_string(status));

  if (target) std::erase_if(packages, [&](const auto& path) { return path.parent_path().filename() != *target; });
  if (packages.empty()) {
    const auto name = target ? " for target " + PathText(*target) : std::string{};
    throw Error(ErrorCode::InvalidArgument, "This build produced no plugin package" + name);
  }
  if (packages.size() > 1) {
    if (target) throw Error(ErrorCode::InvalidArgument, "Build a single platform and ABI for target " + PathText(*target));
    std::string message = "This build produced multiple plugin packages. Select one with --target <name>.";
    for (const auto& path : packages) message += "\n  " + PathText(path.parent_path().filename());
    throw Error(ErrorCode::InvalidArgument, message);
  }

  const auto& directory = packages.front();
  const auto path = PathText(directory);
  PackageInfo package;
  if (std::ranges::find(build_arguments, fs::path("android")) != build_arguments.end()) {
    const auto manifest = nlohmann::json::parse(AsText(ReadFileBytes(directory / "plugin.json", 64 * 1024)));
    const auto target_name = manifest.at("target").get<std::string>();
    if (!target_name.starts_with("android-")) throw Error(ErrorCode::InvalidPackage, "Expected an Android package");
    package = InspectPackageForTarget(File(path), target_name);
  } else package = PluginManager::InspectPackage(File(path));
  const nlohmann::json result{{"path", path}, {"target", PathText(directory.parent_path().filename())},
      {"id", package.id}, {"version", package.version}, {"digest", package.digest}};
  std::cout << result.dump() << '\n';
}

} // namespace huxerui::pluginkit::detail
