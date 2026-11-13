#include "digest.h"

#include <clang-c/Index.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using huxerui::pluginkit::Error;
using huxerui::pluginkit::ErrorCode;
using namespace huxerui::pluginkit::detail;

std::string Text(CXString value) {
  const auto* characters = clang_getCString(value);
  std::string result = characters ? characters : "";
  clang_disposeString(value);
  return result;
}

std::vector<CXCursor> Children(CXCursor parent) {
  struct Visit {
    std::vector<CXCursor> children;
    std::exception_ptr error;
  } visit;
  clang_visitChildren(parent, [](CXCursor cursor, CXCursor, CXClientData data) noexcept {
    auto& visit = *static_cast<Visit*>(data);
    try {
      visit.children.push_back(cursor);
      return CXChildVisit_Continue;
    } catch (...) {
      visit.error = std::current_exception();
      return CXChildVisit_Break;
    }
  }, &visit);
  if (visit.error) std::rethrow_exception(visit.error);
  return std::move(visit.children);
}

std::string Name(CXCursor cursor) { return Text(clang_getCursorSpelling(cursor)); }

fs::path CursorPath(CXFile file) {
  const auto path = Text(clang_getFileName(file));
  return fs::path(std::u8string(path.begin(), path.end()));
}

std::string QualifiedName(CXCursor cursor) {
  auto name = Name(cursor);
  for (auto parent = clang_getCursorSemanticParent(cursor); parent.kind == CXCursor_Namespace;
       parent = clang_getCursorSemanticParent(parent)) {
    name = Name(parent) + "::" + name;
  }
  return name;
}

std::string StandardTemplateName(CXCursor declaration) {
  auto scope = clang_getCursorSemanticParent(declaration);
  while (scope.kind == CXCursor_Namespace && clang_Cursor_isInlineNamespace(scope)) {
    scope = clang_getCursorSemanticParent(scope);
  }
  if (scope.kind == CXCursor_Namespace && QualifiedName(scope) == "std") {
    return "std::" + Name(declaration);
  }
  return QualifiedName(declaration);
}

bool InHeader(CXCursor cursor) { return clang_Location_isFromMainFile(clang_getCursorLocation(cursor)) != 0; }

[[noreturn]] void Reject(CXCursor cursor, std::string_view message) {
  CXFile file = nullptr;
  unsigned line = 0;
  unsigned column = 0;
  clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, &line, &column, nullptr);
  const auto name = file ? Text(clang_getFileName(file)) : "contract";
  throw Error(ErrorCode::InvalidArgument, name + ":" + std::to_string(line) + ":" + std::to_string(column) +
              ": " + std::string(message));
}

CXCursor FindInterface(CXCursor parent, std::string_view name) {
  for (const auto cursor : Children(parent)) {
    if (!InHeader(cursor)) continue;
    if (cursor.kind == CXCursor_Namespace) {
      const auto nested = FindInterface(cursor, name);
      if (!clang_Cursor_isNull(nested)) return nested;
    } else if ((cursor.kind == CXCursor_ClassDecl || cursor.kind == CXCursor_StructDecl) &&
               clang_isCursorDefinition(cursor) && QualifiedName(cursor) == name) {
      return cursor;
    }
  }
  return clang_getNullCursor();
}

struct Type {
  std::string cpp;
  std::string wire;
};

struct Field {
  std::string name;
  Type type;
};

struct Record {
  std::string name;
  std::vector<Field> fields;
};

struct Operation {
  std::string name;
  Type request;
  Type response;
  bool async;
  std::string receiver;
  std::string receiver_header;
};

CXType PluginKitArgument(CXType type, std::string_view name) {
  const auto canonical = clang_getCanonicalType(type);
  if (QualifiedName(clang_getTypeDeclaration(canonical)) == name &&
      !clang_isConstQualifiedType(canonical) && !clang_isVolatileQualifiedType(canonical)) {
    auto argument = clang_Type_getTemplateArgumentAsType(type, 0);
    if (argument.kind == CXType_Invalid) argument = clang_Type_getTemplateArgumentAsType(canonical, 0);
    return argument;
  }
  return CXType{CXType_Invalid, {}};
}

