#pragma once

/// @file
/// @brief Contracts published by the Photo Studio host.

#include "filter_progress.h"

#include <huxerui/pluginkit/contract.h>
#include <huxerui/stream.h>
#include <huxerui/task.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace photo_studio {

/// @brief Maximum width and height, in pixels, used by the interactive preview.
inline constexpr std::uint32_t PreviewEdgeLimit = 2048;

/// @brief Dimensions of the RGBA8 stream and the selected effect strength.
struct FilterRequest {
  /// Positive pixel dimensions, each at most PreviewEdgeLimit.
  std::uint32_t width;
  std::uint32_t height;
  /// Effect strength from 0 (original colors) through 100 (full effect).
  std::uint32_t strength;
};

/// @brief A dominant color in the output image and its rounded share of nontransparent pixels.
struct Swatch {
  /// Packed sRGB channels as 0xRRGGBB.
  std::uint32_t rgb;
  /// Rounded percentage of pixels represented by this color bucket.
  std::uint32_t percent;
};

/// @brief Converts an image's colors while preserving its dimensions and alpha channel.
class HPK_REMOTE ImageFilter {
public:
  virtual ~ImageFilter() = default;

  /// @brief Filters tightly packed, top-down, straight-alpha sRGB RGBA8 pixels.
  /// @param request Input dimensions and selected strength.
  /// @param input Exactly width times height times four input bytes.
  /// @param output Destination for the same number of bytes, closed on success.
  /// @param progress Optional host receiver, called after each block is written.
  /// @return Up to four representative output colors, ordered by frequency.
  virtual huxerui::Task<std::vector<Swatch>> Apply(FilterRequest request, huxerui::AsyncInputStream input,
      huxerui::AsyncOutputStream output, std::shared_ptr<FilterProgress> progress) = 0;
};

/// @brief Extension point discovered by the studio to populate its effect picker.
inline constexpr huxerui::pluginkit::ExtensionPoint<ImageFilter> Effects{"example.photo-filters"};

} // namespace photo_studio
