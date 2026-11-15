#pragma once

#include <huxerui/pluginkit.h>

#include <nlohmann/json_fwd.hpp>

namespace huxerui::pluginkit::detail {

struct SavedSetting {
  SettingType type;
  SettingValue value;

  bool operator==(const SavedSetting&) const = default;
};

// Keep each override's schema type so uninstall/reinstall can distinguish strings from enumeration choices.
using SavedSettings = std::map<std::string, SavedSetting, std::less<>>;

std::vector<SettingDefinition> ParseSettingsSchema(const nlohmann::json& manifest);
void ValidateSettingsResources(const std::vector<SettingDefinition>& schema, const File& directory);
std::vector<SettingDefinition> ResolveSettingsSchema(std::vector<SettingDefinition> schema, const File& directory,
                                                     const Locale& locale);
SavedSettings ReadSavedSettings(const nlohmann::json& json);
nlohmann::json DescribeSavedSettings(const SavedSettings& settings);
SavedSettings ValidateOverrides(const std::vector<SettingDefinition>& schema, SettingValues overrides);
SavedSettings RetainOverrides(const std::vector<SettingDefinition>& schema, const SavedSettings& saved);
SettingValues ExplicitOverrides(const SavedSettings& saved);
SettingValues EffectiveSettings(const std::vector<SettingDefinition>& schema, const SavedSettings& saved);

} // namespace huxerui::pluginkit::detail
