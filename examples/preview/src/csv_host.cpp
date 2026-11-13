#include "csv_host.h"

#include "csv_contract.host.h"

#include <huxerui/pluginkit.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace pluginkit;

constexpr ExtensionPoint<csv_import::TableImporter> Importers{"example.table-importers"};
constexpr char CommandHelp[] =
    "inspect <package>, install <package> <digest>, list, enable <id>, disable <id>, "
    "import <file> [plugin-id] [timeout-ms], quit";

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

fs::path Path(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string ReadCsv(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw Error(ErrorCode::Io, "Cannot open the CSV file");
  std::string contents;
  std::array<char, 8192> buffer{};
  while (file) {
    file.read(buffer.data(), buffer.size());
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count > MaxPayload - contents.size()) throw Error(ErrorCode::PayloadTooLarge, "CSV input exceeds the payload limit");
    contents.append(buffer.data(), count);
  }
  if (!file.eof() || file.bad()) throw Error(ErrorCode::Io, "Cannot read the CSV file");
  return contents;
}

Json Describe(const PackageInfo& package) {
  return {{"id", package.id}, {"version", package.version}, {"digest", package.digest},
          {"contribution", package.contribution}, {"extensions", package.extensions}};
}

Json List(PluginManager& manager) {
  auto plugins = Json::array();
  for (const auto& plugin : manager.ListPlugins()) {
    auto record = Describe(plugin.package);
    record["enabled"] = plugin.enabled;
    record["worker_pid"] = plugin.worker_pid ? Json(*plugin.worker_pid) : Json(nullptr);
    plugins.push_back(std::move(record));
  }
  auto importers = Json::array();
  for (const auto& extension : manager.ListExtensions(Importers)) {
    importers.push_back({{"plugin", extension.id.plugin}, {"id", extension.id.contribution},
                         {"extensions", extension.extensions}});
  }
  return {{"plugins", std::move(plugins)}, {"importers", std::move(importers)}};
}

Json Import(PluginManager& manager, const std::vector<std::string>& args) {
  const auto path = Path(args[1]);
  auto suffix = Utf8(path.extension());
  for (auto& character : suffix) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  std::optional<ExtensionId> selected;
  for (const auto& extension : manager.ListExtensions(Importers)) {
    if (args.size() >= 3 && extension.id.plugin != args[2]) continue;
    if (std::ranges::find(extension.extensions, suffix) == extension.extensions.end()) continue;
    if (selected) throw Error(ErrorCode::InvalidArgument, "Several importers match this file. Specify a plugin ID.");
    selected = extension.id;
  }
  if (!selected) throw Error(ErrorCode::NotFound, "No enabled importer matches the file");
  auto timeout = DefaultTimeout;
  if (args.size() == 4) {
    std::uint32_t milliseconds = 0;
    const auto end = args[3].data() + args[3].size();
    const auto result = std::from_chars(args[3].data(), end, milliseconds);
    if (result.ec != std::errc{} || result.ptr != end || milliseconds == 0 ||
        std::chrono::milliseconds(milliseconds) > std::chrono::hours(24)) {
      throw Error(ErrorCode::InvalidArgument, "Invalid timeout in milliseconds");
    }
    timeout = std::chrono::milliseconds(milliseconds);
  }
  auto source = ReadCsv(path);
  auto importer = manager.OpenExtension(Importers, *selected);
  auto imported = importer.Import({std::move(source)}, timeout);
  importer.Close();

  auto issues = Json::array();
  for (const auto& issue : imported.issues) {
    issues.push_back({{"code", issue.code}, {"row", issue.row}, {"column", issue.column}, {"message", issue.message}});
  }
  Json table = nullptr;
  if (imported.table) table = {{"header", imported.table->header}, {"rows", imported.table->rows}};
  return {{"table", std::move(table)}, {"issues", std::move(issues)}};
}

Json Command(PluginManager& manager, const std::vector<std::string>& args) {
  const auto& name = args.front();
  if (name == "inspect" && args.size() == 2) return Describe(PluginManager::InspectPackage(Path(args[1])));
  if (name == "install" && args.size() == 3) {
    manager.InstallPackage(Path(args[1]), args[2]);
    return Json::object();
  }
  if ((name == "enable" || name == "disable") && args.size() == 2) {
    manager.SetEnabled(args[1], name == "enable");
    return Json::object();
  }
  if (name == "list" && args.size() == 1) return List(manager);
  if (name == "import" && args.size() >= 2 && args.size() <= 4) return Import(manager, args);
  throw Error(ErrorCode::InvalidArgument, CommandHelp);
}

void Print(const Json& response) {
  std::cout << response.dump(-1, ' ', false, Json::error_handler_t::replace) << std::endl;
}

void PrintError(const std::exception& error) {
  const auto* kit_error = dynamic_cast<const Error*>(&error);
  Print({{"ok", false}, {"error", kit_error ? ErrorName(kit_error->Code()) : "io"}, {"message", error.what()}});
}

void Report(PluginManager& manager, const std::vector<std::string>& args) {
  auto response = Command(manager, args);
  response["ok"] = true;
  Print(response);
}

std::vector<std::string> Split(const std::string& line) {
  std::istringstream input(line);
  std::vector<std::string> args;
  while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
    std::string value;
    if (!(input >> std::quoted(value, '"', '\0'))) throw Error(ErrorCode::InvalidArgument, "Unclosed quoted argument");
    args.push_back(std::move(value));
  }
  return args;
}

} // namespace

int csv_import::preview::Run(std::span<const fs::path> args) {
  try {
    const auto profile = args.empty() ? DefaultProfile() : args.front();
    PluginManager manager(profile, WorkerPath());
    manager.RegisterExtensionPoint(Importers);
    if (args.size() > 1) {
      std::vector<std::string> command;
      for (const auto& arg : args.subspan(1)) command.push_back(Utf8(arg));
      Report(manager, command);
    } else {
      if (args.empty()) std::cerr << "Enter one command per line.\n" << CommandHelp << '\n';
      for (std::string line; std::getline(std::cin, line);) {
        try {
          const auto command = Split(line);
          if (command.empty()) continue;
          if (command.size() == 1 && command.front() == "quit") break;
          Report(manager, command);
        } catch (const std::exception& error) {
          PrintError(error);
        }
      }
    }
  } catch (const std::exception& error) {
    PrintError(error);
    return 1;
  }
  return 0;
}
