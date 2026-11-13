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
  bool is_const;
  bool reference;
  std::string receiver;
  std::string receiver_header;
};

CXType PluginKitArgument(CXType type, std::string_view name) {
  const auto canonical = clang_getCanonicalType(type);
  if (StandardTemplateName(clang_getTypeDeclaration(canonical)) == name &&
      !clang_isConstQualifiedType(canonical) && !clang_isVolatileQualifiedType(canonical)) {
    auto argument = clang_Type_getTemplateArgumentAsType(type, 0);
    if (argument.kind == CXType_Invalid) argument = clang_Type_getTemplateArgumentAsType(canonical, 0);
    return argument;
  }
  return CXType{CXType_Invalid, {}};
}

bool IsRemote(CXCursor cursor) {
  return std::ranges::any_of(Children(cursor), [](CXCursor child) {
    return child.kind == CXCursor_AnnotateAttr && Name(child) == "hpk.remote";
  });
}

class Schema {
public:
  explicit Schema(CXCursor interface) : interface_name(QualifiedName(interface)) {
    if (!IsRemote(interface)) Reject(interface, "Mark the interface class with HPK_REMOTE");
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
          !clang_CXXMethod_isPureVirtual(cursor) || count < 0 || count > 2 ||
          clang_Cursor_isVariadic(cursor) || clang_Type_getCXXRefQualifier(clang_getCursorType(cursor)) != CXRefQualifier_None) {
        Reject(cursor, "Expected a public pure virtual operation with at most one request and one receiver");
      }
      const auto exception_spec = clang_getCursorExceptionSpecificationType(cursor);
      if (exception_spec != CXCursor_ExceptionSpecificationKind_None) {
        Reject(cursor, "Remote operations must allow transport errors to propagate");
      }
      const auto name = Name(cursor);
      const auto method_type = Text(clang_getTypeSpelling(clang_getCursorType(cursor)));
      const auto qualifiers = std::string_view(method_type).substr(method_type.rfind(')') + 1);
      if (qualifiers.find("volatile") != std::string_view::npos) {
        Reject(cursor, "Remote operations cannot be volatile-qualified");
      }
      if (name.starts_with("operator") || name == "Extension" || name == "Callback" || name == "ServiceHandle" ||
          !names.insert(name).second) {
        Reject(cursor, "Operation names must be unique ordinary method names");
      }
      auto result = clang_getCursorResultType(cursor);
      const auto task_result = PluginKitArgument(result, "huxerui::Task");
      const bool async = task_result.kind != CXType_Invalid;
      if (async) result = task_result;
      Operation operation{name, {"void", "void"},
                          result.kind == CXType_Void ? Type{"void", "void"} : ReadType(result, cursor, 0),
                          async, clang_CXXMethod_isConst(cursor) != 0, false, {}, {}};
      if (count) {
        const auto argument = clang_Cursor_getArgument(cursor, 0);
        auto type = clang_getCursorType(argument);
        operation.reference = type.kind == CXType_LValueReference;
        if (operation.reference) {
          if (async) Reject(argument, "A coroutine operation must take its request by value");
          type = clang_getPointeeType(type);
          if (!clang_isConstQualifiedType(type) || clang_isVolatileQualifiedType(type)) {
            Reject(argument, "A borrowed request must be a const reference");
          }
        }
        operation.request = ReadType(clang_getUnqualifiedType(type), argument, 0);
      }
      if (count == 2) {
        const auto receiver = clang_Cursor_getArgument(cursor, 1);
        const auto receiver_type = PluginKitArgument(clang_getCursorType(receiver), "std::shared_ptr");
        const auto declaration = clang_getTypeDeclaration(clang_getCanonicalType(receiver_type));
        CXFile receiver_file = nullptr;
        CXFile interface_file = nullptr;
        clang_getSpellingLocation(clang_getCursorLocation(declaration), &receiver_file, nullptr, nullptr, nullptr);
        clang_getSpellingLocation(clang_getCursorLocation(interface), &interface_file, nullptr, nullptr, nullptr);
        if (!async || receiver_type.kind == CXType_Invalid || !receiver_file ||
            clang_File_isEqual(receiver_file, interface_file) || !clang_isCursorDefinition(declaration) || !IsRemote(declaration)) {
          Reject(receiver, "Use a by-value shared_ptr to an HPK_REMOTE receiver from a separate header on a Task operation");
        }
        for (const auto method : Children(declaration)) {
          if (method.kind == CXCursor_CXXMethod &&
              (PluginKitArgument(clang_getCursorResultType(method), "huxerui::Task").kind == CXType_Invalid ||
               clang_Cursor_getNumArguments(method) > 1)) {
            Reject(method, "A receiver operation must return Task and accept at most one request");
          }
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
      if (method.is_const) methods.back()["const"] = true;
      if (method.reference) methods.back()["const_ref"] = true;
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

std::string Signature(const Operation& operation) {
  const auto& result = operation.response.cpp;
  std::string signature = (operation.async ? "Task<" + result + ">" : result) + " " + operation.name + "(";
  if (operation.request.cpp != "void") {
    signature += (operation.reference ? "const " + operation.request.cpp + "&" : operation.request.cpp) + " request";
  }
  if (!operation.receiver.empty()) signature += ", Receiver<" + operation.receiver + "> receiver";
  signature += operation.is_const ? ") const override" : ") override";
  return signature;
}

std::string Arguments(const Operation& operation) {
  std::string result;
  if (operation.request.cpp != "void") result = operation.reference ? "request" : "std::move(request)";
  if (!operation.receiver.empty()) result += ", std::move(receiver)";
  return result;
}

std::string CaptureRequest(const Operation& operation) {
  return operation.request.cpp == "void" ? "" : "request = std::move(request)";
}

std::string EncodedRequest(const Operation& operation) {
  return operation.request.cpp == "void" ? "Bytes{}" : "detail::Encode(request)";
}

void EmitOperationDocumentation(std::ostream& out, const Operation& operation, std::string_view action) {
  out << "  /// @brief " << action << " `" << operation.name << "()`"
      << (operation.async ? " when the Task runs.\n" : " synchronously.\n");
  if (operation.request.cpp != "void") {
    out << "  /// @param request ";
    if (operation.async) out << "Request value owned by the returned Task.\n";
    else if (operation.reference) out << "Request value borrowed until the call returns.\n";
    else out << "Request value passed to the implementation.\n";
  }
  if (!operation.receiver.empty()) {
    out << "  /// @param receiver Optional host callback object retained for this call.\n";
  }
  if (operation.async) {
    out << (operation.response.cpp == "void" ? "  /// @return Lazy Task that completes when the operation finishes.\n"
                                             : "  /// @return Lazy Task yielding the operation's result.\n");
  } else if (operation.response.cpp != "void") {
    out << "  /// @return Result returned by the published operation.\n";
  }
}

void EmitAsyncCallback(std::ostream& out, const Operation& operation, std::string_view type) {
  const auto empty = operation.request.cpp == "void";
  const auto qualifier = empty ? "[[maybe_unused]] " : "";
  out << ", " << (empty ? "nullptr" : "std::move(request)") << ",\n"
      << "        [](" << type << "& target, " << qualifier << "auto request) { return target."
      << operation.name << "(" << Arguments(operation) << "); },\n"
      << "        [](" << qualifier << "const auto& request) { return " << EncodedRequest(operation) << "; });\n";
}

void EmitExtension(std::ostream& out, const Schema& schema, std::string_view type) {
  out << "/// @brief Adapts the published interface to its installed execution mode.\n"
      << "///\n"
      << "/// Call through ExtensionHandle on the owning HuxerUI application thread. In-process calls use\n"
      << "/// the native C++ implementation. Worker calls encode the request and response over IPC.\n"
      << "/// @see PluginManager::CreateExtension()\n"
      << "class Contract<" << type << ">::Extension final : public " << type << " {\npublic:\n"
      << "  /// @brief Binds the interface adapter to the handle's invocation target.\n"
      << "  /// @param target Target created by PluginManager for the approved execution mode.\n"
      << "  explicit Extension(std::shared_ptr<detail::ExtensionTarget> target) : target_(std::move(target)) {}\n\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    const auto& result = operation.response.cpp;
    const auto arguments = Arguments(operation);
    EmitOperationDocumentation(out, operation, "Invokes the extension's");
    out << "  /// @throws Error With Closed for a released or retired target, DeadlineExceeded if its time budget\n"
        << "  /// expires, or PluginFailed if the implementation fails.\n"
        << "  /// @throws Error With WorkerExited if the worker exits, Protocol for a malformed exchange, or\n"
        << "  /// PayloadTooLarge if the encoded request or response exceeds a wire limit.\n";
    if (operation.async) {
      out << "  /// @note A queued timeout or cancellation affects only this call. An active worker exchange\n"
          << "  /// is retired on timeout or cancellation. In process, PluginKit requests cooperative stop.\n";
    } else {
      out << "  /// @throws Error With Busy if the plugin is handling another invocation, or CallbackUnavailable\n"
          << "  /// if a synchronous worker call attempts to call the host.\n"
          << "  /// @note A synchronous worker call blocks until its reply or failure. The in-process time budget\n"
          << "  /// is checked at call boundaries.\n";
    }
    out << "  " << Signature(operation) << " {\n"
        << "    if (target_->Mode() == ExecutionMode::InProcess) {\n";
    if (operation.async) {
      out << "      return detail::InvokeLocal<" << result << ">(target_, [" << CaptureRequest(operation)
          << "](void* object, " << (operation.receiver.empty() ? "[[maybe_unused]] " : "")
          << "std::shared_ptr<detail::LocalInvocation> invocation";
      if (result != "void") out << ", auto result";
      out << ") mutable {\n";
      if (!operation.receiver.empty()) {
        out << "        auto receiver = invocation->receiver ? std::make_shared<Contract<" << operation.receiver
            << ">::Callback>(invocation) : nullptr;\n";
      }
      const auto call = "static_cast<" + std::string(type) + "*>(object)->" + operation.name + "(" + arguments + ")";
      if (result == "void") out << "        return " << call << ";\n";
      else out << "        return detail::StoreLocalResult<" << result << ">(" << call << ", result);\n";
      out << "      }" << (operation.receiver.empty() ? "" : ", receiver") << ");\n";
    } else {
      out << "      return detail::InvokeLocalSync<" << result << ">(*target_, [&](void* object) {\n"
          << "        return static_cast<" << type << "*>(object)->" << operation.name << "(" << arguments << ");\n"
          << "      });\n";
    }
    out << "    }\n";
    if (operation.async) {
      out << "    return detail::InvokeWorker<" << result << ">(target_, " << i + 1 << ", [" << CaptureRequest(operation)
          << "] { return " << EncodedRequest(operation) << "; }";
      if (!operation.receiver.empty()) out << ", detail::BindReceiver(std::move(receiver))";
    } else {
      out << "    return detail::DecodeResult<" << result << ">(target_->CallWorker(" << i + 1 << ", "
          << EncodedRequest(operation) << ")";
    }
    out << ");\n  }\n\n";
  }
  out << "private:\n  std::shared_ptr<detail::ExtensionTarget> target_;\n};\n\n";
}

void EmitCallback(std::ostream& out, const Schema& schema, std::string_view type) {
  out << "/// @brief Typed receiver for a single local or worker invocation.\n"
      << "///\n"
      << "/// Await operations from the owning invocation or an awaited child Task. The adapter expires when\n"
      << "/// the invocation finishes or is cancelled.\n"
      << "class Contract<" << type << ">::Callback final : public " << type << " {\npublic:\n"
      << "  /// @brief Binds a worker receiver to the current invocation.\n"
      << "  /// @param worker Host endpoint carrying the invocation identity and receiver slot.\n"
      << "  explicit Callback(detail::HostEndpoint worker) : worker_(worker) {}\n"
      << "  /// @brief Binds an in-process receiver to the current invocation.\n"
      << "  /// @param local Weak reference to the invocation that owns the host receiver.\n"
      << "  explicit Callback(std::weak_ptr<detail::LocalInvocation> local) : local_(std::move(local)) {}\n\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    const auto& result = operation.response.cpp;
    EmitOperationDocumentation(out, operation, "Calls the host receiver's");
    out << "  /// @throws Error With ReceiverClosed if the receiver expired or the Task runs outside its\n"
        << "  /// owning invocation. The Task also reports failures from the host callback.\n";
    out << "  " << Signature(operation) << " {\n";
    out << "    return detail::InvokeReceiver<" << result << ", " << type << ">(local_, worker_, " << i + 1;
    EmitAsyncCallback(out, operation, type);
    out << "  }\n\n";
  }
  out << "private:\n  std::optional<detail::HostEndpoint> worker_;\n  std::weak_ptr<detail::LocalInvocation> local_;\n};\n\n";
}

std::string EmitBindings(const Schema& schema, std::string_view header, std::string_view id, std::uint32_t version,
                         std::string_view fingerprint) {
  std::ostringstream out;
  out << "#pragma once\n\n"
      << "/// @file\n/// @brief Generated bindings shared by the host and the plugin.\n"
      << "///\n/// Include this header in plugin code that implements or calls the published interface.\n\n"
      << "#include \"" << header << "\"\n#include <huxerui/pluginkit/detail/extension.h>\n";
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
      << "  /// Exact contract identifier, version, and schema fingerprint used by the host and plugin.\n"
      << "  static constexpr ContractInfo Info{\"" << id << "\", " << version << ", \"" << fingerprint << "\"};\n\n"
      << "  /// Generated implementation used by ExtensionHandle.\n"
      << "  class Extension;\n";
  const auto receiver_contract = receiver_headers.empty() && std::ranges::all_of(schema.operations, &Operation::async);
  if (receiver_headers.empty()) {
    out << "  /// This interface has no receiver parameters and can be registered as a host service.\n"
        << "  static constexpr bool HostService = true;\n";
  }
  if (receiver_contract) {
    out << "  /// Generated implementation passed to a plugin as an invocation-scoped receiver.\n"
        << "  class Callback;\n";
  }
  out << "\n"
      << "  /// @brief Decodes the request and invokes the selected operation.\n"
      << "  /// @param implementation Instance that implements the interface. Keep it alive until the Task completes.\n"
      << "  /// @param operation Operation number from the published contract.\n"
      << "  /// @param payload Encoded request owned by the task.\n"
      << "  /// @param context Invocation data used to bind a receiver, when supplied.\n"
      << "  /// @return Lazy Task yielding the encoded result, or an empty buffer for a void result.\n"
      << "  /// @throws Error With Protocol for an unknown operation or invalid payload, or PayloadTooLarge\n"
      << "  /// if a wire limit is exceeded. Failures from the implementation also propagate.\n"
      << "  static Task<Bytes> Dispatch(" << type << "& implementation, std::uint32_t operation, Bytes payload,\n"
      << "                              [[maybe_unused]] detail::DispatchContext context) {\n"
      << "    switch (operation) {\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    out << "      case " << i + 1 << ": {\n";
    if (operation.request.cpp == "void") {
      out << "        if (!payload.empty()) throw Error(ErrorCode::Protocol, \"Unexpected request payload\");\n";
    }
    const bool returns_value = operation.response.cpp != "void";
    out << "        ";
    if (returns_value) out << "auto result = ";
    if (operation.async) out << "co_await ";
    out << "implementation." << operation.name << "(";
    if (operation.request.cpp != "void") out << "detail::Decode<" << operation.request.cpp << ">(payload)";
    if (!operation.receiver.empty()) {
      out << ", context.receiver ? std::make_shared<Contract<" << operation.receiver
          << ">::Callback>(detail::HostEndpoint(context.invocation, context.receiver)) : nullptr";
    }
    out << ");\n        co_return " << (returns_value ? "detail::Encode(result)" : "Bytes{}") << ";\n      }\n";
  }
  out << "      default: throw Error(ErrorCode::Protocol, \"Unknown contract operation\");\n"
      << "    }\n  }\n};\n\n";
  EmitExtension(out, schema, type);
  if (receiver_contract) EmitCallback(out, schema, type);
  if (receiver_headers.empty()) {
    out << "/// @brief Calls a granted host service through its published C++ interface.\n"
        << "///\n"
        << "/// Each method resolves the current plugin invocation and checks its service grant. Task methods\n"
        << "/// work in both execution modes. Synchronous methods require a synchronous in-process call.\n"
        << "/// @see GetService(), PluginManager::ProvideService()\n"
        << "template<>\nclass ServiceHandle<" << type << "> final : public " << type << " {\npublic:\n"
        << "  /// @brief Exposes the service methods for ordinary member access.\n"
        << "  /// @return Borrowed pointer to this adapter, valid for the lifetime of the handle.\n"
        << "  ServiceHandle* operator->() noexcept { return this; }\n\n";
    for (std::size_t i = 0; i < schema.operations.size(); ++i) {
      const auto& operation = schema.operations[i];
      const auto& result = operation.response.cpp;
      EmitOperationDocumentation(out, operation, "Calls the host service's");
      out << "  /// @throws Error With ServiceUnavailable outside a managed invocation, ServiceDenied without\n"
          << "  /// a grant, or ContractMismatch for a different service contract.\n";
      if (!operation.async) {
        out << "  /// @throws Error With CallbackUnavailable if called from a managed plugin Task.\n";
      }
      out << "  " << Signature(operation) << " {\n";
      if (operation.async) {
        out << "    return detail::InvokeService<" << result << ", " << type << ">(" << i + 1;
        EmitAsyncCallback(out, operation, type);
      } else {
        out << "    return detail::InvokeServiceSync<" << result << ", " << type << ">([&](" << type
            << "& target) { return target." << operation.name << "(" << Arguments(operation) << "); });\n";
      }
      out << "  }\n\n";
    }
    out << "};\n\n";
  }
  out << "} // namespace huxerui::pluginkit\n";
  return out.str();
}

std::string EmitHost(std::string_view bindings) {
  std::ostringstream out;
  out << "#pragma once\n\n"
      << "/// @file\n/// @brief Generated host entry point for the published plugin contract.\n"
      << "///\n/// Include this header to register the contract with PluginManager and create typed handles.\n\n"
      << "#include \"" << bindings << "\"\n#include <huxerui/pluginkit.h>\n";
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
      "-resource-dir=" HPK_CLANG_RESOURCE_DIR, "-DHPK_GENERATOR=1"};
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
  WriteAtomicFile(output / host, AsBytes(EmitHost(PathText(bindings))));
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
