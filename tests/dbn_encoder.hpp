// A DBN encoder, for tests only.
//
// It exists so property tests can generate streams the decoder has never
// seen and prove they survive a round trip, and so the malformed-input tests
// can start from a valid stream and break exactly one thing. It is never
// linked into the shipping library, which only ever reads.

#ifndef BOOKREPLAY_TESTS_DBN_ENCODER_HPP
#define BOOKREPLAY_TESTS_DBN_ENCODER_HPP

#include "bookreplay/dbn.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <zstd.h>

namespace bookreplay::testing {

inline void set_cstr(std::span<char> field, std::string_view text) {
  if (text.size() >= field.size()) {
    throw std::runtime_error("string does not fit its fixed-width DBN field");
  }
  std::fill(field.begin(), field.end(), '\0');
  std::copy(text.begin(), text.end(), field.begin());
}

/// Mirrors DbnMetadata's variable-length tail on the encoding side.
struct EncodedMapping {
  std::string raw_symbol;
  std::uint32_t start_date = 0;
  std::uint32_t end_date = 0;
  std::string symbol;
};

class DbnEncoder {
 public:
  DbnEncoder& version(std::uint8_t v) {
    version_ = v;
    return *this;
  }

  DbnEncoder& dataset(std::string v) {
    dataset_ = std::move(v);
    return *this;
  }

  DbnEncoder& schema(std::optional<std::uint16_t> v) {
    schema_ = v;
    return *this;
  }

  DbnEncoder& window(std::uint64_t start, std::uint64_t end) {
    start_ = start;
    end_ = end;
    return *this;
  }

  DbnEncoder& ts_out(bool on, std::uint64_t value = 0) {
    ts_out_ = on;
    ts_out_value_ = value;
    return *this;
  }

  DbnEncoder& symbol(std::string s) {
    symbols_.push_back(std::move(s));
    return *this;
  }

  DbnEncoder& symbol_cstr_len(std::uint16_t v) {
    symbol_cstr_len_ = v;
    return *this;
  }

  DbnEncoder& mapping(EncodedMapping m) {
    mappings_.push_back(std::move(m));
    return *this;
  }

  /// Appends a record, filling in the rtype and length the wire format
  /// requires. Callers testing malformed streams use add_raw instead.
  template <typename T>
  DbnEncoder& add(const T& record) {
    static_assert(kIsDbnRecord<T>);
    T copy = record;
    copy.hd.rtype = RecordTraits<T>::kRType;
    copy.hd.length = static_cast<std::uint8_t>(kLengthUnits<T> +
                                               (ts_out_ ? sizeof(std::uint64_t) / kLengthUnit : 0));
    append_bytes(records_, &copy, sizeof(T));
    if (ts_out_) {
      append(records_, ts_out_value_);
    }
    return *this;
  }

  /// Appends bytes verbatim, however wrong they are.
  DbnEncoder& add_raw(const std::vector<std::byte>& bytes) {
    records_.insert(records_.end(), bytes.begin(), bytes.end());
    return *this;
  }

  [[nodiscard]] std::vector<std::byte> encode() const {
    std::vector<std::byte> meta;
    append_cstr(meta, dataset_, kDatasetCstrLen);
    append(meta, schema_.value_or(kNullSchema));
    append(meta, start_);
    append(meta, end_);
    append(meta, limit_);
    append(meta, stype_in_.value_or(kNullSType));
    append(meta, stype_out_);
    append(meta, static_cast<std::uint8_t>(ts_out_ ? 1 : 0));
    append(meta, symbol_cstr_len_);
    meta.resize(meta.size() + kMetadataReservedLen);
    append(meta, std::uint32_t{0});  // schema_definition_length

    append_symbols(meta, symbols_);
    append_symbols(meta, partial_);
    append_symbols(meta, not_found_);
    append_mappings(meta);

    // DBN v3 encodes the metadata frame at a length divisible by 8, so that
    // records land aligned even before the reader shifts its buffer.
    while (meta.size() % 8 != 0) {
      meta.push_back(std::byte{0});
    }

    std::vector<std::byte> out;
    out.reserve(kPreludeSize + meta.size() + records_.size());
    out.push_back(static_cast<std::byte>('D'));
    out.push_back(static_cast<std::byte>('B'));
    out.push_back(static_cast<std::byte>('N'));
    out.push_back(static_cast<std::byte>(version_));
    append(out, static_cast<std::uint32_t>(meta.size()));
    out.insert(out.end(), meta.begin(), meta.end());
    out.insert(out.end(), records_.begin(), records_.end());
    return out;
  }

