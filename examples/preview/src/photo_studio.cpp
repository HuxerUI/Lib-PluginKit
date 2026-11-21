#include "photo_studio.h"

#include "photo/effect_panel.h"
#include "photo/image_filter.host.h"

#include <huxerui/huxerui.h>
#include <huxerui/pluginkit.h>
#include <nlohmann/json.hpp>
#include <photo_studio_assets_resources.h>

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

namespace photo_studio {
namespace {

using namespace huxerui;
using namespace huxerui::pluginkit;

constexpr std::size_t SourceLimit = 32 * 1024 * 1024;

struct Effect {
  ExtensionInfo extension;
  std::string digest;
  std::string label;
  std::string description;
  ExecutionMode mode;
  bool has_panel;
};

struct Result {
  Bitmap image;
  std::string label;
  std::uint32_t strength;
  std::vector<Swatch> palette;
};

struct StudioState {
  Bitmap original;
  std::string source;
  std::optional<Result> result;
  std::vector<Effect> effects;
  std::optional<PackageInfo> pending;
  std::size_t selected = 0;
  std::optional<std::uint32_t> completed_rows;
  std::string message;
  bool busy = false;
  bool failed = false;
  bool removing_effect = false;
};

RuntimeDesc StudioRuntime(const AppDirectories& dirs) {
#if defined(__ANDROID__)
  (void)dirs;
  return RuntimeDesc::Android();
#else
  if (!dirs.executable_directory) throw Error(ErrorCode::Io, "Could not locate the studio executable");
#if defined(_WIN32)
  return RuntimeDesc::Executable(dirs.executable_directory->Child("hpk-worker.exe"));
#else
  return RuntimeDesc::Executable(dirs.executable_directory->Child("hpk-worker"));
#endif
#endif
}

std::vector<Effect> DiscoverEffects(const PluginManager& manager) {
  const auto plugins = manager.ListPlugins();
  const auto panels = manager.ListExtensions(EffectPanels);
  std::vector<Effect> effects;
  for (auto extension : manager.ListExtensions(Effects)) {
    const auto plugin = std::ranges::find_if(plugins, [&](const auto& item) {
      return item.package.id == extension.PluginId();
    });
    if (plugin == plugins.end()) continue;
    auto label = extension.PluginId();
    std::string description;
    if (extension.metadata_json) {
      const auto metadata = nlohmann::json::parse(*extension.metadata_json);
      if (const auto name = metadata.find("label"); name != metadata.end() && name->is_string() &&
          !name->get_ref<const std::string&>().empty()) label = name->get<std::string>();
      if (const auto detail = metadata.find("description"); detail != metadata.end() && detail->is_string()) {
        description = detail->get<std::string>();
      }
    }
    const bool has_panel = std::ranges::any_of(panels, [&](const auto& panel) {
      return panel.PluginId() == extension.PluginId();
    });
    effects.push_back({std::move(extension), plugin->package.digest, std::move(label), std::move(description),
                       plugin->mode, has_panel});
  }
  return effects;
}

void RefreshEffects(const PluginManager& manager, State<StudioState> state, std::string_view select = {}) {
  auto effects = DiscoverEffects(manager);
  const auto selected = std::ranges::find_if(effects, [&](const auto& effect) { return effect.extension.PluginId() == select; });
  const auto index = selected == effects.end() ? std::size_t{0} : static_cast<std::size_t>(selected - effects.begin());
  state.Update([&](StudioState& value) {
    value.effects = std::move(effects);
    value.selected = index;
  });
}

class Progress final : public FilterProgress {
public:
  explicit Progress(State<StudioState> state) : state_(std::move(state)) {}

