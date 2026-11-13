#pragma once

/// @file
/// @brief Byte types and bounded CBOR encoding used by generated contract bindings.

#include <pluginkit/abi.h>
#include <pluginkit/error.h>

#include <concepts>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pluginkit {

/// @brief Owning byte buffer for encoded requests and responses.
using Bytes = std::vector<std::uint8_t>;

/// @brief Read-only view of encoded bytes. The source storage must outlive the view.
using ByteView = std::span<const std::uint8_t>;

/// @brief Maximum encoded payload size in bytes for a single request or response.
inline constexpr std::size_t MaxPayload = HPK_MAX_PAYLOAD;

namespace detail {

inline bool IsUtf8(std::string_view text) {
  for (std::size_t offset = 0; offset < text.size();) {
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) continue;
    if (first < 0xc2 || first > 0xf4) return false;

    const unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (count > text.size() - offset) return false;
    std::uint32_t value = first & (0x7fu >> (count + 1));
    for (unsigned i = 0; i < count; ++i) {
      const auto next = static_cast<unsigned char>(text[offset++]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    constexpr std::uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
    if (value < minimum[count] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}

inline constexpr std::size_t MaxArrayItems = 256 * 1024;

class Writer {
public:
  void Byte(std::uint8_t value) {
    RequireSpace(1);
    bytes_.push_back(value);
  }

  void Head(unsigned major, std::uint64_t value) {
    if (value < 24) {
      Byte(static_cast<std::uint8_t>((major << 5) | value));
      return;
    }
    const unsigned width = value <= UINT8_MAX ? 1 : value <= UINT16_MAX ? 2 : value <= UINT32_MAX ? 4 : 8;
    const unsigned additional = width == 1 ? 24 : width == 2 ? 25 : width == 4 ? 26 : 27;
    Byte(static_cast<std::uint8_t>((major << 5) | additional));
    for (unsigned i = width; i > 0; --i) Byte(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
  }

  void Array(std::size_t count) {
    if (count > MaxArrayItems) throw Error(ErrorCode::PayloadTooLarge, "CBOR array exceeds the item limit");
    Head(4, count);
  }

  void Text(std::string_view text) {
    RequireSpace(text.size());
    if (!IsUtf8(text)) throw Error(ErrorCode::InvalidArgument, "CBOR text must be UTF-8");
    Head(3, text.size());
    RequireSpace(text.size());
    bytes_.insert(bytes_.end(), text.begin(), text.end());
  }

  Bytes Finish() && { return std::move(bytes_); }

private:
  void RequireSpace(std::size_t size) const {
    if (size > MaxPayload - bytes_.size()) throw Error(ErrorCode::PayloadTooLarge, "CBOR payload exceeds the byte limit");
  }

  Bytes bytes_;
};

class Reader {
public:
  explicit Reader(ByteView data) : remaining_(data) {
    if (data.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "CBOR payload exceeds the byte limit");
  }

  std::uint8_t Byte() {
    if (remaining_.empty()) Invalid();
    const auto value = remaining_.front();
    remaining_ = remaining_.subspan(1);
    return value;
  }

  bool TakeNull() {
    if (remaining_.empty() || remaining_.front() != 0xf6) return false;
    Byte();
    return true;
  }

  std::uint64_t Head(unsigned major) {
    const auto first = Byte();
    if (static_cast<unsigned>(first >> 5) != major) Invalid();
    return Argument(first);
  }

  std::pair<unsigned, std::uint64_t> Integer() {
    const auto first = Byte();
    const unsigned major = first >> 5;
    if (major > 1) Invalid();
    return {major, Argument(first)};
  }

  std::size_t Array() {
    const auto count = Head(4);
    // Each item needs at least one byte. Check before a generated decoder reserves storage.
    if (count > MaxArrayItems || count > remaining_.size()) Invalid();
    return static_cast<std::size_t>(count);
  }

  void Record(std::size_t fields) {
    if (Array() != fields) Invalid();
  }

  std::string Text() {
    const auto size = Head(3);
    if (size > remaining_.size()) Invalid();
    const auto text = std::string_view(reinterpret_cast<const char*>(remaining_.data()), static_cast<std::size_t>(size));
    if (!IsUtf8(text)) Invalid();
    remaining_ = remaining_.subspan(static_cast<std::size_t>(size));
    return std::string(text);
  }

  void Finish() const {
    if (!remaining_.empty()) Invalid();
  }

  [[noreturn]] static void Invalid() { throw Error(ErrorCode::Protocol, "Invalid CBOR payload for the contract"); }

private:
  std::uint64_t Argument(std::uint8_t first) {
    const unsigned additional = first & 31;
    if (additional < 24) return additional;
    if (additional > 27) Invalid();
    const unsigned width = 1u << (additional - 24);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | Byte();
    return value;
  }

  ByteView remaining_;
};

template<class T>
struct Codec;

template<std::integral T>
struct Codec<T> {
  static void Write(Writer& writer, T value) {
    if constexpr (std::signed_integral<T>) {
      if (value < 0) {
        writer.Head(1, static_cast<std::uint64_t>(-(value + 1)));
        return;
      }
    }
    writer.Head(0, static_cast<std::uint64_t>(value));
  }

  static T Read(Reader& reader) {
    const auto [major, value] = reader.Integer();
    if (value > static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) Reader::Invalid();
    if (major == 0) return static_cast<T>(value);
    if constexpr (std::signed_integral<T>) return static_cast<T>(-1 - static_cast<T>(value));
    Reader::Invalid();
  }
};

template<>
struct Codec<bool> {
  static void Write(Writer& writer, bool value) { writer.Byte(value ? 0xf5 : 0xf4); }
  static bool Read(Reader& reader) {
    const auto value = reader.Byte();
    if (value != 0xf4 && value != 0xf5) Reader::Invalid();
    return value == 0xf5;
  }
};

template<>
struct Codec<std::string> {
  static void Write(Writer& writer, const std::string& value) { writer.Text(value); }
  static std::string Read(Reader& reader) { return reader.Text(); }
};

template<class T>
struct Codec<std::vector<T>> {
  static void Write(Writer& writer, const std::vector<T>& values) {
    writer.Array(values.size());
    for (const auto& value : values) Codec<T>::Write(writer, value);
  }

  static std::vector<T> Read(Reader& reader) {
    const auto count = reader.Array();
    std::vector<T> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) values.push_back(Codec<T>::Read(reader));
    return values;
  }
};

template<class T>
struct Codec<std::optional<T>> {
  // Wrap present values in an array so an empty inner optional has its own representation.
  static void Write(Writer& writer, const std::optional<T>& value) {
    if (value) {
      writer.Array(1);
      Codec<T>::Write(writer, *value);
    } else {
      writer.Byte(0xf6);
    }
  }

  static std::optional<T> Read(Reader& reader) {
    if (reader.TakeNull()) return std::nullopt;
    reader.Record(1);
    return std::optional<T>(std::in_place, Codec<T>::Read(reader));
  }
};

template<class T>
Bytes Encode(const T& value) {
  Writer writer;
  Codec<T>::Write(writer, value);
  return std::move(writer).Finish();
}

template<class T>
T Decode(ByteView bytes) {
  Reader reader(bytes);
  auto result = Codec<T>::Read(reader);
  reader.Finish();
  return result;
}

} // namespace detail
} // namespace pluginkit
