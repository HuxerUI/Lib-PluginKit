#include "digest.h"

#include <clang-c/Index.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
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

struct ValueCodec {
  std::string cpp;
  std::vector<Field> fields;
  std::optional<Type> representation;
};

enum class ParameterKind { Value, Stream, Receiver };

struct Parameter {
  ParameterKind kind;
  Type type;
  std::string name;
  bool reference = false;
  std::string receiver_header;
  std::string receiver_schema;
};

struct Operation {
  std::string name;
  Type response;
  std::vector<Parameter> parameters;
  bool async;
  bool is_const;

  bool HasStreams() const {
    return std::ranges::any_of(parameters, [](const auto& p) { return p.kind == ParameterKind::Stream; });
  }

  bool HasGrants() const {
    return std::ranges::any_of(parameters, [](const auto& p) { return p.kind != ParameterKind::Value; });
  }
};

std::string StreamType(CXType type) {
  const auto canonical = clang_getCanonicalType(type);
  if (canonical.kind != CXType_Record) return {};
  const auto name = QualifiedName(clang_getTypeDeclaration(canonical));
  for (const std::string_view stream : {"InputStream", "OutputStream", "AsyncInputStream", "AsyncOutputStream"}) {
    if (name == std::string("huxerui::") + std::string(stream)) return std::string(stream);
  }
  return {};
}

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

struct CodecDefinition {
  CXType value;
  CXCursor declaration;
};

struct StandardValue {
  std::string_view name;
  std::string_view wire;
};

constexpr std::array<StandardValue, 5> StandardValues{{
    {"int32_t", "int32_t"}, {"uint32_t", "uint32_t"}, {"int64_t", "int64_t"}, {"uint64_t", "uint64_t"},
    {"string", "text"},
}};

struct WireDefinitions {
  std::vector<CodecDefinition> codecs;
  std::array<CXType, StandardValues.size()> values{};
};

void FindWireDefinitions(CXCursor scope, WireDefinitions& definitions) {
  for (const auto cursor : Children(scope)) {
    if (cursor.kind == CXCursor_Namespace || cursor.kind == CXCursor_LinkageSpec) {
      FindWireDefinitions(cursor, definitions);
    } else if ((cursor.kind == CXCursor_StructDecl || cursor.kind == CXCursor_ClassDecl) &&
               clang_isCursorDefinition(cursor) && Name(cursor) == "WireCodec") {
      const auto primary = clang_getSpecializedCursorTemplate(cursor);
      if (clang_Cursor_isNull(primary) || QualifiedName(primary) != "huxerui::pluginkit::WireCodec") continue;
      const auto value = clang_Type_getTemplateArgumentAsType(clang_getCursorType(cursor), 0);
      if (value.kind != CXType_Invalid) definitions.codecs.push_back({clang_getCanonicalType(value), cursor});
    } else if (cursor.kind == CXCursor_TypedefDecl || cursor.kind == CXCursor_TypeAliasDecl) {
      const auto name = Name(cursor);
      const auto value = std::ranges::find(StandardValues, name, &StandardValue::name);
      if (value == StandardValues.end()) continue;
      // Some standard libraries make the C integer typedefs visible in std with using declarations.
      const bool global_integer = name != "string" && clang_getCursorSemanticParent(cursor).kind == CXCursor_TranslationUnit &&
          clang_Location_isInSystemHeader(clang_getCursorLocation(cursor));
      if (global_integer || StandardTemplateName(cursor) == "std::" + name) {
        definitions.values[value - StandardValues.begin()] = clang_getCanonicalType(clang_getCursorType(cursor));
      }
    }
  }
}

CXFile SourceFile(CXCursor cursor) {
  CXFile file = nullptr;
  clang_getSpellingLocation(clang_getCursorLocation(cursor), &file, nullptr, nullptr, nullptr);
  return file;
}

