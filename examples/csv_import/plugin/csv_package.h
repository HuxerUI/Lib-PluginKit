#pragma once

#include "exporters/export_contract.bindings.h"
#include "csv_records.h"
#include "process_id.h"

#include <csv_resources_resources.h>
#include <huxerui/pluginkit/plugin.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <stdexcept>

namespace csv_import::fixture {

inline const huxerui::pluginkit::PluginContext* active_context = nullptr;
inline std::uint32_t live_exporters = 0;

inline std::uint32_t ReadCellLimit(const huxerui::RawAsset& asset) {
  const auto text = asset.ReadString();
  std::string_view value_text(text);
  while (!value_text.empty() && (value_text.back() == '\r' || value_text.back() == '\n')) value_text.remove_suffix(1);
  std::uint32_t value = 0;
  const auto end = value_text.data() + value_text.size();
  const auto parsed = std::from_chars(value_text.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !value) {
    throw std::runtime_error("Invalid CSV cell limit resource");
  }
  return value;
}

inline void Record(const huxerui::pluginkit::PluginContext& context, std::string_view event) {
  if (!context.DataDirectory().Child("lifecycle.log").AppendString(std::string(event) + '\n')) {
    throw std::runtime_error("Cannot write the CSV package lifecycle log");
  }
}

template<class T>
T RequireIo(huxerui::IoResult<T> result) {
  if (!result.Succeeded()) {
    throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::Io, result.Error().message);
  }
  return std::move(result).Value();
}

inline huxerui::Task<huxerui::Bytes> ReadImportChunk(huxerui::AsyncInputStream& input, std::size_t& remaining) {
  // One extra byte distinguishes a file at the limit from one that still has data.
  auto bytes = RequireIo(co_await input.ReadAsync(std::min(CsvChunkSize, remaining + 1)));
  if (bytes.size() > remaining) {
    throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::PayloadTooLarge, "CSV input exceeds the example limit");
  }
  remaining -= bytes.size();
  co_return bytes;
}

inline std::span<const std::byte> CsvBytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

inline std::size_t ConversionChunk(std::uint32_t read_bytes) {
  if (!read_bytes || read_bytes > HPK_MAX_STREAM_CHUNK) {
    throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::InvalidArgument, "Invalid CSV conversion read size");
  }
  return read_bytes;
}

inline huxerui::Bytes TakeBytes(std::string& text) {
  huxerui::Bytes bytes(text.size());
  if (!text.empty()) std::memcpy(bytes.data(), text.data(), text.size());
  text.clear();
  return bytes;
}

struct CsvConversion {
  CsvConversion(char separator, std::uint32_t limit) : records(separator, limit) {}

  void Feed(std::span<const std::byte> bytes) {
    summary.input_bytes += bytes.size();
    RequireParsed(records.Feed(bytes, [this](Row row, bool) { AppendCsvRecord(buffer, row); }));
    summary.rows = records.Rows();
  }

  void Finish() {
    RequireParsed(records.Finish([this](Row row, bool) { AppendCsvRecord(buffer, row); }));
    summary.rows = records.Rows();
  }

  CsvRecords records;
  CsvSummary summary{};
  std::string buffer;

private:
  void RequireParsed(bool ok) const {
    if (!ok) throw huxerui::pluginkit::Error(huxerui::pluginkit::ErrorCode::InvalidArgument, records.Error().message);
  }
};

class CsvExporter : public TableExporter {
public:
  explicit CsvExporter(const huxerui::pluginkit::PluginContext& context) {
    if (active_context != &context) throw std::runtime_error("Expected the activation's context");
#if defined(CSV_IMPORT_UPGRADE)
    if (context.DataDirectory().Child("fail-exporter").Exists()) {
      throw std::runtime_error("The CSV candidate could not construct its exporter");
    }
#endif
    cell_limit_ = ReadCellLimit(context.Resources().GetRawResource(csv_resources::raw::cell_limit_txt));
    ++live_exporters;
  }

  ~CsvExporter() override { --live_exporters; }

  std::string Format() const override { return "csv"; }
  std::uint32_t ProcessId() const override { return csv_import::ProcessId(); }