  [[nodiscard]] std::vector<std::byte> encode_compressed(bool checksum = false) const {
    return compress(encode(), checksum);
  }

  /// `checksum` writes zstd's optional frame checksum, which is what makes a
  /// single flipped byte reliably detectable rather than merely likely.
  static std::vector<std::byte> compress(const std::vector<std::byte>& in, bool checksum = false) {
    const std::size_t bound = ZSTD_compressBound(in.size());
    std::vector<std::byte> out(bound);

    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    if (ctx == nullptr) {
      throw std::runtime_error("could not allocate a zstd compression context");
    }
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, 3);
    if (checksum) {
      ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1);
    }
    const std::size_t written = ZSTD_compress2(ctx, out.data(), bound, in.data(), in.size());
    ZSTD_freeCCtx(ctx);

    if (ZSTD_isError(written) != 0) {
      throw std::runtime_error(ZSTD_getErrorName(written));
    }
    out.resize(written);
    return out;
  }

 private:
  static constexpr std::size_t kPreludeSize = 8;
  static constexpr std::size_t kDatasetCstrLen = 16;
  static constexpr std::size_t kMetadataReservedLen = 53;
  static constexpr std::uint16_t kNullSchema = 0xFFFF;
  static constexpr std::uint8_t kNullSType = 0xFF;

  static void append_bytes(std::vector<std::byte>& out, const void* src, std::size_t n) {
    const std::size_t start = out.size();
    out.resize(start + n);
    std::memcpy(out.data() + start, src, n);
  }

  template <typename T>
  static void append(std::vector<std::byte>& out, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    append_bytes(out, &value, sizeof(T));
  }

  static void append_cstr(std::vector<std::byte>& out, const std::string& s, std::size_t width) {
    const std::size_t start = out.size();
    out.resize(start + width);
    set_cstr({reinterpret_cast<char*>(out.data() + start), width}, s);
  }

  void append_symbols(std::vector<std::byte>& out, const std::vector<std::string>& list) const {
    append(out, static_cast<std::uint32_t>(list.size()));
    for (const std::string& s : list) {
      append_cstr(out, s, symbol_cstr_len_);
    }
  }

  void append_mappings(std::vector<std::byte>& out) const {
    append(out, static_cast<std::uint32_t>(mappings_.size()));
    for (const EncodedMapping& m : mappings_) {
      append_cstr(out, m.raw_symbol, symbol_cstr_len_);
      append(out, std::uint32_t{1});  // one interval per mapping is enough here
      append(out, m.start_date);
      append(out, m.end_date);
      append_cstr(out, m.symbol, symbol_cstr_len_);
    }
  }

  std::uint8_t version_ = kSupportedDbnVersion;
  std::string dataset_ = "GLBX.MDP3";
  std::optional<std::uint16_t> schema_ = std::uint16_t{0};
  std::uint64_t start_ = 1'785'888'000'000'000'000ULL;
  std::uint64_t end_ = 1'785'974'400'000'000'000ULL;
  std::uint64_t limit_ = 0;
  std::optional<std::uint8_t> stype_in_ = std::uint8_t{4};
  std::uint8_t stype_out_ = 0;
  bool ts_out_ = false;
  std::uint64_t ts_out_value_ = 0;
  std::uint16_t symbol_cstr_len_ = static_cast<std::uint16_t>(kSymbolCstrLen);
  std::vector<std::string> symbols_;
  std::vector<std::string> partial_;
  std::vector<std::string> not_found_;
  std::vector<EncodedMapping> mappings_;
  std::vector<std::byte> records_;
};

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_DBN_ENCODER_HPP