Json DescribeRecord(const ValueCodec& record) {
  auto fields = Json::array();
  for (const auto& field : record.fields) {
    fields.push_back({{"number", fields.size() + 1}, {"name", field.name}, {"type", field.type.wire}});
  }
  return {{"name", record.cpp.substr(2)}, {"fields", std::move(fields)}};
}

std::optional<Type> BuiltinWireType(CXType type, const WireDefinitions& definitions) {
  if (type.kind == CXType_Bool) return Type{"bool", "bool"};
  for (std::size_t i = 0; i < StandardValues.size(); ++i) {
    if (definitions.values[i].kind != CXType_Invalid && clang_equalTypes(type, definitions.values[i])) {
      const auto& value = StandardValues[i];
      return Type{"std::" + std::string(value.name), std::string(value.wire)};
    }
  }
  return std::nullopt;
}

class Schema {
public:
  Schema(CXCursor interface, const WireDefinitions& definitions, bool receiver = false)
      : interface_name(QualifiedName(interface)), definitions_(definitions) {
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
          !clang_CXXMethod_isPureVirtual(cursor) || count < 0 ||
          clang_Cursor_isVariadic(cursor) || clang_Type_getCXXRefQualifier(clang_getCursorType(cursor)) != CXRefQualifier_None) {
        Reject(cursor, "Expected a public pure virtual operation with a typed parameter list");
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
      if (receiver && !async) Reject(cursor, "An invocation callback operation must return Task");
      if (async) result = task_result;
      Operation operation{name, clang_getCanonicalType(result).kind == CXType_Void ? Type{"void", "void"} : ReadType(result, cursor, 0),
                          {}, async, clang_CXXMethod_isConst(cursor) != 0};
      for (int index = 0; index < count; ++index) {
        operation.parameters.push_back(ReadParameter(clang_Cursor_getArgument(cursor, index), interface, async, receiver));
      }
      operations.push_back(std::move(operation));
    }
    if (!destructor || operations.empty()) Reject(interface, "The interface needs a public virtual destructor and operations");
  }

  Json Describe(std::string_view id, std::uint32_t version) const {
    auto methods = Json::array();
    for (const auto& method : operations) {
      auto parameters = Json::array();
      for (const auto& parameter : method.parameters) {
        const auto kind = parameter.kind == ParameterKind::Stream ? "stream"
            : parameter.kind == ParameterKind::Receiver ? "receiver" : "value";
        Json item{{"name", parameter.name}, {"kind", kind}, {"type", parameter.type.wire}};
        if (parameter.reference) item["const_ref"] = true;
        if (!parameter.receiver_schema.empty()) item["schema"] = parameter.receiver_schema;
        parameters.push_back(std::move(item));
      }
      methods.push_back({{"number", methods.size() + 1}, {"name", method.name},
                         {"parameters", std::move(parameters)}, {"response", method.response.wire}});
      if (method.async) methods.back()["async"] = true;
      if (method.is_const) methods.back()["const"] = true;
    }
    auto types = Json::array();
    for (const auto& codec : value_codecs) {
      if (!codec.representation) types.push_back(DescribeRecord(codec));
    }
    return {{"id", id}, {"version", version}, {"operations", std::move(methods)}, {"records", std::move(types)}};
  }

  std::string interface_name;
  std::vector<ValueCodec> value_codecs;
  std::vector<Operation> operations;