class Schema {
public:
  explicit Schema(CXCursor interface) : interface_name(QualifiedName(interface)) {
    bool destructor = false;
    std::set<std::string> names;
    for (const auto cursor : Children(interface)) {
      if (cursor.kind == CXCursor_CXXAccessSpecifier || clang_isAttribute(cursor.kind)) continue;
      if (cursor.kind == CXCursor_Destructor && clang_getCXXAccessSpecifier(cursor) == CX_CXXPublic &&
          clang_CXXMethod_isVirtual(cursor)) {
        destructor = true;
        continue;
      }
      const auto count = clang_Cursor_getNumArguments(cursor);
      if (cursor.kind != CXCursor_CXXMethod || clang_getCXXAccessSpecifier(cursor) != CX_CXXPublic ||
          !clang_CXXMethod_isPureVirtual(cursor) || count < 1 || count > 2 ||
          clang_Cursor_isVariadic(cursor) || clang_Type_getCXXRefQualifier(clang_getCursorType(cursor)) != CXRefQualifier_None) {
        Reject(cursor, "Expected a public pure virtual operation with one request and an optional receiver");
      }
      const auto name = Name(cursor);
      if (name.starts_with("operator") || name == "ExtensionHandle" || name == "ServiceHandle" ||
          name == "Receiver" || !names.insert(name).second) {
        Reject(cursor, "Operation names must be unique ordinary method names");
      }
      const auto argument = clang_Cursor_getArgument(cursor, 0);
      auto type = clang_getCursorType(argument);
      auto result = clang_getCursorResultType(cursor);
      const auto task_result = PluginKitArgument(result, "huxerui::Task");
      const bool async = task_result.kind != CXType_Invalid;
      if (async) result = task_result;
      if (type.kind == CXType_LValueReference) {
        if (async) Reject(argument, "A coroutine operation must take its request by value");
        type = clang_getPointeeType(type);
        if (!clang_isConstQualifiedType(type) || clang_isVolatileQualifiedType(type)) {
          Reject(argument, "A borrowed request must be a const reference");
        }
        type = clang_getUnqualifiedType(type);
      }
      Operation operation{name, ReadType(type, argument, 0),
                          result.kind == CXType_Void ? Type{"void", "void"} : ReadType(result, cursor, 0),
                          async, {}, {}};
      if (count == 2) {
        const auto receiver = clang_Cursor_getArgument(cursor, 1);
        const auto receiver_type = PluginKitArgument(clang_getCursorType(receiver), "huxerui::pluginkit::Receiver");
        const auto declaration = clang_getTypeDeclaration(clang_getCanonicalType(receiver_type));
        CXFile receiver_file = nullptr;
        CXFile interface_file = nullptr;
        clang_getSpellingLocation(clang_getCursorLocation(declaration), &receiver_file, nullptr, nullptr, nullptr);
        clang_getSpellingLocation(clang_getCursorLocation(interface), &interface_file, nullptr, nullptr, nullptr);
        if (!async || receiver_type.kind == CXType_Invalid || !receiver_file ||
            clang_File_isEqual(receiver_file, interface_file) || !clang_isCursorDefinition(declaration)) {
          Reject(receiver, "Use a by-value huxerui::pluginkit::Receiver<Interface> from a separate contract header on a Task operation");
        }
        auto binding = CursorPath(receiver_file).lexically_relative(CursorPath(interface_file).parent_path());
        if (binding.empty()) Reject(receiver, "Could not locate the receiver contract relative to this header");
        binding.replace_extension(".bindings.h");
        operation.receiver = "::" + QualifiedName(declaration);
        const auto relative = binding.generic_u8string();
        operation.receiver_header.assign(relative.begin(), relative.end());
      }
      operations.push_back(std::move(operation));
    }
    if (!destructor || operations.empty()) Reject(interface, "The interface needs a public virtual destructor and operations");
  }