  Task<void> OnRows(std::uint32_t rows) override {
    state_.Update([&](StudioState& value) { value.completed_rows = rows; });
    co_return;
  }

private:
  State<StudioState> state_;
};

struct StagedPixels {
  Bytes bytes;
  bool closed = false;
};

Task<Result> ApplyEffect(std::shared_ptr<PluginManager> host, Effect effect, Bitmap source, std::uint32_t strength,
                         std::shared_ptr<FilterProgress> progress) {
  const auto expected = source.Pixels().size();
  auto staged = std::make_shared<StagedPixels>();
  staged->bytes.reserve(expected);
  auto input = AsyncInputStream::FromReader(
      [source, offset = std::size_t{}](std::stop_token, std::size_t count) mutable -> Task<IoResult<Bytes>> {
        const auto pixels = source.Pixels();
        const auto chunk = pixels.subspan(offset, std::min(count, pixels.size() - offset));
        offset += chunk.size();
        co_return IoResult<Bytes>(Bytes(chunk.begin(), chunk.end()));
      });
  auto output = AsyncOutputStream::FromWriter(
      [staged, expected](std::stop_token, Bytes bytes) -> Task<IoResult<void>> {
        if (bytes.size() > expected - staged->bytes.size()) {
          co_return IoResult<void>::Failure({IoErrorCode::Io, "The filter wrote too many pixels"});
        }
        staged->bytes.insert(staged->bytes.end(), bytes.begin(), bytes.end());
        co_return IoResult<void>::Success();
      },
      [staged, expected](std::stop_token) -> Task<IoResult<void>> {
        if (staged->bytes.size() != expected) {
          co_return IoResult<void>::Failure({IoErrorCode::Io, "The filter did not finish writing the image"});
        }
        staged->closed = true;
        co_return IoResult<void>::Success();
      });

  auto filter = host->CreateExtension(Effects, effect.extension);
  const FilterRequest request{source.PixelWidth(), source.PixelHeight(), strength};
  auto palette = co_await filter->Apply(request, std::move(input), std::move(output), std::move(progress));
  if (!staged->closed || palette.size() > 4 ||
      std::ranges::any_of(palette, [](const Swatch& item) { return item.rgb > 0xffffff || item.percent > 100; })) {
    throw Error(ErrorCode::PluginFailed, "The filter returned an invalid image or palette");
  }
  // Both modes publish the result only after the output is closed and the call succeeds.
  auto bitmap = Bitmap::FromRgba8(request.width, request.height, std::move(staged->bytes));
  co_return Result{std::move(bitmap), std::move(effect.label), strength, std::move(palette)};
}

Bitmap FitPreview(Bitmap image) {
  if (!image.HasValue()) throw Error(ErrorCode::InvalidArgument, "Could not decode the photo");
  const auto edge = std::max(image.PixelWidth(), image.PixelHeight());
  if (edge <= PreviewEdgeLimit) return image;
  const auto scaled = [edge](std::uint32_t length) {
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1, std::uint64_t{length} * PreviewEdgeLimit / edge));
  };
  return image.Resize(scaled(image.PixelWidth()), scaled(image.PixelHeight()));
}

Task<Bitmap> LoadPhoto(const FileReference& file) {
  auto stream = co_await file.OpenReadAsync();
  if (!stream.Succeeded()) throw Error(ErrorCode::Io, stream.Error().message);
  auto input = std::move(stream).Value();
  Bytes bytes;
  for (;;) {
    auto read = co_await input.ReadAsync(512 * 1024);
    if (!read.Succeeded()) throw Error(ErrorCode::Io, read.Error().message);
    const auto& chunk = read.Value();
    if (chunk.empty()) break;
    if (chunk.size() > SourceLimit - bytes.size()) throw Error(ErrorCode::PayloadTooLarge, "Choose a photo under 32 MiB");
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
  }
  auto image = co_await ImageAsset::FromEncoded(std::move(bytes)).DecodeAsync();
  co_return co_await RunWorker(FitPreview, std::move(image));
}

// Serialize the studio's picker and plugin actions and report failures in the window.
template<class Action>
void RunAction(const TaskScope& tasks, State<StudioState> state, std::string status, Action action) {
  if (state->busy) return;
  state.Update([&](StudioState& value) {
    value.busy = true;
    value.failed = false;
    value.message = std::move(status);
  });
  tasks.Launch([state, action = std::move(action)]() mutable -> Task<void> {
    try {
      auto message = co_await action();
      state.Update([&](StudioState& value) {
        value.busy = false;
        value.removing_effect = false;
        value.completed_rows.reset();
        value.message = std::move(message);
      });
    } catch (const std::exception& error) {
      state.Update([&](StudioState& value) {
        value.busy = false;
        value.removing_effect = false;
        value.failed = true;
        value.completed_rows.reset();
        value.message = error.what();
      });
    }
  });
}

