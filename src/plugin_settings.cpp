#include "plugin_settings.h"
#include "digest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <set>

namespace huxerui::pluginkit::detail {
namespace {

using Json = nlohmann::json;

[[noreturn]] void InvalidField(std::string_view field, std::string_view message,
                               ErrorCode code = ErrorCode::InvalidPackage) {
  throw Error(code, std::string(field) + ": " + std::string(message));
}

SettingType ParseType(std::string_view type, std::string_view field, ErrorCode code = ErrorCode::InvalidPackage) {
  if (type == "boolean") return SettingType::Boolean;
  if (type == "integer") return SettingType::Integer;
  if (type == "string") return SettingType::String;
  if (type == "enum") return SettingType::Enumeration;
  InvalidField(field, "Unknown setting type", code);
}

std::string_view TypeName(SettingType type) {
  switch (type) {
    case SettingType::Boolean: return "boolean";
    case SettingType::Integer: return "integer";
    case SettingType::String: return "string";
    case SettingType::Enumeration: return "enum";
  }
  throw Error(ErrorCode::InvalidArgument, "Unknown setting type");
}

std::int64_t ParseInteger(const Json& value, std::string_view field, ErrorCode code = ErrorCode::InvalidPackage) {
  constexpr auto limit = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > limit)) {
    InvalidField(field, "Expected a signed 64-bit integer", code);
  }
  return value.get<std::int64_t>();
}

SettingValue ParseValue(const Json& value, std::string_view field, ErrorCode code = ErrorCode::InvalidPackage) {
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number_integer()) return ParseInteger(value, field, code);
  if (value.is_string()) return value.get<std::string>();
  InvalidField(field, "Expected a boolean, signed 64-bit integer, or string", code);
}

bool HasValueType(SettingType type, const SettingValue& value) {
  switch (type) {
    case SettingType::Boolean: return std::holds_alternative<bool>(value);
    case SettingType::Integer: return std::holds_alternative<std::int64_t>(value);
    case SettingType::String:
    case SettingType::Enumeration: return std::holds_alternative<std::string>(value);
  }
  return false;
}

std::optional<SettingIssueKind> ValueIssue(const SettingDefinition& definition, const SettingValue& value) {
  if (!HasValueType(definition.type, value)) return SettingIssueKind::TypeMismatch;
  if (const auto integer = std::get_if<std::int64_t>(&value)) {
    if ((definition.minimum && *integer < *definition.minimum) ||
        (definition.maximum && *integer > *definition.maximum)) return SettingIssueKind::OutOfRange;
  }
  if (const auto text = std::get_if<std::string>(&value)) {
    if (!IsUtf8(*text)) return SettingIssueKind::InvalidText;
    if (definition.type == SettingType::Enumeration &&
        std::ranges::find(definition.options, *text, &SettingOption::value) == definition.options.end()) {
      return SettingIssueKind::UnknownOption;
    }
  }
  return std::nullopt;
}

SettingText ParseText(const Json& value, std::string_view field) {
  if (value.is_string()) return value.get<std::string>();
  if (!value.is_object() || value.size() != 1 || !value.contains("resource") || !value.at("resource").is_string()) {
    InvalidField(field, "Expected literal text or an object containing a string resource reference");
  }
  const std::string_view reference = value.at("resource").get_ref<const std::string&>();
  const auto separator = reference.find(':');
  constexpr std::string_view prefix = "strings/";
  const auto key = separator == std::string_view::npos ? std::string_view{} : reference.substr(separator + 1);
  if (separator == 0 || !key.starts_with(prefix) || key == prefix) {
    InvalidField(field, "Expected a resource identity in the form namespace:strings/key");
  }
  try {
    return StringResource(reference.substr(0, separator), key);
  } catch (const std::invalid_argument& error) {
    InvalidField(field, "Invalid string resource " + std::string(reference) + ": " + error.what());
  }
}

std::vector<SettingOption> ParseOptions(const Json& options, const std::string& field) {
  if (!options.is_array() || options.empty()) InvalidField(field, "Expected a nonempty list of choices");
  std::set<std::string, std::less<>> values;
  std::vector<SettingOption> result;
  result.reserve(options.size());
  for (const auto& option : options) {
    const auto path = field + '[' + std::to_string(result.size()) + ']';
    if (!option.is_object() || option.size() != 2 || !option.contains("value") || !option.contains("title") ||
        !option.at("value").is_string()) {
      InvalidField(path, "Expected a string value and a title");
    }
    auto value = option.at("value").get<std::string>();
    if (!values.insert(value).second) InvalidField(path + ".value", "Duplicate choice value");
    result.push_back({std::move(value), ParseText(option.at("title"), path + ".title")});
  }
  return result;
}

template<class Schema, class Visit>
void VisitSettingTexts(Schema& schema, Visit visit) {
  for (auto& setting : schema) {
    const auto field = "settings." + setting.key;
    visit(setting.title, field + ".title");
    if (setting.description) visit(*setting.description, field + ".description");
    for (std::size_t i = 0; i < setting.options.size(); ++i) {
      visit(setting.options[i].title, field + ".options[" + std::to_string(i) + "].title");
    }
  }
}

std::optional<std::string> ResolveText(const SettingText& text, std::string_view field, const File& directory,
                                      const ResourceConfiguration& configuration, std::optional<Resources>& resources) {
  const auto resource = std::get_if<StringResource>(&text);
  if (!resource) return std::nullopt;
  try {
    if (!resources) resources.emplace(Resources::OpenPackage(directory));
    return resources->GetString(*resource, configuration);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    InvalidField(field, "Cannot resolve string resource " + resource->ToString() + ": " + error.what());
  }
}

} // namespace

