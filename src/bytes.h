#pragma once

#include <huxerui/pluginkit/detail/wire.h>

#include <charconv>
#include <filesystem>

namespace huxerui::pluginkit::detail {

inline std::span<const std::byte> AsBytes(std::string_view text) {
  return std::as_bytes(std::span(text));
}

inline std::string_view AsText(std::span<const std::byte> bytes) {
  if (bytes.empty()) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::string PathText(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {text.begin(), text.end()};
}

template<class T>
T ParseInteger(std::string_view text, std::string_view name) {
  T result{};
  if (!text.empty()) {
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result);
    if (parsed.ec == std::errc{} && parsed.ptr == end) return result;
  }
  throw Error(ErrorCode::InvalidArgument, "Invalid " + std::string(name));
}

} // namespace huxerui::pluginkit::detail
