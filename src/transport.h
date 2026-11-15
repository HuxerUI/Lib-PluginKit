#pragma once

#include "bytes.h"

#include <huxerui/pluginkit/plugin.h>

#include <array>
#include <chrono>

namespace huxerui::pluginkit::detail {

enum class Command : std::uint32_t { Activate = 1, Create, Invoke, Close, Shutdown };
enum class FrameKind : std::uint32_t { Request = 1, Reply, ServiceCall, ServiceReply, StreamCall, StreamReply };

struct Frame {
  std::uint32_t code;
  std::uint32_t instance = 0;
  std::uint32_t operation = 0;
  Bytes payload;
  FrameKind kind = FrameKind::Request;
  std::uint32_t call = 0;
  std::uint32_t callback = 0;
  // Reverse calls carry a one-based parameter slot. Zero identifies a registered host service.
  std::uint32_t slot = 0;
  bool synchronous = false;
  std::uint32_t read_limit = 0;
};

inline constexpr std::uint32_t FrameMagic = 0x314b5048; // HPK1 in little-endian byte order.
using Deadline = std::chrono::steady_clock::time_point;

inline bool IsErrorCode(std::uint32_t code) {
  return code >= static_cast<std::uint32_t>(ErrorCode::InvalidArgument) &&
         code <= static_cast<std::uint32_t>(ErrorCode::Ambiguous);
}

struct ExpectedExtension {
  std::string point;
  std::uint32_t ordinal;
  std::string contract_id;
  std::uint32_t contract_version;
  std::string contract_fingerprint;

  ContractInfo Info() const noexcept { return {contract_id, contract_version, contract_fingerprint}; }
};

struct PluginStartup {
  PluginIdentity identity;
  std::string bundle_directory;
  std::string data_directory;
  std::vector<ExpectedExtension> extensions;

  Bytes Encode() const {
    Writer writer;
    writer.Array(6);
    writer.Text(identity.id);
    writer.Text(identity.version);
    writer.Text(identity.digest);
    writer.Text(bundle_directory);
    writer.Text(data_directory);
    writer.Array(extensions.size());
    for (const auto& extension : extensions) {
      writer.Array(5);
      writer.Text(extension.point);
      Codec<std::uint32_t>::Write(writer, extension.ordinal);
      writer.Text(extension.contract_id);
      Codec<std::uint32_t>::Write(writer, extension.contract_version);
      writer.Text(extension.contract_fingerprint);
    }
    return std::move(writer).Finish();
  }

  static PluginStartup Decode(std::span<const std::byte> payload) {
    Reader reader(payload);
    reader.Record(6);
    PluginStartup startup{{reader.Text(), reader.Text(), reader.Text()}, reader.Text(), reader.Text(), {}};
    const auto count = reader.Array();
    if (!count || count > HPK_MAX_EXTENSIONS) throw Error(ErrorCode::Protocol, "Invalid startup factory count");
    startup.extensions.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      reader.Record(5);
      startup.extensions.push_back({reader.Text(), Codec<std::uint32_t>::Read(reader), reader.Text(),
                                    Codec<std::uint32_t>::Read(reader), reader.Text()});
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

inline std::size_t FramePayloadLimit(FrameKind kind) noexcept {
  return kind == FrameKind::StreamCall || kind == FrameKind::StreamReply ? MaxStreamChunk : MaxPayload;
}

inline bool IsStreamOperation(std::uint32_t operation, std::uint32_t count, std::size_t payload_size) noexcept {
  if (operation == HPK_READ_STREAM) return count > 0 && count <= MaxStreamChunk && !payload_size;
  if (count) return false;
  return (operation == HPK_WRITE_STREAM && payload_size > 0 && payload_size <= MaxStreamChunk) ||
         (operation == HPK_CLOSE_STREAM && !payload_size);
}

template<class Write>
void WriteFrame(Write&& write, const Frame& frame) {
  if (frame.payload.size() > FramePayloadLimit(frame.kind)) {
    throw Error(ErrorCode::PayloadTooLarge, "Worker frame exceeds the byte limit");
  }
  const std::array<std::uint32_t, 11> header{
      FrameMagic, static_cast<std::uint32_t>(frame.kind), frame.call, frame.callback, frame.code, frame.instance,
      frame.operation, static_cast<std::uint32_t>(frame.payload.size()), frame.slot, frame.synchronous,
      frame.read_limit};
  write(std::as_bytes(std::span(header)));
  write(std::span<const std::byte>(frame.payload));
}

template<class Read>
Frame ReadFrame(Read&& read) {
  std::array<std::uint32_t, 11> header{};
  read(std::as_writable_bytes(std::span(header)));
  const auto kind = static_cast<FrameKind>(header[1]);
  if (header[0] != FrameMagic || kind < FrameKind::Request || kind > FrameKind::StreamReply ||
      header[7] > FramePayloadLimit(kind) || header[9] > 1 ||
      (kind == FrameKind::StreamCall && (!header[8] || !IsStreamOperation(header[6], header[10], header[7]))) ||
      (kind != FrameKind::StreamCall && header[10])) {
    throw Error(ErrorCode::Protocol, "Invalid worker frame header");
  }
  Frame frame{header[4], header[5], header[6], Bytes(header[7]), kind,
              header[2], header[3], header[8], header[9] != 0, header[10]};
  read(std::span(frame.payload));
  return frame;
}

} // namespace huxerui::pluginkit::detail