  Json Describe(std::string_view id, std::uint32_t version) const {
    auto methods = Json::array();
    for (const auto& method : operations) {
      methods.push_back({{"number", methods.size() + 1}, {"name", method.name},
                         {"request", method.request.wire}, {"response", method.response.wire}});
      if (method.async) methods.back()["async"] = true;
      if (!method.receiver.empty()) methods.back()["receiver"] = method.receiver;
    }
    auto types = Json::array();
    for (const auto& record : records) {
      auto fields = Json::array();
      for (const auto& field : record.fields) {
        fields.push_back({{"number", fields.size() + 1}, {"name", field.name}, {"type", field.type.wire}});
      }
      types.push_back({{"name", record.name}, {"fields", std::move(fields)}});
    }
    return {{"id", id}, {"version", version}, {"operations", std::move(methods)}, {"records", std::move(types)}};
  }

  std::string interface_name;
  std::vector<Record> records;
  std::vector<Operation> operations;

private:
  Type ReadType(CXType type, CXCursor origin, unsigned depth) {
    if (depth >= 16) Reject(origin, "The wire type exceeds the nesting limit");
    if (clang_isConstQualifiedType(type) || clang_isVolatileQualifiedType(type)) {
      Reject(origin, "Wire values must be unqualified");
    }
    const auto spelling = Text(clang_getTypeSpelling(type));
    if (spelling == "bool") return {"bool", "bool"};
    if (spelling == "std::string") return {"std::string", "text"};
    for (const auto integer : {"int32_t", "uint32_t", "int64_t", "uint64_t"}) {
      if (spelling == integer || spelling == std::string("std::") + integer) {
        return {std::string("std::") + integer, integer};
      }
    }

    const auto canonical = clang_getCanonicalType(type);
    const auto declaration = clang_getTypeDeclaration(canonical);
    const auto name = StandardTemplateName(declaration);
    if (name == "std::vector" || name == "std::optional") {
      auto element = clang_Type_getTemplateArgumentAsType(type, 0);
      if (element.kind == CXType_Invalid) element = clang_Type_getTemplateArgumentAsType(canonical, 0);
      if (name == "std::vector") {
        const auto allocator = clang_Type_getTemplateArgumentAsType(canonical, 1);
        if (StandardTemplateName(clang_getTypeDeclaration(allocator)) != "std::allocator") {
          Reject(origin, "A vector must use the standard allocator");
        }
      }
      const auto nested = ReadType(element, origin, depth + 1);
      return {name + "<" + nested.cpp + ">", name.substr(5) + "<" + nested.wire + ">"};
    }
    if (canonical.kind == CXType_Record && declaration.kind == CXCursor_StructDecl && InHeader(declaration)) {
      const auto record = QualifiedName(declaration);
      ReadRecord(declaration, record, depth);
      return {"::" + record, record};
    }
    Reject(origin, "Unsupported wire type '" + spelling + "'");
  }

  void ReadRecord(CXCursor declaration, const std::string& name, unsigned depth) {
    const auto parent = clang_getCursorSemanticParent(declaration);
    if (!clang_isCursorDefinition(declaration) || name.empty() || name.starts_with("::") ||
        (parent.kind != CXCursor_TranslationUnit && parent.kind != CXCursor_Namespace)) {
      Reject(declaration, "Use a named record defined at namespace scope in the contract header");
    }
    if (std::ranges::find(records, name, &Record::name) != records.end()) return;
    if (!visiting_.insert(name).second) Reject(declaration, "Recursive records are unsupported");
    Record record{name, {}};
    for (const auto cursor : Children(declaration)) {
      if (cursor.kind == CXCursor_CXXAccessSpecifier || clang_isAttribute(cursor.kind)) continue;
      if (cursor.kind != CXCursor_FieldDecl || clang_getCXXAccessSpecifier(cursor) != CX_CXXPublic ||
          clang_Cursor_isBitField(cursor) || Name(cursor).empty()) {
        Reject(cursor, "Records may contain only named public fields");
      }
      record.fields.push_back({Name(cursor), ReadType(clang_getCursorType(cursor), cursor, depth + 1)});
    }
    records.push_back(std::move(record));
    visiting_.erase(name);
  }

  std::set<std::string> visiting_;
};

