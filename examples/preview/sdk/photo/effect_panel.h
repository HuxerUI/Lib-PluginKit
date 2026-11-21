#pragma once

/// @file
/// @brief In-process controls contributed by a Photo Studio effect.

#include <huxerui/pluginkit/contract.h>
#include <huxerui/state.h>
#include <huxerui/view.h>

namespace photo_studio {

/// @brief Supplies native HuxerUI controls for an effect in the host window.
class EffectPanel {
public:
  virtual ~EffectPanel() = default;

  /// @brief Builds the panel on the host's UI thread during composition.
  /// @param strength Shared effect strength from 0 through 100.
  /// @param enabled Whether the user can change the controls.
  /// @return The view to mount while the host owns this extension instance.
  virtual huxerui::View BuildView(huxerui::State<float> strength, bool enabled) = 0;
};

/// @brief Optional controls provided by the same package as its photo effect.
inline constexpr huxerui::pluginkit::ExtensionPoint<EffectPanel> EffectPanels{"example.photo-filter-panels"};

} // namespace photo_studio

namespace huxerui::pluginkit {

/// @brief Shared identity of the native effect panel interface.
template<>
struct Contract<photo_studio::EffectPanel> {
  /// Contract identity published to both the host and its in-process plugins.
  static constexpr ContractInfo Info{"example.photo.effect-panel", 1,
      "1ee1960a9b1bf93728c792cbbd5e457ca0f6be4c23d872e2ecd8289cdf29c972"};
};

} // namespace huxerui::pluginkit