private:
  Parameter ReadParameter(CXCursor argument, CXCursor interface, bool async, bool receiver) {
    auto type = clang_getCursorType(argument);
    const auto canonical = clang_getCanonicalType(type);
    const auto stream = StreamType(type);
    if (!stream.empty()) {
      if (receiver) Reject(argument, "Invocation callbacks accept only ordinary values");
      if (clang_isConstQualifiedType(canonical) || clang_isVolatileQualifiedType(canonical) ||
          stream.starts_with("Async") != async) {
        Reject(argument, "Pass InputStream or OutputStream by value to synchronous methods and async streams to Task methods");
      }
      return {ParameterKind::Stream, {stream, stream}, Name(argument), false, {}, {}};
    }

    const auto callback = PluginKitArgument(type, "std::shared_ptr");
    if (callback.kind != CXType_Invalid) {
      const auto declaration = clang_getTypeDeclaration(clang_getCanonicalType(callback));
      const auto file = SourceFile(declaration);
      const auto interface_file = SourceFile(interface);
      if (receiver || !async || !file || !interface_file || clang_File_isEqual(file, interface_file) ||
          !clang_isCursorDefinition(declaration) || !IsRemote(declaration) ||
          clang_isConstQualifiedType(callback) || clang_isVolatileQualifiedType(callback)) {
        Reject(argument, "Use a by-value shared_ptr to an HPK_REMOTE callback from a separate header on a Task operation");
      }
      auto binding = CursorPath(file).lexically_relative(CursorPath(interface_file).parent_path());
      if (binding.empty()) Reject(argument, "Could not locate the callback contract relative to this header");
      binding.replace_extension(".bindings.h");
      const auto relative = binding.generic_u8string();
      const Schema reflected(declaration, definitions_, true);
      const auto shape = reflected.Describe(reflected.interface_name, 1).dump();
      return {ParameterKind::Receiver, {"::" + reflected.interface_name, reflected.interface_name}, Name(argument), false,
              std::string(relative.begin(), relative.end()), Sha256(AsBytes(shape))};
    }

    const auto reference = canonical.kind == CXType_LValueReference;
    if (reference) {
      if (async) Reject(argument, "A Task operation must take its arguments by value");
      type = clang_getPointeeType(canonical);
      if (!clang_isConstQualifiedType(type) || clang_isVolatileQualifiedType(type)) {
        Reject(argument, "A borrowed argument must be a const reference to an ordinary value");
      }
    }
    return {ParameterKind::Value, ReadType(clang_getUnqualifiedType(type), argument, 0), Name(argument), reference, {}, {}};
  }

  std::optional<Type> ReadMapping(CXType canonical, CXCursor origin, unsigned depth) {
    const bool named = canonical.kind == CXType_Record || canonical.kind == CXType_Enum;
    if (!named && (canonical.kind < CXType_Bool || canonical.kind > CXType_LongDouble)) return std::nullopt;
    const auto mapping = std::ranges::find_if(definitions_.codecs, [&](const auto& codec) {
      return clang_equalTypes(canonical, codec.value);
    });
    if (mapping == definitions_.codecs.end()) return std::nullopt;

    const auto name = Text(clang_getTypeSpelling(canonical));
    const auto cpp = named ? "::" + name : name;
    const auto found = std::ranges::find(value_codecs, cpp, &ValueCodec::cpp);
    if (found != value_codecs.end()) {
      return Type{cpp, "codec<" + name + "," + found->representation->wire + ">"};
    }

    const auto members = Children(mapping->declaration);
    const auto alias = std::ranges::find_if(members, [](CXCursor member) {
      return (member.kind == CXCursor_TypeAliasDecl || member.kind == CXCursor_TypedefDecl) && Name(member) == "WireType";
    });
    if (alias == members.end()) Reject(origin, "WireCodec for '" + name + "' must declare WireType");
    if (!visiting_.insert(cpp).second) Reject(origin, "Recursive WireCodec for '" + name + "'");
    auto wire = ReadType(clang_getTypedefDeclUnderlyingType(*alias), origin, depth + 1);
    Type result{cpp, "codec<" + name + "," + wire.wire + ">"};
    value_codecs.push_back({cpp, {}, std::move(wire)});
    visiting_.erase(cpp);
    return result;
  }

  Type ReadType(CXType type, CXCursor origin, unsigned depth) {
    if (depth >= 16) Reject(origin, "The wire type exceeds the nesting limit");
    const auto canonical = clang_getCanonicalType(type);
    if (clang_isConstQualifiedType(canonical) || clang_isVolatileQualifiedType(canonical)) {
      Reject(origin, "Wire values must be unqualified");
    }
    if (auto mapped = ReadMapping(canonical, origin, depth)) return std::move(*mapped);
    if (auto builtin = BuiltinWireType(canonical, definitions_)) return std::move(*builtin);
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
    if (canonical.kind == CXType_Record && declaration.kind == CXCursor_StructDecl &&
        clang_Cursor_isNull(clang_getSpecializedCursorTemplate(declaration))) {
      const auto record = QualifiedName(declaration);
      ReadRecord(declaration, record, depth);
      return {"::" + record, record};
    }
    Reject(origin, "Unsupported wire type '" + Text(clang_getTypeSpelling(type)) + "'");
  }

  void ReadRecord(CXCursor declaration, const std::string& name, unsigned depth) {
    const auto parent = clang_getCursorSemanticParent(declaration);
    if (!clang_isCursorDefinition(declaration) || name.empty() || name.starts_with("::") ||
        (parent.kind != CXCursor_TranslationUnit && parent.kind != CXCursor_Namespace)) {
      Reject(declaration, "Use a named record defined at namespace scope in a shared contract header");
    }
    const auto cpp = "::" + name;
    if (std::ranges::find(value_codecs, cpp, &ValueCodec::cpp) != value_codecs.end()) return;
    if (!visiting_.insert(cpp).second) Reject(declaration, "Recursive records are unsupported");
    ValueCodec record{cpp, {}, {}};
    for (const auto cursor : Children(declaration)) {
      if (cursor.kind == CXCursor_CXXAccessSpecifier || clang_isAttribute(cursor.kind)) continue;
      if (cursor.kind != CXCursor_FieldDecl || clang_getCXXAccessSpecifier(cursor) != CX_CXXPublic ||
          clang_Cursor_isBitField(cursor) || Name(cursor).empty()) {
        Reject(cursor, "Records may contain only named public fields");
      }
      record.fields.push_back({Name(cursor), ReadType(clang_getCursorType(cursor), cursor, depth + 1)});
    }
    value_codecs.push_back(std::move(record));
    visiting_.erase(cpp);
  }

  const WireDefinitions& definitions_;
  std::set<std::string> visiting_;
};