std::string EmitBindings(const Schema& schema, std::string_view header, std::string_view id, std::uint32_t version,
                         std::string_view fingerprint) {
  std::ostringstream out;
  out << "#pragma once\n\n"
      << "/// @file\n/// @brief Generated bindings shared by the host and the plugin.\n\n"
      << "#include \"" << header << "\"\n#include <huxerui/pluginkit/detail/invocation.h>\n";
  std::set<std::string> receiver_headers;
  for (const auto& operation : schema.operations) {
    if (!operation.receiver_header.empty()) receiver_headers.insert(operation.receiver_header);
  }
  for (const auto& receiver : receiver_headers) out << "#include \"" << receiver << "\"\n";
  out << "\nnamespace huxerui::pluginkit::detail {\n\n";
  for (const auto& record : schema.records) {
    const auto type = "::" + record.name;
    out << "template<>\nstruct Codec<" << type << "> {\n"
        << "  static void Write(Writer& writer, const " << type << "& value) {\n"
        << "    writer.Array(" << record.fields.size() << ");\n";
    if (record.fields.empty()) out << "    (void)value;\n";
    for (const auto& field : record.fields) {
      out << "    Codec<" << field.type.cpp << ">::Write(writer, value." << field.name << ");\n";
    }
    out << "  }\n\n  static " << type << " Read(Reader& reader) {\n"
        << "    reader.Record(" << record.fields.size() << ");\n    return {";
    for (std::size_t i = 0; i < record.fields.size(); ++i) {
      if (i != 0) out << ", ";
      out << "Codec<" << record.fields[i].type.cpp << ">::Read(reader)";
    }
    out << "};\n  }\n};\n\n";
  }
  const auto type = "::" + schema.interface_name;
  out << "} // namespace huxerui::pluginkit::detail\n\nnamespace huxerui::pluginkit {\n\n"
      << "/// @brief Generated contract identity and operation dispatcher for " << schema.interface_name << ".\n"
      << "template<>\nstruct Contract<" << type << "> {\n"
      << "  /// Exact contract identity shared by the host and plugin.\n"
      << "  static constexpr ContractInfo Info{\"" << id << "\", " << version << ", \"" << fingerprint << "\"};\n\n"
      << "  /// @brief Decodes the request and invokes the selected operation.\n"
      << "  /// @param implementation Plugin instance that implements the interface.\n"
      << "  /// @param operation Operation number from the published contract.\n"
      << "  /// @param payload Encoded request owned by the task.\n"
      << "  /// @param context Invocation data for a call that supplies a receiver.\n"
      << "  /// @return Task that produces the encoded result.\n"
      << "  /// @throws Error With Protocol if the operation or request payload is invalid.\n"
      << "  static Task<Bytes> Dispatch(" << type << "& implementation, std::uint32_t operation, Bytes payload,\n"
      << "                              [[maybe_unused]] detail::DispatchContext context) {\n"
      << "    switch (operation) {\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    out << "      case " << i + 1 << ": {\n";
    if (!operation.receiver.empty()) {
      out << "        if (!context.receiver || !context.invocation.host) {\n"
          << "          throw Error(ErrorCode::Protocol, \"Missing invocation receiver\");\n        }\n";
    }
    const bool returns_value = operation.response.cpp != "void";
    out << "        ";
    if (returns_value) out << "auto result = ";
    if (operation.async) out << "co_await ";
    out << "implementation." << operation.name << "(detail::Decode<" << operation.request.cpp << ">(payload)";
    if (!operation.receiver.empty()) {
      out << ", Receiver<" << operation.receiver << ">(detail::HostEndpoint(context.invocation, context.receiver))";
    }
    out << ");\n        co_return " << (returns_value ? "detail::Encode(result)" : "Bytes{}") << ";\n      }\n";
  }
  out << "      default: throw Error(ErrorCode::Protocol, \"Unknown contract operation\");\n"
      << "    }\n  }\n};\n\n";
  if (receiver_headers.empty()) {
    for (const auto proxy : {"ServiceHandle", "Receiver"}) {
      out << "/// @brief Invocation-scoped proxy for " << schema.interface_name << " on the host.\n"
          << "template<>\nclass " << proxy << "<" << type << "> {\npublic:\n"
          << "  /// @brief Binds the proxy to its owning invocation.\n"
          << "  explicit " << proxy << "(detail::HostEndpoint endpoint) : endpoint_(endpoint) {}\n\n";
      for (std::size_t i = 0; i < schema.operations.size(); ++i) {
        const auto& operation = schema.operations[i];
        out << "  /// @brief Invokes " << operation.name << " on the host when awaited.\n"
            << "  /// @param request Value to encode for the host.\n"
            << "  /// @return Task that completes after the host replies.\n"
            << "  /// @throws Error If the proxy is expired, unauthorized, or the call fails.\n"
            << "  Task<" << operation.response.cpp << "> " << operation.name << "(const " << operation.request.cpp
            << "& request) const {\n"
            << "    return detail::InvokeHost<" << operation.response.cpp << ">(endpoint_, Contract<" << type
            << ">::Info, " << i + 1 << ", detail::Encode(request));\n  }\n\n";
      }
      out << "private:\n  detail::HostEndpoint endpoint_;\n};\n\n";
    }
  }
  out << "} // namespace huxerui::pluginkit\n";
  return out.str();
}

