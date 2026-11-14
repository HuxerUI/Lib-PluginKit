#pragma once

#include "csv_contract.h"

#include <span>
#include <utility>

namespace csv_import::fixture {

inline constexpr std::size_t CsvChunkSize = 512 * 1024;

using Row = std::vector<std::string>;

inline void AppendCsvRecord(std::string& output, const Row& row) {
  // Quoting every field preserves empty cells, embedded delimiters, and line breaks.
  bool first = true;
  for (const auto& field : row) {
    if (!std::exchange(first, false)) output.push_back(',');
    output.push_back('"');
    for (const char character : field) {
      if (character == '"') output.push_back('"');
      output.push_back(character);
    }
    output.push_back('"');
  }
  output += "\r\n";
}

class CsvRecords {
public:
  CsvRecords(char separator, std::uint32_t limit) : separator_(separator), limit_(limit) {}

  template<class Emit>
  bool Feed(std::span<const std::byte> data, Emit emit) {
    if (data.empty()) return true;
    auto bytes = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
    if (!started_) {
      constexpr std::string_view bom = "\xef\xbb\xbf";
      while (!bytes.empty() && prefix_.size() < bom.size()) {
        prefix_.push_back(bytes.front());
        bytes.remove_prefix(1);
        if (!bom.starts_with(prefix_)) {
          started_ = true;
          for (const auto byte : prefix_) {
            if (!Consume(byte, emit)) return false;
          }
          break;
        }
        if (prefix_.size() == bom.size()) started_ = true;
      }
    }
    for (const auto byte : bytes) {
      if (!Consume(byte, emit)) return false;
    }
    return true;
  }

  template<class Emit>
  bool Finish(Emit emit) {
    if (!started_) {
      for (const auto byte : prefix_) {
        if (!Consume(byte, emit)) return false;
      }
    }
    if (field_ == Field::Quoted) return Fail("csv.unclosed_quote", "Expected a closing quote");
    if (record_started_ && !EndRecord(emit)) return false;
    if (!width_) return Fail("csv.empty", "Expected a header row");
    return true;
  }

  std::uint32_t Rows() const noexcept { return width_ ? record_ - 2 : 0; }
  const Issue& Error() const { return issue_; }

private:
  enum class Field { Start, Plain, Quoted, AfterQuote };

  template<class Emit>
  bool Consume(char byte, Emit& emit) {
    if (std::exchange(skip_lf_, false) && byte == '\n') return true;
    record_started_ = true;
    if (field_ == Field::Quoted) {
      if (byte == '"') field_ = Field::AfterQuote;
      else value_.push_back(byte);
      return true;
    }
    if (byte == '"') {
      if (field_ == Field::Plain) return Fail("csv.unexpected_quote", "A quote must start a field");
      if (field_ == Field::AfterQuote) value_.push_back(byte);
      field_ = Field::Quoted;
      return true;
    }
    if (byte == separator_) return EndField();
    if (byte == '\r' || byte == '\n') {
      skip_lf_ = byte == '\r';
      return EndRecord(emit);
    }
    if (field_ == Field::AfterQuote) {
      return Fail("csv.after_quote", "Expected a separator or line break after the closing quote");
    }
    value_.push_back(byte);
    field_ = Field::Plain;
    return true;
  }

  bool EndField() {
    if (++cells_ > limit_) return Fail("csv.cell_limit", "Too many cells");
    row_.push_back(std::exchange(value_, {}));
    field_ = Field::Start;
    return true;
  }

  template<class Emit>
  bool EndRecord(Emit& emit) {
    if (!EndField()) return false;
    if (!width_) width_ = row_.size();
    if (row_.size() != width_) return Fail("csv.column_count", "The row width differs from the header", 1);
    emit(std::exchange(row_, {}), record_ == 1);
    ++record_;
    record_started_ = false;
    return true;
  }

  bool Fail(std::string code, std::string message, std::uint32_t column = 0) {
    issue_ = {std::move(code), record_, column ? column : static_cast<std::uint32_t>(row_.size() + 1),
              std::move(message)};
    return false;
  }

  const char separator_;
  const std::uint32_t limit_;
  Field field_ = Field::Start;
  std::uint32_t record_ = 1;
  std::uint32_t cells_ = 0;
  std::size_t width_ = 0;
  bool started_ = false;
  bool record_started_ = false;
  bool skip_lf_ = false;
  std::string prefix_;
  std::string value_;
  Row row_;
  Issue issue_;
};

} // namespace csv_import::fixture