void AppendArgument(std::string& arguments, const std::string& value) {
  if (!arguments.empty()) arguments += ", ";
  arguments += value;
}

std::string ArgumentName(std::size_t index) { return "arg" + std::to_string(index + 1); }

std::string ParameterType(const Parameter& parameter) {
  if (parameter.kind == ParameterKind::Receiver) return "std::shared_ptr<" + parameter.type.cpp + ">";
  if (parameter.reference) return "const " + parameter.type.cpp + "&";
  return parameter.type.cpp;
}

std::string Signature(const Operation& operation) {
  const auto& result = operation.response.cpp;
  std::string parameters;
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    AppendArgument(parameters, ParameterType(operation.parameters[i]) + " " + ArgumentName(i));
  }
  return (operation.async ? "Task<" + result + ">" : result) + " " + operation.name +
      "(" + parameters + (operation.is_const ? ") const override" : ") override");
}

std::string PassArgument(const Parameter& parameter, std::size_t index) {
  const auto argument = ArgumentName(index);
  return parameter.reference ? argument : "std::move(" + argument + ")";
}

std::string Arguments(const Operation& operation, bool native_binding = false) {
  std::string arguments;
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto& parameter = operation.parameters[i];
    const auto slot = std::to_string(i + 1);
    auto argument = PassArgument(parameter, i);
    if (native_binding && parameter.kind == ParameterKind::Stream) {
      const auto factory = parameter.type.cpp.ends_with("InputStream") ? "detail::LocalInput(" : "detail::LocalOutput(";
      const auto owner = operation.async ? "invocation->grants" : "grants";
      argument = std::string(factory) + owner + ", " + slot +
          (operation.async ? ", invocation->execution)" : ")");
    } else if (native_binding && parameter.kind == ParameterKind::Receiver) {
      argument = "detail::LocalReceiver<" + parameter.type.cpp + ">(invocation, " + slot + ")";
    }
    AppendArgument(arguments, argument);
  }
  return arguments;
}