std::string EmitHost(const Schema& schema, std::string_view bindings) {
  std::ostringstream out;
  out << "#pragma once\n\n"
      << "/// @file\n/// @brief Generated host API for the published plugin contract.\n\n"
      << "#include \"" << bindings << "\"\n#include <huxerui/pluginkit.h>\n\n"
      << "namespace huxerui::pluginkit {\n\n"
      << "/// @brief Move-only handle to one instance of " << schema.interface_name << " in the plugin worker.\n"
      << "///\n"
      << "/// Calls share the worker's serialized request queue. Coordinate moves, Close(), and destruction\n"
      << "/// with all calls on this handle. Destruction attempts to close the instance and suppresses close errors.\n"
      << "/// @see PluginManager::CreateExtension()\n"
      << "template<>\nclass ExtensionHandle<::" << schema.interface_name << "> {\npublic:\n"
      << "  /// @brief Takes ownership of an instance created by PluginManager.\n"
      << "  /// @param instance Instance to move into this handle.\n"
      << "  explicit ExtensionHandle(detail::ExtensionInstance instance) : instance_(std::move(instance)) {}\n\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    out << "  /// @brief Invokes " << operation.name << " in the plugin worker when the task runs.\n"
        << "  /// @param request Value encoded according to the contract.\n";
    if (!operation.receiver.empty()) out << "  /// @param receiver Host callback object retained for this invocation.\n";
    out << "  /// @param timeout Time budget from 1 ms through 24 hours, including time in the worker queue.\n"
        << "  /// @return Task that produces the decoded result.\n"
        << "  /// @throws Error With Closed if the handle or generation is closed, DeadlineExceeded if the request\n"
        << "  /// expires, or WorkerExited if the worker exits before the reply.\n"
        << "  /// @throws Error With PluginFailed if the implementation reports a failure.\n"
        << "  /// @note A queue timeout affects only this call. An in-flight timeout closes the worker generation\n"
        << "  /// and the operation may already have taken effect.\n"
        << "  Task<" << operation.response.cpp << ">"
        << " " << operation.name << "(const " << operation.request.cpp << "& request,\n";
    if (!operation.receiver.empty()) out << "      std::shared_ptr<" << operation.receiver << "> receiver,\n";
    out << "      std::chrono::milliseconds timeout = DefaultTimeout) const {\n";
    out << "    return detail::InvokeExtension<" << operation.response.cpp << ">(instance_.Call(), " << i + 1
        << ", detail::Encode(request), timeout";
    if (!operation.receiver.empty()) {
      out << ", detail::BindReceiver<" << operation.receiver << ">(std::move(receiver))";
    }
    out << ");\n";
    out << "  }\n\n";
  }
  out << "  /// @brief Closes the local handle and requests release of the instance from a live worker.\n"
      << "  /// @return Lazy task. Await it before moving or destroying this handle.\n"
      << "  /// @throws Error If the close request fails. A failed close also retires the worker generation.\n"
      << "  Task<void> Close() { return instance_.Close(); }\n\nprivate:\n  detail::ExtensionInstance instance_;\n};\n\n"
      << "} // namespace huxerui::pluginkit\n";
  return out.str();
}

