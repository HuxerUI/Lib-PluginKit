#pragma once

#include <codec_mappings.h>

namespace csv_import::codec_check {

using BorrowedColumns = const ColumnCounts&;

class HPK_REMOTE MappedColumns {
public:
  virtual ~MappedColumns() = default;
  virtual ColumnCounts Scale(BorrowedColumns columns, double scale) = 0;
};

} // namespace csv_import::codec_check
