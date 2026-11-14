#pragma once

#include "bytes.h"

#include <huxerui/pluginkit/contract.h>

#include <array>
#include <chrono>

namespace huxerui::pluginkit::detail {

enum class Command : std::uint32_t { Activate = 1, Create, Invoke, Close, Shutdown };
enum class FrameKind : std::uint32_t { Request = 1, Reply, ServiceCall, ServiceReply };

struct Frame {
  std::uint32_t code;
  std::uint32_t instance = 0;
  std::uint32_t operation = 0;
  Bytes payload;
  FrameKind kind = FrameKind::Request;
  std::uint32_t call = 0;
  std::uint32_t callback = 0;
  std::uint32_t receiver = 0;
  bool synchronous = false;
};

inline constexpr std::uint32_t FrameMagic = 0x314b5048; // HPK1 in little-endian byte order.
using Deadline = std::chrono::steady_clock::time_point;

inline bool IsErrorCode(std::uint32_t code) {
  return code >= static_cast<std::uint32_t>(ErrorCode::InvalidArgument) &&
         code <= static_cast<std::uint32_t>(ErrorCode::Ambiguous);
}

struct ExpectedExtension {
  std::string point;
  std::string contract_id;
  std::uint32_t contract_version;
  std::string contract_fingerprint;

  ContractInfo Info() const noexcept { return {contract_id, contract_version, contract_fingerprint}; }
};

struct PluginStartup {
  std::string id;
  std::string version;
  std::string digest;
  std::string bundle_directory;
  std::string data_directory;
  std::vector<ExpectedExtension> extensions;

  Bytes Encode() const {
    Writer writer;
    writer.Array(6);
    writer.Text(id);
    writer.Text(version);
    writer.Text(digest);
    writer.Text(bundle_directory);
    writer.Text(data_directory);
    writer.Array(extensions.size());
    for (const auto& extension : extensions) {
      writer.Array(4);
      writer.Text(extension.point);
      writer.Text(extension.contract_id);
      Codec<std::uint32_t>::Write(writer, extension.contract_version);
      writer.Text(extension.contract_fingerprint);
    }
    return std::move(writer).Finish();
  }

  static PluginStartup Decode(std::span<const std::byte> payload) {
    Reader reader(payload);
    reader.Record(6);
    PluginStartup startup{reader.Text(), reader.Text(), reader.Text(), reader.Text(), reader.Text(), {}};
    const auto count = reader.Array();
    if (!count || count > HPK_MAX_EXTENSIONS) throw Error(ErrorCode::Protocol, "Invalid startup factory count");
    startup.extensions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      reader.Record(4);
      startup.extensions.push_back({reader.Text(), reader.Text(), Codec<std::uint32_t>::Read(reader), reader.Text()});
    }
    reader.Finish();
    return startup;
  }
};

inline Bytes ExtensionPayload(std::string_view point, std::uint32_t ordinal) {
  Writer writer;
  writer.Array(2);
  writer.Text(point);
  Codec<std::uint32_t>::Write(writer, ordinal);
  return std::move(writer).Finish();
}

inline void PreparePluginData(const PluginStartup& startup) {
  const auto& text = startup.data_directory;
  const auto path = std::filesystem::path(std::u8string(text.begin(), text.end()));
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) throw Error(ErrorCode::Io, "Cannot create the plugin data directory: " + error.message());
}

inline Bytes ServicePayload(ContractInfo service, std::span<const std::byte> request) {
  // Keep contract identity ahead of the encoded request so the host can authorize before decoding the arguments.
  Writer writer;
  writer.Text(service.id);
  Codec<std::uint32_t>::Write(writer, service.version);
  writer.Text(service.fingerprint);
  auto bytes = std::move(writer).Finish();
  if (request.size() > MaxPayload - bytes.size()) {
    throw Error(ErrorCode::PayloadTooLarge, "Host service request exceeds the byte limit");
  }
  bytes.insert(bytes.end(), request.begin(), request.end());
  return bytes;
}

struct ServiceRequest {
  std::string id;
  std::uint32_t version;
  std::string fingerprint;
  std::span<const std::byte> request;
};

inline ServiceRequest ReadServicePayload(std::span<const std::byte> payload) {
  Reader reader(payload);
  auto id = reader.Text();
  auto version = Codec<std::uint32_t>::Read(reader);
  auto fingerprint = reader.Text();
  return {std::move(id), version, std::move(fingerprint), reader.Remaining()};
}

inline Frame ErrorReply(const Error& error) {
  const std::string_view message(error.what());
  const auto bytes = AsBytes(message.size() <= MaxPayload && IsUtf8(message) ? message : "PluginKit operation failed");
  return {static_cast<std::uint32_t>(error.Code()), 0, 0, Bytes(bytes.begin(), bytes.end())};
}

template<class Write>
void WriteFrame(Write&& write, const Frame& frame) {
  if (frame.payload.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "Worker frame exceeds the byte limit");
  const std::array<std::uint32_t, 10> header{
      FrameMagic, static_cast<std::uint32_t>(frame.kind), frame.call, frame.callback, frame.code, frame.instance,
      frame.operation, static_cast<std::uint32_t>(frame.payload.size()), frame.receiver, frame.synchronous};
  write(std::as_bytes(std::span(header)));
  write(std::span<const std::byte>(frame.payload));
}

template<class Read>
Frame ReadFrame(Read&& read) {
  std::array<std::uint32_t, 10> header{};
  read(std::as_writable_bytes(std::span(header)));
  if (header[0] != FrameMagic || header[7] > MaxPayload || header[9] > 1) {
    throw Error(ErrorCode::Protocol, "Invalid worker frame header");
  }
  Frame frame{header[4], header[5], header[6], Bytes(header[7]), static_cast<FrameKind>(header[1]),
              header[2], header[3], header[8], header[9] != 0};
  read(std::span(frame.payload));
  return frame;
}

} // namespace huxerui::pluginkit::detail