void Generate(const fs::path& header, std::string_view interface_name, std::string_view id, std::uint32_t version,
              const fs::path& output, std::vector<std::string> options) {
  if (!IsId(id) || !version) throw Error(ErrorCode::InvalidArgument, "Invalid contract ID or version");
  const auto path = PathText(fs::absolute(header));
  const auto source = ReadFileBytes(header, 1024 * 1024);
  const auto text = AsText(source);
  // Parse the exact header bytes that will be copied into the SDK.
  CXUnsavedFile input{path.c_str(), text.data(), static_cast<unsigned long>(text.size())};
  std::vector<std::string> arguments{
      HPK_CLANG_EXECUTABLE, "-x", "c++", "-std=c++20", "--target=" HPK_CLANG_TARGET,
      "-resource-dir=" HPK_CLANG_RESOURCE_DIR};
#if defined(HPK_CLANG_SYSROOT)
  arguments.insert(arguments.end(), {"-isysroot", HPK_CLANG_SYSROOT});
#endif
  arguments.insert(arguments.end(), options.begin(), options.end());
  std::vector<const char*> argv;
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  std::unique_ptr<void, decltype(&clang_disposeIndex)> index(clang_createIndex(0, 0), clang_disposeIndex);
  CXTranslationUnit raw = nullptr;
  const auto status = clang_parseTranslationUnit2FullArgv(index.get(), path.c_str(), argv.data(),
      static_cast<int>(argv.size()), &input, 1, CXTranslationUnit_SkipFunctionBodies, &raw);
  std::unique_ptr<std::remove_pointer_t<CXTranslationUnit>, decltype(&clang_disposeTranslationUnit)> unit(
      raw, clang_disposeTranslationUnit);
  if (status != CXError_Success) throw Error(ErrorCode::InvalidArgument, "Clang could not open the contract header");
  bool failed = false;
  for (unsigned i = 0; i < clang_getNumDiagnostics(unit.get()); ++i) {
    const auto diagnostic = clang_getDiagnostic(unit.get(), i);
    if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
      std::cerr << Text(clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions())) << '\n';
      failed = true;
    }
    clang_disposeDiagnostic(diagnostic);
  }
  if (failed) throw Error(ErrorCode::InvalidArgument, "Clang rejected the contract header");
  const auto interface = FindInterface(clang_getTranslationUnitCursor(unit.get()), interface_name);
  if (clang_Cursor_isNull(interface)) throw Error(ErrorCode::InvalidArgument, "Interface definition was not found in the contract header");
  const Schema schema(interface);
  auto manifest = schema.Describe(id, version);
  // Hash the compact schema before adding its fingerprint to the published manifest.
  const auto fingerprint = Sha256(AsBytes(manifest.dump()));
  manifest["fingerprint"] = fingerprint;

  auto bindings = header.stem();
  bindings += ".bindings.h";
  auto host = header.stem();
  host += ".host.h";
  fs::create_directories(output);
  WriteAtomicFile(output / header.filename(), source);
  WriteAtomicFile(output / bindings, AsBytes(EmitBindings(schema, PathText(header.filename()), id, version, fingerprint)));
  WriteAtomicFile(output / host, AsBytes(EmitHost(schema, PathText(bindings))));
  WriteAtomicFile(output / "contract.json", AsBytes(manifest.dump(2)));
}

int Run(std::span<const fs::path> args) {
  if (args.size() < 7 || args[1] != "gen" || (args.size() > 7 && args[7] != "--")) {
    std::cerr << "Usage: hpk gen <header> <qualified-interface> <contract-id> <version> <sdk-dir> [-- <clang-options>]\n";
    return 1;
  }
  try {
    std::vector<std::string> options;
    for (std::size_t i = 8; i < args.size(); ++i) options.push_back(PathText(args[i]));
    Generate(args[2], PathText(args[3]), PathText(args[4]),
             ParseInteger<std::uint32_t>(PathText(args[5]), "contract version"), args[6], std::move(options));
  } catch (const std::exception& error) {
    std::cerr << "hpk: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  return Run(std::vector<fs::path>(argv, argv + argc));
}
