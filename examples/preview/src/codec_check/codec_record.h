#pragma once

#include <huxerui/pluginkit/contract.h>
#include <huxerui/stream.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace csv_import::codec_check {

using Count = std::uint32_t;

struct ColumnCount {
  Count value;
};

using ColumnCounts = std::vector<std::optional<ColumnCount>>;

class HPK_REMOTE_CONTRACT("example.codec-check", 1) RecordColumns {
public:
  virtual ~RecordColumns() = default;
  virtual ColumnCounts Write(ColumnCounts columns, huxerui::OutputStream table, huxerui::OutputStream report) = 0;
};

class HPK_REMOTE_CONTRACT("example.codec-check", 1) ReorderedColumns {
public:
  virtual ~ReorderedColumns() = default;
  virtual ColumnCounts Write(ColumnCounts columns, huxerui::OutputStream report, huxerui::OutputStream table) = 0;
};

} // namespace csv_import::codec_check