std::vector<SettingDefinition> ParseSettingsSchema(const Json& manifest) {
  const auto settings = manifest.find("settings");
  if (settings == manifest.end()) return {};
  if (!settings->is_array()) InvalidField("settings", "Expected a list of declarations");
  std::vector<SettingDefinition> result;
  result.reserve(settings->size());
  std::set<std::string, std::less<>> keys;
  for (const auto& entry : *settings) {
    const auto field = "settings[" + std::to_string(result.size()) + ']';
    if (!entry.is_object() || !entry.contains("key") || !entry.at("key").is_string() ||
        !entry.contains("type") || !entry.at("type").is_string() || !entry.contains("title") ||
        !entry.contains("default")) {
      InvalidField(field, "Expected a setting key, type, title, and default");
    }
    auto key = entry.at("key").get<std::string>();
    if (!IsId(key) || !keys.insert(key).second) InvalidField(field + ".key", "Invalid or duplicate setting key");
    const auto type = ParseType(entry.at("type").get_ref<const std::string&>(), field + ".type");
    SettingDefinition definition{std::move(key), type, ParseText(entry.at("title"), field + ".title"), {},
                                 ParseValue(entry.at("default"), field + ".default"), {}, {}, {}};
    for (const auto& [name, value] : entry.items()) {
      if (name == "key" || name == "type" || name == "title" || name == "default") continue;
      const auto path = field + '.' + name;
      if (name == "description") definition.description = ParseText(value, path);
      else if (name == "minimum" && type == SettingType::Integer) definition.minimum = ParseInteger(value, path);
      else if (name == "maximum" && type == SettingType::Integer) definition.maximum = ParseInteger(value, path);
      else if (name == "options" && type == SettingType::Enumeration) definition.options = ParseOptions(value, path);
      else InvalidField(path, "Unexpected field for this setting type");
    }
    if (definition.minimum && definition.maximum && *definition.minimum > *definition.maximum) {
      InvalidField(field, "The minimum exceeds the maximum");
    }
    if (ValueIssue(definition, definition.default_value)) InvalidField(field + ".default", "Invalid setting default");
    result.push_back(std::move(definition));
  }
  return result;
}

void ValidateSettingsResources(const std::vector<SettingDefinition>& schema, const File& directory) {
  std::optional<Resources> resources;
  const ResourceConfiguration defaults;
  VisitSettingTexts(schema, [&](const SettingText& text, std::string_view field) {
    (void)ResolveText(text, field, directory, defaults, resources);
  });
}

std::vector<SettingDefinition> ResolveSettingsSchema(std::vector<SettingDefinition> schema, const File& directory,
                                                     const Locale& locale) {
  std::optional<Resources> resources;
  const ResourceConfiguration configuration{locale};
  VisitSettingTexts(schema, [&](SettingText& text, std::string_view field) {
    if (auto value = ResolveText(text, field, directory, configuration, resources)) text = std::move(*value);
  });
  return schema;
}

SavedSettings ReadSavedSettings(const Json& json) {
  if (!json.is_object()) throw Error(ErrorCode::Io, "Saved settings must be a JSON object");
  SavedSettings result;
  for (const auto& [key, saved] : json.items()) {
    const auto field = "settings." + key;
    const auto type = ParseType(saved.at("type").get_ref<const std::string&>(), field, ErrorCode::Io);
    auto value = ParseValue(saved.at("value"), field, ErrorCode::Io);
    if (!IsId(key) || !HasValueType(type, value)) InvalidField(field, "Invalid saved setting", ErrorCode::Io);
    result.emplace(key, SavedSetting{type, std::move(value)});
  }
  return result;
}

Json DescribeSavedSettings(const SavedSettings& settings) {
  auto json = Json::object();
  for (const auto& [key, setting] : settings) {
    json[key] = {{"type", TypeName(setting.type)},
                 {"value", std::visit([](const auto& value) -> Json { return value; }, setting.value)}};
  }
  return json;
}

SavedSettings ValidateOverrides(const std::vector<SettingDefinition>& schema, SettingValues overrides) {
  SavedSettings result;
  std::vector<SettingIssue> issues;
  for (auto& [key, value] : overrides) {
    const auto definition = std::ranges::find(schema, key, &SettingDefinition::key);
    if (definition == schema.end()) issues.push_back({key, SettingIssueKind::UnknownKey});
    else if (const auto issue = ValueIssue(*definition, value)) issues.push_back({key, *issue});
    else result.emplace(key, SavedSetting{definition->type, std::move(value)});
  }
  if (!issues.empty()) throw SettingsValidationError(std::move(issues));
  return result;
}

SavedSettings RetainOverrides(const std::vector<SettingDefinition>& schema, const SavedSettings& saved) {
  SavedSettings result;
  for (const auto& definition : schema) {
    const auto setting = saved.find(definition.key);
    if (setting != saved.end() && setting->second.type == definition.type && !ValueIssue(definition, setting->second.value)) {
      result.insert(*setting);
    }
  }
  return result;
}

SettingValues ExplicitOverrides(const SavedSettings& saved) {
  SettingValues result;
  for (const auto& [key, setting] : saved) result.emplace(key, setting.value);
  return result;
}

SettingValues EffectiveSettings(const std::vector<SettingDefinition>& schema, const SavedSettings& saved) {
  SettingValues result;
  for (const auto& definition : schema) {
    const auto setting = saved.find(definition.key);
    result.emplace(definition.key, setting == saved.end() ? definition.default_value : setting->second.value);
  }
  return result;
}

} // namespace huxerui::pluginkit::detail