std::string CaptureArguments(const Operation& operation, bool worker) {
  std::string captures;
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto& parameter = operation.parameters[i];
    if (parameter.kind == ParameterKind::Value) {
      const auto name = ArgumentName(i);
      AppendArgument(captures, name + " = std::move(" + name + ")");
    } else if (worker && parameter.kind == ParameterKind::Receiver) {
      AppendArgument(captures, "slot" + std::to_string(i + 1));
    }
  }
  return captures;
}

void EmitOperationDocumentation(std::ostream& out, const Operation& operation, std::string_view action) {
  out << "  /// @brief " << action << " " << operation.name << "()"
      << (operation.async ? " when the Task runs.\n" : " synchronously.\n");
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto& parameter = operation.parameters[i];
    out << "  /// @param " << ArgumentName(i) << " ";
    switch (parameter.kind) {
      case ParameterKind::Value:
        out << (operation.async ? "Value owned by the returned Task.\n"
                                 : parameter.reference ? "Value borrowed until the call returns.\n"
                                                       : "Value passed to the implementation.\n");
        break;
      case ParameterKind::Stream:
        out << (parameter.type.cpp.ends_with("InputStream") ? "Source granted for this invocation.\n"
                                                           : "Destination to close before returning.\n");
        break;
      case ParameterKind::Receiver:
        out << "Optional host object granted for this invocation.\n";
        break;
    }
  }
  if (operation.async) {
    out << (operation.response.cpp == "void" ? "  /// @return Lazy Task that completes when the operation finishes.\n"
                                             : "  /// @return Lazy Task yielding the operation's result.\n");
  } else if (operation.response.cpp != "void") {
    out << "  /// @return Result returned by the published operation.\n";
  }
}

void EmitGrants(std::ostream& out, const Operation& operation) {
  out << "    auto grants = std::make_shared<detail::CallGrants>(" << operation.parameters.size() << ");\n";
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto name = ArgumentName(i);
    const auto slot = i + 1;
    switch (operation.parameters[i].kind) {
      case ParameterKind::Value:
        break;
      case ParameterKind::Stream:
        out << "    grants->Add(" << slot << ", std::move(" << name << "));\n";
        break;
      case ParameterKind::Receiver:
        out << "    const std::uint32_t slot" << slot << " = " << name << " ? " << slot << "u : 0u;\n"
            << "    if (slot" << slot << ") detail::GrantReceiver(*grants, slot" << slot << ", std::move(" << name << "));\n";
        break;
    }
  }
}

void EmitWriteArguments(std::ostream& out, const Operation& operation, std::string_view type, std::string_view indent) {
  out << indent << "detail::Writer writer;\n" << indent << "writer.Array(" << operation.parameters.size() << ");\n";
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto& parameter = operation.parameters[i];
    const auto slot = i + 1;
    switch (parameter.kind) {
      case ParameterKind::Value:
        out << indent << "detail::Codec<" << parameter.type.cpp << ", " << type << ">::Write(writer, " << ArgumentName(i) << ");\n";
        break;
      case ParameterKind::Stream:
        out << indent << "detail::Codec<std::uint32_t>::Write(writer, " << slot << ");\n";
        break;
      case ParameterKind::Receiver:
        out << indent << "if (slot" << slot << ") detail::Codec<std::uint32_t>::Write(writer, slot" << slot << ");\n"
            << indent << "else writer.Byte(0xf6);\n";
        break;
    }
  }
}

