#ifndef BOOKREPLAY_DBN_READER_HPP
#define BOOKREPLAY_DBN_READER_HPP

#include "bookreplay/dbn.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bookreplay {

/// Thrown on any malformed input; a wrong file is never half-decoded.
class DbnError : public BookreplayError {
 public:
  using BookreplayError::BookreplayError;
};

struct MappingInterval {
  std::uint32_t start_date;  ///< YYYYMMDD as a decimal integer
  std::uint32_t end_date;
  std::string symbol;
};

/// How a requested symbol resolved over time: a continuous contract like
/// `MNQ.v.0` maps to a different instrument_id on either side of a roll.
struct SymbolMapping {
  std::string raw_symbol;
  std::vector<MappingInterval> intervals;
};

struct DbnMetadata {
  std::uint8_t version;
  std::string dataset;
  std::optional<std::uint16_t> schema;  ///< absent when the stream mixes schemas
  std::uint64_t start;
  std::optional<std::uint64_t> end;    ///< absent when the capture was never closed
  std::optional<std::uint64_t> limit;  ///< absent when the request had no record cap
  std::optional<std::uint8_t> stype_in;
  std::uint8_t stype_out;
  bool ts_out;  ///< when set, every record carries 8 extra bytes
  std::uint16_t symbol_cstr_len;
  std::vector<std::string> symbols;
  std::vector<std::string> partial;
  std::vector<std::string> not_found;
  std::vector<SymbolMapping> mappings;
};

class DbnReader {
 public:
  explicit DbnReader(const std::filesystem::path& path);

  /// Decodes an in-memory stream; the bytes must outlive the reader.
  DbnReader(const std::byte* data, std::size_t size);

  ~DbnReader();
  DbnReader(DbnReader&&) noexcept;
  DbnReader& operator=(DbnReader&&) noexcept;
  DbnReader(const DbnReader&) = delete;
  DbnReader& operator=(const DbnReader&) = delete;

  [[nodiscard]] const DbnMetadata& metadata() const noexcept;

  /// The next record, or nullptr at a clean end of stream; throws DbnError if
  /// the stream ends mid-record or the record is not well formed. The pointer
  /// aims into the reader's own buffer and is invalidated by the next call.
  [[nodiscard]] const RecordHeader* next();

  [[nodiscard]] std::uint64_t record_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_DBN_READER_HPP
