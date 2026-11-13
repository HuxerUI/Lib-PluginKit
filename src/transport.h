#pragma once

#include "bytes.h"

#include <array>
#include <chrono>

namespace pluginkit::detail {

enum class Command : std::uint32_t { Open = 1, Invoke, Close };

struct Frame {
  std::uint32_t code;
  std::uint32_t instance = 0;
  std::uint32_t operation = 0;
  Bytes payload;
};

inline constexpr std::uint32_t FrameMagic = 0x314b5048; // HPK1 in little-endian byte order.
using Deadline = std::chrono::steady_clock::time_point;

template<class Write>
void WriteFrame(Write&& write, const Frame& frame) {
  if (frame.payload.size() > MaxPayload) throw Error(ErrorCode::PayloadTooLarge, "Worker frame exceeds the byte limit");
  const std::array<std::uint32_t, 5> header{
      FrameMagic, frame.code, frame.instance, frame.operation, static_cast<std::uint32_t>(frame.payload.size())};
  write(ByteView{reinterpret_cast<const std::uint8_t*>(header.data()), sizeof(header)});
  write(ByteView(frame.payload));
}

template<class Read>
Frame ReadFrame(Read&& read) {
  std::array<std::uint32_t, 5> header{};
  read(std::span{reinterpret_cast<std::uint8_t*>(header.data()), sizeof(header)});
  if (header[0] != FrameMagic || header[4] > MaxPayload) throw Error(ErrorCode::Protocol, "Invalid worker frame header");
  Frame frame{header[1], header[2], header[3], Bytes(header[4])};
  read(std::span(frame.payload));
  return frame;
}

} // namespace pluginkit::detail