Task<std::string> InspectSelection(std::shared_ptr<FilePicker> picker, File staging, State<StudioState> state) {
  auto selected = co_await picker->OpenDirectoryAsync();
  if (!selected) co_return std::string{};
  state.Update([](StudioState& value) { value.pending.reset(); });
  const auto children = co_await selected->ListChildrenAsync();
  if (!children.Succeeded()) throw Error(ErrorCode::Io, children.Error().message);
  if (std::ranges::none_of(children.Value(), [](const auto& child) { return child.Name() == "plugin.json"; })) {
    throw Error(ErrorCode::InvalidPackage, "Choose the effect folder containing plugin.json");
  }
  // Copy provider-backed directories into app storage before inspecting the package bytes.
  if (!co_await staging.DeleteRecursivelyAsync()) throw Error(ErrorCode::Io, "Could not clear the package staging directory");
  if (!co_await staging.CreateDirectoriesAsync()) throw Error(ErrorCode::Io, "Could not create the package staging directory");
  auto copied = co_await selected->CopyDirectoryContentsToAsync(staging);
  if (!copied.Succeeded()) throw Error(ErrorCode::Io, copied.Error().message);
  auto package = co_await RunWorker([](const File& path) { return PluginManager::InspectPackage(path); }, staging);
  if (std::ranges::none_of(package.extensions, [](const auto& item) { return item.Point() == Effects.id; })) {
    throw Error(ErrorCode::InvalidPackage, "This package does not provide a photo effect");
  }
  state.Update([&](StudioState& value) { value.pending = std::move(package); });
  co_return std::string{};
}

View Surface(const ColorScheme& colors, View contents) {
  return std::move(contents).With(Padding(18.0F), Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch),
      Background(colors.surface), CornerRadius(16.0F));
}

View PhotoPane(const ColorScheme& colors, std::string heading, std::string detail, const Bitmap& image, float height) {
  return Surface(colors, Column {
    Text(std::move(heading), TextRole::Title),
    Stack {
      image.HasValue() ? Image(image).Fit(ImageFit::Contain).With(Frame{.height = height}) :
          Text("Apply an installed effect to preview it here.").With(Padding(16.0F), Foreground(colors.on_surface_variant),
              Align(HorizontalAlignment::Center, VerticalAlignment::Center)),
    }.With(Frame{.height = height}, Background(colors.surface_container), CornerRadius(10.0F)),
    Text(std::move(detail), TextRole::Label).LineLimit(1).Overflow(TextOverflow::Ellipsis)
        .With(Foreground(colors.on_surface_variant)),
  });
}

View Palette(const ColorScheme& colors, const std::vector<Swatch>& palette) {
  if (palette.empty()) return {};
  return Surface(colors, Column {
    Text("Dominant colors", TextRole::Title),
    Flow {
      ForEach(std::views::iota(std::size_t{0}, palette.size()), [&](std::size_t index) {
        const auto& item = palette[index];
        const auto color = Color::Rgb((item.rgb >> 16) & 255u, (item.rgb >> 8) & 255u, item.rgb & 255u);
        return Column {
          Stack{}.With(Frame{.width = 124.0F, .height = 36.0F}, Background(color), CornerRadius(8.0F)),
          Text(std::format("#{:06X} · {}%", item.rgb, item.percent)).With(Foreground(colors.on_surface_variant)),
        }.With(Spacing(6.0F)).Key(index);
      }),
    }.With(Spacing(14.0F)),
  });
}

struct MountedPanel {
  std::string plugin_id;
  ExtensionHandle<EffectPanel> handle;
};

[[huxerui::composable]]
View PluginControls(std::shared_ptr<PluginManager> host, std::string plugin_id, State<float> strength, bool enabled) {
  const auto panel = UseState(std::shared_ptr<MountedPanel>{});
  const auto error = UseState(std::string{});

  Lifecycle([=] {
    error = std::string{};
    try {
      panel = std::make_shared<MountedPanel>(MountedPanel{plugin_id, host->CreateExtension(EffectPanels, plugin_id)});
    } catch (const std::exception& failure) {
      error = std::string(failure.what());
    }
    // Keep the borrowed extension alive until the tree containing its callbacks has been retired.
    return [panel] { panel = std::shared_ptr<MountedPanel>{}; };
  }, host, plugin_id);

  const std::shared_ptr<MountedPanel>& mounted = panel;
  if (mounted && mounted->plugin_id == plugin_id) return mounted->handle->BuildView(strength, enabled);
  const std::string& message = error;
  return message.empty() ? View{} : Text(message).With(Foreground(UseTheme().colors.error));
}

