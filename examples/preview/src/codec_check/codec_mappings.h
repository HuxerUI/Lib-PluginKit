#pragma once

#include "codec_record.h"

#include <bit>

namespace csv_import::codec_check {

template<class T>
using Encoding = T;

using ScaleBits = Encoding<::std::uint64_t>;

} // namespace csv_import::codec_check

namespace huxerui::pluginkit {

template<>
struct WireCodec<csv_import::codec_check::ColumnCount> {
  using WireType = csv_import::codec_check::Count;

  static WireType Encode(const csv_import::codec_check::ColumnCount& columns) { return columns.value; }
  static csv_import::codec_check::ColumnCount Decode(WireType value) { return {value}; }
};

template<>
struct WireCodec<double> {
  using WireType = csv_import::codec_check::ScaleBits;

  static WireType Encode(const double& scale) { return std::bit_cast<WireType>(scale); }
  static double Decode(WireType bits) { return std::bit_cast<double>(bits); }
};

} // namespace huxerui::pluginkit