void EmitReadArguments(std::ostream& out, const Operation& operation, std::string_view type) {
  out << "        detail::Reader reader(payload);\n"
      << "        reader.Record(" << operation.parameters.size() << ");\n";
  if (operation.HasStreams()) out << "        const detail::WorkerStreams streams(invocation);\n";
  // Decode in declaration order before entering the C++ call, whose argument evaluation order can differ.
  for (std::size_t i = 0; i < operation.parameters.size(); ++i) {
    const auto& parameter = operation.parameters[i];
    const auto read_slot = "detail::ReadSlot(reader, " + std::to_string(i + 1) + ")";
    out << "        auto " << ArgumentName(i) << " = ";
    switch (parameter.kind) {
      case ParameterKind::Value:
        out << "detail::Codec<" << parameter.type.cpp << ", " << type << ">::Read(reader)";
        break;
      case ParameterKind::Stream: {
        const auto prefix = operation.async ? "streams.Async" : "streams.";
        out << prefix << (parameter.type.cpp.ends_with("InputStream") ? "Input(" : "Output(") << read_slot << ")";
        break;
      }
      case ParameterKind::Receiver:
        out << "reader.TakeNull() ? nullptr : std::make_shared<Contract<" << parameter.type.cpp
            << ">::Callback>(detail::HostEndpoint(invocation, " << read_slot << "))";
        break;
    }
    out << ";\n";
  }
  out << "        reader.Finish();\n";
  if (operation.HasStreams() && !operation.async) {
    out << "        const detail::SynchronousStreamScope scope(streams.Owner());\n";
  }
}

void EmitAsyncCallback(std::ostream& out, const Operation& operation, std::string_view type) {
  out << ", &" << type << "::" << operation.name;
  const auto arguments = Arguments(operation);
  if (!arguments.empty()) out << ", " << arguments;
  out << ");\n";
}

void EmitExtension(std::ostream& out, const Schema& schema, std::string_view type) {
  out << "/// @brief Adapts the published interface to its installed execution mode.\n"
      << "///\n"
      << "/// Call through ExtensionHandle on the owning HuxerUI application thread. In-process calls use\n"
      << "/// the native C++ implementation. Worker calls encode the arguments and result over IPC.\n"
      << "/// @see PluginManager::CreateExtension()\n"
      << "class Contract<" << type << ">::Extension final : public " << type << " {\npublic:\n"
      << "  /// @brief Binds the interface adapter to the handle's invocation target.\n"
      << "  /// @param target Target created by PluginManager for the approved execution mode.\n"
      << "  explicit Extension(std::shared_ptr<detail::ExtensionTarget> target) : target_(std::move(target)) {}\n\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    const auto& result = operation.response.cpp;
    const auto grants = operation.HasGrants() ? "std::move(grants)" : "nullptr";
    EmitOperationDocumentation(out, operation, "Invokes the extension's");
    out << "  /// @throws Error With Closed for a released or retired target, DeadlineExceeded if its time budget\n"
        << "  /// expires, or PluginFailed if the implementation fails.\n"
        << "  /// @throws Error With WorkerExited if the worker exits, Protocol for a malformed exchange, or\n"
        << "  /// PayloadTooLarge if the encoded request or result exceeds a wire limit.\n";
    if (operation.async) {
      out << "  /// @note Cancellation of an active worker exchange retires its generation. In-process\n"
          << "  /// cancellation requests cooperative stop. Invocation grants are revoked in either mode.\n";
    } else {
      out << "  /// @throws Error With Busy if the plugin is handling another invocation, or CallbackUnavailable\n"
          << "  /// if a synchronous worker call attempts to call a host service.\n"
          << "  /// @note A synchronous worker call blocks until its reply or failure. The in-process time budget\n"
          << "  /// is checked at call boundaries.\n";
    }
    out << "  " << Signature(operation) << " {\n";
    if (operation.HasGrants()) EmitGrants(out, operation);
    out << "    if (target_->Mode() == ExecutionMode::InProcess) {\n";
    const auto call = "static_cast<" + std::string(type) + "*>(object)->" + operation.name +
        "(" + Arguments(operation, true) + ")";
    if (operation.async) {
      out << "      return detail::InvokeLocal<" << result << ">(target_, [" << CaptureArguments(operation, false)
          << "](void* object, [[maybe_unused]] std::shared_ptr<detail::LocalInvocation> invocation";
      if (result != "void") out << ", auto result";
      out << ") mutable {\n";
      if (result == "void") out << "        return " << call << ";\n";
      else out << "        return detail::StoreLocalResult<" << result << ">(" << call << ", result);\n";
      out << "      }, " << grants << ");\n";
    } else {
      out << "      return detail::InvokeLocalSync<" << result << ">(*target_, [&](void* object) {\n"
          << "        return " << call << ";\n"
          << "      }, " << (operation.HasGrants() ? "grants" : "nullptr") << ");\n";
    }
    out << "    }\n";
    if (operation.async) {
      out << "    return detail::InvokeWorker<" << result << ", " << type << ">(target_, " << i + 1 << ", ["
          << CaptureArguments(operation, true) << "] {\n";
      EmitWriteArguments(out, operation, type, "      ");
      out << "      return std::move(writer).Finish();\n    }, " << grants << ");\n";
    } else {
      EmitWriteArguments(out, operation, type, "    ");
      out << "    return detail::DecodeResult<" << result << ", " << type << ">(target_->CallWorkerSync(" << i + 1
          << ", std::move(writer).Finish(), " << grants << "));\n";
    }
    out << "  }\n\n";
  }
  out << "private:\n  std::shared_ptr<detail::ExtensionTarget> target_;\n};\n\n";
}