  huxerui::Task<void> Export(ExportRequest request, huxerui::AsyncOutputStream output) override {
    std::string buffer;
    const auto append = [&](const Row& row) {
      if (row.size() != request.header.size() || row.empty()) {
        throw std::runtime_error("CSV records must match the header width");
      }
      AppendCsvRecord(buffer, row);
    };
    append(request.header);
    for (const auto& row : request.rows) {
      append(row);
      if (buffer.size() >= CsvChunkSize) RequireIo(co_await output.WriteAsync(TakeBytes(buffer)));
    }
    if (!buffer.empty()) RequireIo(co_await output.WriteAsync(TakeBytes(buffer)));
    RequireIo(co_await output.CloseAsync());
  }

  CsvSummary Convert(ConversionOptions options, huxerui::InputStream input, huxerui::OutputStream output) override {
    return ConvertOne(std::move(input), ';', std::move(output), options.read_bytes);
  }

  huxerui::Task<CsvSummary> ConvertAsync(ConversionOptions options, huxerui::AsyncInputStream input,
      huxerui::AsyncOutputStream output, std::shared_ptr<ImportProgress> progress) override {
    return ConvertOneAsync(std::move(input), ';', std::move(output), options.read_bytes, std::move(progress));
  }

  PairSummary ConvertPair(huxerui::InputStream first, const Separator& separator,
      huxerui::OutputStream first_csv, std::uint32_t read_bytes,
      huxerui::OutputStream second_csv, huxerui::InputStream second) override {
    const auto a = ConvertOne(std::move(first), separator.Text().front(), std::move(first_csv), read_bytes);
    const auto b = ConvertOne(std::move(second), separator.Text().front(), std::move(second_csv), read_bytes);
    return {a, b, separator};
  }

  huxerui::Task<PairSummary> ConvertPairAsync(huxerui::AsyncInputStream first, Separator separator,
      std::shared_ptr<ImportProgress> first_progress, huxerui::AsyncOutputStream first_csv,
      std::uint32_t read_bytes, huxerui::AsyncOutputStream second_csv,
      std::shared_ptr<ImportProgress> second_progress, huxerui::AsyncInputStream second) override {
    const auto a = co_await ConvertOneAsync(
        std::move(first), separator.Text().front(), std::move(first_csv), read_bytes, std::move(first_progress));
    const auto b = co_await ConvertOneAsync(
        std::move(second), separator.Text().front(), std::move(second_csv), read_bytes, std::move(second_progress));
    co_return PairSummary{a, b, std::move(separator)};
  }

private:
  CsvSummary ConvertOne(huxerui::InputStream input, char separator, huxerui::OutputStream output,
                        std::uint32_t read_bytes) {
    CsvConversion conversion(separator, cell_limit_);
    huxerui::Bytes bytes(ConversionChunk(read_bytes));
    const auto flush = [&] {
      RequireIo(output.Write(CsvBytes(conversion.buffer)));
      conversion.summary.output_bytes += conversion.buffer.size();
      conversion.buffer.clear();
    };
    while (const auto count = RequireIo(input.Read(bytes))) {
      conversion.Feed(std::span(bytes).first(count));
      if (conversion.buffer.size() >= CsvChunkSize) flush();
    }
    conversion.Finish();
    flush();
    RequireIo(output.Close());
    return conversion.summary;
  }

  huxerui::Task<CsvSummary> ConvertOneAsync(huxerui::AsyncInputStream input, char separator,
      huxerui::AsyncOutputStream output, std::uint32_t read_bytes, std::shared_ptr<ImportProgress> progress) {
    CsvConversion conversion(separator, cell_limit_);
    const auto chunk = ConversionChunk(read_bytes);
    const auto flush = [&]() -> huxerui::Task<void> {
      const auto size = conversion.buffer.size();
      if (size) RequireIo(co_await output.WriteAsync(TakeBytes(conversion.buffer)));
      conversion.summary.output_bytes += size;
    };
    std::uint32_t reported = 0;
    for (;;) {
      const auto bytes = RequireIo(co_await input.ReadAsync(chunk));
      if (bytes.empty()) break;
      conversion.Feed(bytes);
      if (conversion.buffer.size() >= CsvChunkSize) co_await flush();
      if (progress && reported != conversion.summary.rows) {
        reported = conversion.summary.rows;
        co_await progress->OnRows(reported, conversion.summary.input_bytes);
      }
    }
    conversion.Finish();
    co_await flush();
    if (progress && reported != conversion.summary.rows) {
      co_await progress->OnRows(conversion.summary.rows, conversion.summary.input_bytes);
    }
    RequireIo(co_await output.CloseAsync());
    co_return conversion.summary;
  }

  std::uint32_t cell_limit_;
};

} // namespace csv_import::fixture