[[huxerui::composable]]
View StudioContent() {
  const auto& colors = UseTheme().colors;
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  const auto sample = UseImage(photo_studio_assets::images::mountain_lake);
  const auto dirs = UseApplication().Directories();
  const auto staging = dirs.temporary_directory.Child("photo-studio-package");
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto callbacks = UseApplicationTaskScope();
  const auto manager = UseState(std::shared_ptr<PluginManager>{});
  const auto state = UseState(StudioState{});
  const auto strength = UseState(100.0F);

  Lifecycle([=] {
    RunAction(tasks, state, "Opening studio…", [=]() -> Task<std::string> {
      auto image = FitPreview(co_await sample.DecodeAsync());
      state.Update([&](StudioState& value) {
        value.original = std::move(image);
        value.source = "Mountain Lake Sunset";
      });
      auto host = std::make_shared<PluginManager>(dirs.data_directory.Child("photo-studio"), StudioRuntime(dirs), callbacks);
      host->RegisterExtensionPoint(Effects);
      host->RegisterExtensionPoint(EffectPanels);
      const auto changes = co_await host->ApplyPendingChanges();
      RefreshEffects(*host, state);
      manager = std::move(host);
      for (const auto& change : changes) {
        if (change.error) co_return change.message;
      }
      co_return std::string{};
    });
  });

  const std::shared_ptr<PluginManager> host = manager;
  const bool ready = host && !state->busy;
  const auto model = state.operator->();
  const auto* active = model->effects.empty() ? nullptr : &model->effects[model->selected];
  const auto can_install = [&](ExecutionMode mode) {
    if (!ready || !model->pending) return false;
    const auto& modes = model->pending->execution_modes;
    return std::ranges::find(modes, mode) != modes.end();
  };
  const auto install = [=](ExecutionMode mode) {
    if (!state->pending) return;
    RunAction(tasks, state, "Installing effect…", [=]() -> Task<std::string> {
      const auto package = *state->pending;
      host->InstallPackage(staging, package.digest, {.mode = mode, .services = {}});
      RefreshEffects(*host, state, package.id);
      state.Update([](StudioState& value) { value.pending.reset(); });
      (void)co_await staging.DeleteRecursivelyAsync();
      co_return "Effect installed.";
    });
  };

  const float height = compact ? 220.0F : 288.0F;
  auto original = PhotoPane(colors, "Original", model->original.HasValue() ?
      std::format("{} · {} × {} px", model->source, model->original.PixelWidth(), model->original.PixelHeight()) : "Loading photo",
      model->original, height);
  auto result = PhotoPane(colors, "Result", model->result ? std::format("{} · {}%", model->result->label,
      model->result->strength) : "No effect applied", model->result ? model->result->image : Bitmap{}, height);
  auto comparison = compact ? View(Column {std::move(original), std::move(result)}
      .With(Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch))) :
      View(Row {std::move(original).With(Grow(1.0F)), std::move(result).With(Grow(1.0F))}.With(Spacing(14.0F)));

  return ScrollView {
    Column {
      Flow {
        Text("Photo Studio", TextRole::Title).With(FontSize(32.0F)),
        Button("Open photo").With(Enabled(ready && picker->CanOpenFiles())).OnClick([=] {
          RunAction(tasks, state, "Opening photo…", [=]() -> Task<std::string> {
            auto file = co_await picker->OpenFileAsync({.name = "Photos", .extensions = {"jpg", "jpeg", "png"},
                                                       .content_types = {"image/jpeg", "image/png"}});
            if (!file) co_return std::string{};
            auto image = co_await LoadPhoto(*file);
            state.Update([&](StudioState& value) {
              value.original = std::move(image);
              value.source = file->Name();
              value.result.reset();
            });
            co_return std::string{};
          });
        }),
        Button("Install effect").With(Enabled(ready && picker->CanOpenDirectories())).OnClick([=] {
          RunAction(tasks, state, "Inspecting effect package…", [=] { return InspectSelection(picker, staging, state); });
        }),
      }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
      model->pending ? Surface(colors, Column {
        Text::Format("Install {} · {}", model->pending->id, model->pending->version).With(FontSize(18.0F)),
        SelectionArea {Text::Format("SHA-256 {}", model->pending->digest).With(FontSize(12.0F))},
        Flow {
          Button("Run in worker").With(Enabled(can_install(ExecutionMode::WorkerProcess)))
              .OnClick([=] { install(ExecutionMode::WorkerProcess); }),
          Button("Run in app").With(Enabled(can_install(ExecutionMode::InProcess)))
              .OnClick([=] { install(ExecutionMode::InProcess); }),
          Button("Cancel").With(Enabled(ready)).OnClick([=] {
            state.Update([](StudioState& value) { value.pending.reset(); });
          }),
        }.With(Spacing(8.0F)),
      }) : View{},
      Surface(colors, Column {
        Text("Effects", TextRole::Title),
        active ? Flow {
          ForEach(std::views::iota(std::size_t{0}, model->effects.size()), [&](std::size_t index) {
            return Chip(model->effects[index].label, model->selected == index).OnChanged([state, index](bool selected) {
              if (selected && !state->busy) state.Update([&](StudioState& value) { value.selected = index; });
            }).With(Enabled(ready)).Key(index);
          }),
        }.With(Spacing(8.0F)) : Text("Install an effect from a folder containing plugin.json.")
            .With(Foreground(colors.on_surface_variant)),
        active ? Text::Format("{} · {}", active->extension.PluginId(),
            active->mode == ExecutionMode::InProcess ? "In process" : "Worker process")
            .With(Foreground(colors.on_surface_variant)) : View{},
        active && !active->description.empty() ? Text(active->description) : View{},
        active && active->has_panel && !model->removing_effect
            ? PluginControls(host, active->extension.PluginId(), strength, ready) : View{},
        active ? Flow {
          active->has_panel ? View{} : Text::Format("Strength {}%", static_cast<unsigned>(static_cast<float>(strength))),
          active->has_panel ? View{} : Slider(strength).Range(0.0F, 100.0F).Step(1.0F)
              .OnChanged([strength](float value) { strength = value; }).With(Frame{.width = 170.0F}, Enabled(ready)),
          Button("Apply").With(Enabled(ready && model->original.HasValue())).OnClick([=] {
            const auto effect = state->effects[state->selected];
            const auto percent = static_cast<std::uint32_t>(static_cast<float>(strength));
            RunAction(tasks, state, "Applying " + effect.label + "…", [=]() -> Task<std::string> {
              state.Update([](StudioState& value) { value.completed_rows = 0; });
              auto output = co_await ApplyEffect(host, effect, state->original, percent, std::make_shared<Progress>(state));
              state.Update([&](StudioState& value) { value.result = std::move(output); });
              co_return std::string{};
            });
          }),
          Button("Remove effect").With(Enabled(ready)).OnClick([=] {
            const auto effect = state->effects[state->selected];
            RunAction(tasks, state, "Removing effect…", [=]() -> Task<std::string> {
              // Uninstall waits for borrowed handles. Omit the panel so its lifecycle releases ours.
              state.Update([](StudioState& value) { value.removing_effect = true; });
              co_await host->UninstallPackage(effect.extension.PluginId(), effect.digest);
              RefreshEffects(*host, state);
              co_return "Effect removed.";
            });
          }),
        }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)) : View{},
        model->message.empty() ? View{} : Text(model->message).With(Foreground(model->failed ? colors.error : colors.primary)),
        model->completed_rows ? ProgressBar(std::min(1.0F,
            static_cast<float>(*model->completed_rows) / static_cast<float>(model->original.PixelHeight()))) : View{},
      }),
      std::move(comparison),
      model->result ? Palette(colors, model->result->palette) : View{},
    }.With(Padding(compact ? 12.0F : 22.0F), Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Background(colors.background), ScrollBar());
}

} // namespace

View StudioRoot() {
  auto theme = ThemeSpec::Default();
  theme.colors.primary = Color::Rgb(137, 66, 35);
  theme.colors.primary_container = Color::Rgb(251, 229, 211);
  theme.colors.on_primary_container = Color::Rgb(94, 45, 24);
  theme.colors.background = Color::Rgb(245, 242, 236);
  theme.colors.surface = Color::Rgb(255, 253, 249);
  theme.colors.surface_container = Color::Rgb(232, 226, 216);
  theme.colors.on_surface = Color::Rgb(40, 34, 29);
  theme.colors.on_surface_variant = Color::Rgb(101, 91, 81);
  theme.interactions.focus_ring = {theme.colors.primary, 2.0F, 2.0F};
  return MaterialTheme(std::move(theme), StudioContent());
}

} // namespace photo_studio