void EmitCallback(std::ostream& out, const Schema& schema, std::string_view type) {
  out << "/// @brief Typed callback object for a single local or worker invocation.\n"
      << "///\n"
      << "/// Await operations from the owning invocation or an awaited child Task. The adapter expires when\n"
      << "/// the invocation finishes or is cancelled.\n"
      << "class Contract<" << type << ">::Callback final : public " << type << " {\npublic:\n"
      << "  /// @brief Binds a worker callback to its invocation and parameter slot.\n"
      << "  /// @param worker Host endpoint carrying the invocation identity and slot.\n"
      << "  explicit Callback(detail::HostEndpoint worker) : worker_(worker) {}\n\n"
      << "  /// @brief Binds an in-process callback to its invocation and parameter slot.\n"
      << "  /// @param local Weak reference to the invocation that owns the host object.\n"
      << "  /// @param slot One-based position of the argument in that call.\n"
      << "  Callback(std::weak_ptr<detail::LocalInvocation> local, std::uint32_t slot) : local_(std::move(local)), slot_(slot) {}\n\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    EmitOperationDocumentation(out, operation, "Calls the host object's");
    out << "  /// @throws Error With ReceiverClosed if the grant expired or the Task runs outside the owning invocation.\n"
        << "  " << Signature(operation) << " {\n"
        << "    return detail::InvokeReceiver<" << operation.response.cpp << ", " << type << ">(local_, slot_, worker_, " << i + 1;
    EmitAsyncCallback(out, operation, type);
    out << "  }\n\n";
  }
  out << "private:\n  std::optional<detail::HostEndpoint> worker_;\n  std::weak_ptr<detail::LocalInvocation> local_;\n"
      << "  std::uint32_t slot_ = 0;\n};\n\n";
}

