#include "photo/image_filter.bindings.h"

#include <huxerui/pluginkit/plugin.h>

#if defined(PHOTO_WARM_PANEL)
#include "photo/effect_panel.h"

#include <huxerui/huxerui.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <span>

namespace photo_studio {
namespace {

using namespace huxerui;
using namespace huxerui::pluginkit;

using Rgb = std::array<unsigned, 3>;

[[maybe_unused]] Rgb Monochrome(Rgb color) {
  const auto luminance = (54 * color[0] + 183 * color[1] + 19 * color[2] + 128) / 256;
  return {luminance, luminance, luminance};
}

[[maybe_unused]] Rgb WarmTone(Rgb color) {
  return {std::min(255u, color[0] * 112 / 100 + 14), std::min(255u, color[1] + 6), color[2] * 80 / 100};
}

struct ColorBucket {
  std::uint32_t count = 0;
  std::uint32_t red = 0;
  std::uint32_t green = 0;
  std::uint32_t blue = 0;
};

class PhotoFilter final : public ImageFilter {
public:
  Task<std::vector<Swatch>> Apply(FilterRequest request, AsyncInputStream input, AsyncOutputStream output,
                                std::shared_ptr<FilterProgress> progress) override {
    if (!request.width || !request.height || request.width > PreviewEdgeLimit || request.height > PreviewEdgeLimit ||
        request.strength > 100) {
      throw Error(ErrorCode::InvalidArgument, "Invalid photo dimensions or effect strength");
    }

    // Filling whole rows before transforming also handles sources that split pixels across reads.
    std::array<ColorBucket, 512> palette{};
    std::uint32_t visible_pixels = 0;
    for (std::uint32_t rows = 0; rows < request.height;) {
      const auto block_rows = std::min(32u, request.height - rows);
      const auto bytes = static_cast<std::size_t>(block_rows) * request.width * 4;
      Bytes block;
      block.reserve(bytes);
      while (block.size() < bytes) {
        auto read = co_await input.ReadAsync(bytes - block.size());
        if (!read.Succeeded()) throw Error(ErrorCode::Io, read.Error().message);
        const auto& chunk = read.Value();
        if (chunk.empty()) throw Error(ErrorCode::InvalidArgument, "Incomplete photo input");
        block.insert(block.end(), chunk.begin(), chunk.end());
      }
      for (std::size_t offset = 0; offset < block.size(); offset += 4) {
        Rgb pixel{std::to_integer<unsigned>(block[offset]), std::to_integer<unsigned>(block[offset + 1]),
                  std::to_integer<unsigned>(block[offset + 2])};
        const auto filtered = PHOTO_TRANSFORM(pixel);
        for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
          pixel[channel] = (pixel[channel] * (100 - request.strength) + filtered[channel] * request.strength + 50) / 100;
          block[offset + channel] = static_cast<std::byte>(pixel[channel]);
        }
        if (block[offset + 3] != std::byte{}) {
          auto& bin = palette[(pixel[0] >> 5) * 64 + (pixel[1] >> 5) * 8 + (pixel[2] >> 5)];
          ++bin.count;
          bin.red += pixel[0];
          bin.green += pixel[1];
          bin.blue += pixel[2];
          ++visible_pixels;
        }
      }
      const auto written = co_await output.WriteAsync(std::move(block));
      if (!written.Succeeded()) throw Error(ErrorCode::Io, written.Error().message);
      rows += block_rows;
      if (progress) co_await progress->OnRows(rows);
      // Give the app a chance to render progress between blocks in either execution mode.
      if (rows < request.height) co_await Delay(std::chrono::milliseconds(1));
    }
    const auto end = co_await input.ReadAsync(1);
    if (!end.Succeeded()) throw Error(ErrorCode::Io, end.Error().message);
    if (!end.Value().empty()) throw Error(ErrorCode::InvalidArgument, "Photo input exceeds its declared dimensions");
    const auto closed = co_await output.CloseAsync();
    if (!closed.Succeeded()) throw Error(ErrorCode::Io, closed.Error().message);

    std::stable_sort(palette.begin(), palette.end(), [](const auto& left, const auto& right) {
      return left.count > right.count;
    });
    std::vector<Swatch> swatches;
    for (const auto& bin : std::span(palette).first(4)) {
      if (!bin.count) break;
      const auto rgb = ((bin.red / bin.count) << 16) | ((bin.green / bin.count) << 8) | (bin.blue / bin.count);
      swatches.push_back({rgb, (bin.count * 100 + visible_pixels / 2) / visible_pixels});
    }
    co_return swatches;
  }
};

constexpr auto FilterFactory = BindExtension<ImageFilter, PhotoFilter>(Effects);

#if defined(PHOTO_WARM_PANEL)
class WarmPanel final : public EffectPanel {
public:
  View BuildView(State<float> strength, bool enabled) override {
    return Column {
      Text("Warm Tone controls", TextRole::Title),
      Text::Format("Warmth {}%", static_cast<unsigned>(static_cast<float>(strength))),
      Slider(strength).Range(0.0F, 100.0F).Step(1.0F).OnChanged([strength](float value) {
        strength = value;
      }).With(Enabled(enabled)),
      Flow {
        Button("Soft · 35%").With(Enabled(enabled)).OnClick([strength] { strength = 35.0F; }),
        Button("Golden · 80%").With(Enabled(enabled)).OnClick([strength] { strength = 80.0F; }),
      }.With(Spacing(8.0F)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
};

constexpr auto PanelFactory = BindExtension<EffectPanel, WarmPanel>(EffectPanels);
#endif

} // namespace
} // namespace photo_studio

#if defined(PHOTO_WARM_PANEL)
HPK_EXPORT_PLUGIN(nullptr, nullptr, photo_studio::FilterFactory, photo_studio::PanelFactory)
#else
HPK_EXPORT_PLUGIN(nullptr, nullptr, photo_studio::FilterFactory)
#endif