void EmitValueCodec(std::ostream& out, const ValueCodec& codec, std::string_view type) {
  out << "template<>\nstruct Codec<" << codec.cpp << ", " << type << ">";
  if (codec.representation) {
    out << " : MappedCodec<" << codec.cpp << ", " << type << ", " << codec.representation->cpp << "> {};\n\n";
    return;
  }
  out << " {\n"
      << "  static void Write(Writer& writer, const " << codec.cpp << "& value) {\n"
      << "    writer.Array(" << codec.fields.size() << ");\n";
  if (codec.fields.empty()) out << "    (void)value;\n";
  for (const auto& field : codec.fields) {
    out << "    Codec<" << field.type.cpp << ", " << type << ">::Write(writer, value." << field.name << ");\n";
  }
  out << "  }\n\n  static " << codec.cpp << " Read(Reader& reader) {\n"
      << "    reader.Record(" << codec.fields.size() << ");\n    return {";
  for (std::size_t i = 0; i < codec.fields.size(); ++i) {
    if (i != 0) out << ", ";
    out << "Codec<" << codec.fields[i].type.cpp << ", " << type << ">::Read(reader)";
  }
  out << "};\n  }\n};\n\n";
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
    for (const auto& parameter : operation.parameters) {
      if (!parameter.receiver_header.empty()) receiver_headers.insert(parameter.receiver_header);
    }
  }
  for (const auto& receiver : receiver_headers) out << "#include \"" << receiver << "\"\n";
  const auto type = "::" + schema.interface_name;
  if (!schema.value_codecs.empty()) {
    out << "\nnamespace huxerui::pluginkit::detail {\n\n";
    for (const auto& codec : schema.value_codecs) EmitValueCodec(out, codec, type);
    out << "} // namespace huxerui::pluginkit::detail\n";
  }
  out << "\nnamespace huxerui::pluginkit {\n\n"
      << "/// @brief Generated contract identity and operation dispatcher for " << schema.interface_name << ".\n"
      << "template<>\nstruct Contract<" << type << "> {\n"
      << "  /// Exact contract identifier, version, and schema fingerprint used by the host and plugin.\n"
      << "  static constexpr ContractInfo Info{\"" << id << "\", " << version << ", \"" << fingerprint << "\"};\n\n"
      << "  /// Generated implementation used by ExtensionHandle.\n"
      << "  class Extension;\n";
  const auto host_service = std::ranges::none_of(schema.operations, &Operation::HasGrants);
  const auto receiver_contract = host_service && std::ranges::all_of(schema.operations, &Operation::async);
  if (host_service) {
    out << "  /// This interface can be registered as a host service.\n"
        << "  static constexpr bool HostService = true;\n";
  }
  if (receiver_contract) {
    out << "  /// Generated implementation passed to a plugin as an invocation-scoped host object.\n"
        << "  class Callback;\n";
  }
  out << "\n"
      << "  /// @brief Decodes the arguments and invokes the selected operation.\n"
      << "  /// @param implementation Instance that implements the interface. Keep it alive until the Task completes.\n"
      << "  /// @param operation Operation number from the published contract.\n"
      << "  /// @param payload Encoded argument list owned by the Task.\n"
      << "  /// @param invocation Worker call identity used to bind the caller's callback and stream slots.\n"
      << "  /// @return Lazy Task yielding the encoded result, or an empty buffer for a void result.\n"
      << "  /// @throws Error With Protocol for an unknown operation or invalid payload, or PayloadTooLarge\n"
      << "  /// if a wire limit is exceeded. Failures from the implementation also propagate.\n"
      << "  static Task<Bytes> Dispatch(" << type << "& implementation, std::uint32_t operation, Bytes payload,\n"
      << "                              [[maybe_unused]] detail::HostInvocation invocation) {\n"
      << "    switch (operation) {\n";
  for (std::size_t i = 0; i < schema.operations.size(); ++i) {
    const auto& operation = schema.operations[i];
    out << "      case " << i + 1 << ": {\n";
    EmitReadArguments(out, operation, type);
    const bool returns_value = operation.response.cpp != "void";
    out << "        ";
    if (returns_value) out << "auto result = ";
    if (operation.async) out << "co_await ";
    out << "implementation." << operation.name << "(" << Arguments(operation) << ");\n"
        << "        co_return ";
    if (returns_value) out << "detail::Encode<" << type << ">(result)";
    else out << "Bytes{}";
    out << ";\n      }\n";
  }
  out << "      default: throw Error(ErrorCode::Protocol, \"Unknown contract operation\");\n"
      << "    }\n"
      << "  }\n};\n\n";

  EmitExtension(out, schema, type);
  if (receiver_contract) EmitCallback(out, schema, type);
  if (host_service) {
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
  WireDefinitions definitions;
  FindWireDefinitions(clang_getTranslationUnitCursor(unit.get()), definitions);
  const Schema schema(interface, definitions);
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
