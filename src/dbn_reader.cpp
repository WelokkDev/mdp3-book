#include "bookreplay/dbn_reader.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <new>
#include <utility>
#include <zstd.h>

#include "hex_byte.hpp"

namespace bookreplay {
namespace {

static_assert(std::endian::native == std::endian::little,
              "DBN is little-endian on the wire; a big-endian host needs byte swaps that "
              "this decoder does not implement");

constexpr std::size_t kMagicSize = 4;
constexpr std::size_t kPreludeSize = 8;
constexpr std::size_t kFixedMetadataLen = 100;
constexpr std::size_t kDatasetCstrLen = 16;
constexpr std::size_t kMetadataReservedLen = 53;
constexpr std::uint16_t kNullSchema = 0xFFFF;
constexpr std::uint8_t kNullSType = 0xFF;

/// A full CME definition day carries on the order of 100k symbols at 71 bytes
/// each; 64 MiB is more than an order of magnitude above that.
constexpr std::uint32_t kMaxMetadataLen = 64U << 20;

constexpr std::uint32_t kZstdMagic = 0xFD2FB528U;
constexpr std::uint32_t kZstdSkippableMask = 0xFFFFFFF0U;
constexpr std::uint32_t kZstdSkippableBase = 0x184D2A50U;

constexpr std::size_t kBufferAlign = 64;
constexpr std::size_t kInitialBufferSize = std::size_t{1} << 18;
constexpr std::size_t kSourceChunk = std::size_t{1} << 18;

struct AlignedDelete {
  void operator()(std::byte* p) const noexcept {
    ::operator delete(p, std::align_val_t{kBufferAlign});
  }
};

using AlignedBuffer = std::unique_ptr<std::byte[], AlignedDelete>;

AlignedBuffer allocate_aligned(std::size_t n) {
  return AlignedBuffer{static_cast<std::byte*>(::operator new(n, std::align_val_t{kBufferAlign}))};
}

template <typename T>
T take(const std::byte*& p, const std::byte* end) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (static_cast<std::size_t>(end - p) < sizeof(T)) {
    throw DbnError("DBN metadata ends mid-field");
  }
  T value;
  std::memcpy(&value, p, sizeof(T));
  p += sizeof(T);
  return value;
}

std::string take_cstr(const std::byte*& p, const std::byte* end, std::size_t width,
                      const char* what) {
  if (static_cast<std::size_t>(end - p) < width) {
    throw DbnError(std::string{"DBN metadata ends mid-"} + what);
  }
  const auto* s = reinterpret_cast<const char*>(p);
  const auto len = static_cast<std::size_t>(std::find(s, s + width, '\0') - s);
  if (len == width) {
    throw DbnError(std::string{"DBN metadata "} + what + " has no null terminator");
  }
  p += width;
  return std::string{s, len};
}

}  // namespace

struct DbnReader::Impl {
  std::ifstream file;
  bool from_memory = false;
  const std::byte* mem = nullptr;
  std::size_t mem_size = 0;
  std::size_t mem_pos = 0;

  ZSTD_DStream* dstream = nullptr;
  std::size_t zstd_pending = 0;  ///< nonzero means the current frame is unfinished
  std::vector<std::byte> src;
  std::size_t src_pos = 0;
  std::size_t src_len = 0;
  bool source_eof = false;

  AlignedBuffer buf;
  std::size_t cap = 0;
  std::size_t rpos = 0;
  std::size_t wpos = 0;

  DbnMetadata meta{};
  std::uint64_t count = 0;

  ~Impl() {
    if (dstream != nullptr) {
      ZSTD_freeDStream(dstream);
    }
  }

  [[nodiscard]] std::size_t available() const noexcept { return wpos - rpos; }

  std::size_t raw_read(std::byte* dst, std::size_t n) {
    if (from_memory) {
      const std::size_t take_n = std::min(n, mem_size - mem_pos);
      if (take_n != 0) {
        std::memcpy(dst, mem + mem_pos, take_n);
        mem_pos += take_n;
      }
      return take_n;
    }
    file.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (file.bad()) {
      throw DbnError("I/O error reading the DBN file");
    }
    return static_cast<std::size_t>(file.gcount());
  }

  void shift() noexcept {
    if (rpos == 0) {
      return;
    }
    const std::size_t n = available();
    if (n != 0) {
      std::memmove(buf.get(), buf.get() + rpos, n);
    }
    rpos = 0;
    wpos = n;
  }

  void grow(std::size_t need) {
    std::size_t new_cap = cap == 0 ? kInitialBufferSize : cap;
    while (new_cap < need) {
      new_cap *= 2;
    }
    AlignedBuffer next = allocate_aligned(new_cap);
    const std::size_t n = available();
    if (n != 0) {
      std::memcpy(next.get(), buf.get() + rpos, n);
    }
    buf = std::move(next);
    cap = new_cap;
    rpos = 0;
    wpos = n;
  }

  void reserve_for(std::size_t need) {
    if (cap - rpos >= need) {
      return;
    }
    shift();
    if (cap < need) {
      grow(need);
    }
  }

  /// Returns 0 only at the end of the source.
  std::size_t fill() {
    if (wpos == cap) {
      return 0;
    }
    const std::size_t before = wpos;
    while (wpos == before) {
      if (src_pos == src_len) {
        if (source_eof) {
          break;
        }
        src_len = raw_read(src.data(), src.size());
        src_pos = 0;
        if (src_len == 0) {
          source_eof = true;
          break;
        }
      }
      if (dstream != nullptr) {
        ZSTD_inBuffer in{src.data(), src_len, src_pos};
        ZSTD_outBuffer out{buf.get(), cap, wpos};
        const std::size_t ret = ZSTD_decompressStream(dstream, &out, &in);
        if (ZSTD_isError(ret) != 0) {
          throw DbnError(std::string{"zstd decompression failed: "} + ZSTD_getErrorName(ret));
        }
        zstd_pending = ret;
        src_pos = in.pos;
        wpos = out.pos;
        if (out.pos == out.size) {
          break;
        }
      } else {
        const std::size_t take_n = std::min(src_len - src_pos, cap - wpos);
        std::memcpy(buf.get() + wpos, src.data() + src_pos, take_n);
        src_pos += take_n;
        wpos += take_n;
      }
    }
    if (wpos == before && source_eof && dstream != nullptr && zstd_pending != 0) {
      throw DbnError("the zstd stream ends inside a frame; the file is truncated");
    }
    return wpos - before;
  }

  void require(std::size_t n, const char* what) {
    while (available() < n) {
      reserve_for(n);
      if (fill() == 0) {
        throw DbnError(std::string{"the DBN stream ends before the "} + what + " is complete");
      }
    }
  }

  void open_source() {
    src.resize(kSourceChunk);
    src_len = raw_read(src.data(), src.size());
    src_pos = 0;
    if (src_len < kMagicSize) {
      throw DbnError("input is too short to be a DBN file");
    }

    if (std::memcmp(src.data(), "DBN", 3) == 0) {
      return;
    }

    std::uint32_t magic = 0;
    std::memcpy(&magic, src.data(), sizeof(magic));
    if (magic == kZstdMagic) {
      dstream = ZSTD_createDStream();
      if (dstream == nullptr) {
        throw DbnError("could not allocate a zstd decompression stream");
      }
      ZSTD_initDStream(dstream);
      return;
    }
    if ((magic & kZstdSkippableMask) == kZstdSkippableBase) {
      throw DbnError(
          "this is a legacy DBZ file, not DBN; convert it with the dbn CLI before reading");
    }
    throw DbnError("input is neither DBN nor zstd-framed DBN");
  }

  void decode_metadata() {
    require(kPreludeSize, "header");
    const std::byte* p = buf.get() + rpos;
    if (std::memcmp(p, "DBN", 3) != 0) {
      throw DbnError("missing DBN magic at the start of the stream");
    }
    const auto version = static_cast<std::uint8_t>(p[3]);
    std::uint32_t frame_size = 0;
    std::memcpy(&frame_size, p + kMagicSize, sizeof(frame_size));
    rpos += kPreludeSize;

    if (!is_supported_dbn_version(version)) {
      throw DbnError("DBN version " + std::to_string(version) +
                     " is not supported; this decoder targets version " +
                     std::to_string(kSupportedDbnVersion) +
                     " only, and the record layouts differ between versions");
    }
    if (frame_size < kFixedMetadataLen) {
      throw DbnError("metadata frame is " + std::to_string(frame_size) +
                     " bytes, shorter than the " + std::to_string(kFixedMetadataLen) +
                     "-byte fixed block");
    }
    if (frame_size > kMaxMetadataLen) {
      throw DbnError("metadata frame claims " + std::to_string(frame_size) + " bytes, beyond the " +
                     std::to_string(kMaxMetadataLen) +
                     "-byte ceiling this decoder will allocate for one");
    }

    require(frame_size, "metadata frame");
    const std::byte* q = buf.get() + rpos;
    const std::byte* const end = q + frame_size;

    meta.version = version;
    meta.dataset = take_cstr(q, end, kDatasetCstrLen, "dataset");
    const auto raw_schema = take<std::uint16_t>(q, end);
    meta.schema =
        raw_schema == kNullSchema ? std::nullopt : std::optional<std::uint16_t>{raw_schema};
    meta.start = take<std::uint64_t>(q, end);
    const auto raw_end = take<std::uint64_t>(q, end);
    meta.end = raw_end == kUndefTimestamp ? std::nullopt : std::optional<std::uint64_t>{raw_end};
    const auto raw_limit = take<std::uint64_t>(q, end);
    meta.limit = raw_limit == 0 ? std::nullopt : std::optional<std::uint64_t>{raw_limit};
    const auto raw_stype_in = take<std::uint8_t>(q, end);
    meta.stype_in =
        raw_stype_in == kNullSType ? std::nullopt : std::optional<std::uint8_t>{raw_stype_in};
    meta.stype_out = take<std::uint8_t>(q, end);
    meta.ts_out = take<std::uint8_t>(q, end) != 0;
    meta.symbol_cstr_len = take<std::uint16_t>(q, end);
    if (meta.symbol_cstr_len != kSymbolCstrLen) {
      throw DbnError("metadata declares a symbol width of " + std::to_string(meta.symbol_cstr_len) +
                     "; DBN version " + std::to_string(kSupportedDbnVersion) + " uses " +
                     std::to_string(kSymbolCstrLen));
    }
    if (static_cast<std::size_t>(end - q) < kMetadataReservedLen) {
      throw DbnError("DBN metadata ends mid-reserved block");
    }
    q += kMetadataReservedLen;

    if (take<std::uint32_t>(q, end) != 0) {
      throw DbnError("this decoder cannot parse embedded schema definitions");
    }

    meta.symbols = take_symbol_list(q, end);
    meta.partial = take_symbol_list(q, end);
    meta.not_found = take_symbol_list(q, end);
    meta.mappings = take_mappings(q, end);

    rpos += frame_size;
    shift();
  }

  std::vector<std::string> take_symbol_list(const std::byte*& p, const std::byte* end) {
    const auto count_n = take<std::uint32_t>(p, end);
    const std::size_t width = meta.symbol_cstr_len;
    if (count_n > static_cast<std::size_t>(end - p) / width) {
      throw DbnError("DBN metadata declares more symbols than the frame holds");
    }
    std::vector<std::string> out;
    out.reserve(count_n);
    for (std::uint32_t i = 0; i < count_n; ++i) {
      out.push_back(take_cstr(p, end, width, "symbol"));
    }
    return out;
  }

  std::vector<SymbolMapping> take_mappings(const std::byte*& p, const std::byte* end) {
    const auto count_n = take<std::uint32_t>(p, end);
    const std::size_t width = meta.symbol_cstr_len;
    const std::size_t interval_size = width + 2 * sizeof(std::uint32_t);
    if (count_n > static_cast<std::size_t>(end - p) / (width + sizeof(std::uint32_t))) {
      throw DbnError("DBN metadata declares more symbol mappings than the frame holds");
    }
    std::vector<SymbolMapping> out;
    out.reserve(count_n);
    for (std::uint32_t i = 0; i < count_n; ++i) {
      SymbolMapping mapping;
      mapping.raw_symbol = take_cstr(p, end, width, "mapping symbol");
      const auto intervals = take<std::uint32_t>(p, end);
      if (intervals > static_cast<std::size_t>(end - p) / interval_size) {
        throw DbnError("a symbol mapping declares more intervals than the frame holds");
      }
      mapping.intervals.reserve(intervals);
      for (std::uint32_t j = 0; j < intervals; ++j) {
        MappingInterval interval;
        interval.start_date = take<std::uint32_t>(p, end);
        interval.end_date = take<std::uint32_t>(p, end);
        interval.symbol = take_cstr(p, end, width, "mapping interval symbol");
        mapping.intervals.push_back(std::move(interval));
      }
      out.push_back(std::move(mapping));
    }
    return out;
  }

  void validate(const RecordHeader& hd) const {
    const std::size_t len = record_bytes(hd);
    if (len < sizeof(RecordHeader)) {
      throw DbnError("record declares " + std::to_string(len) +
                     " bytes, shorter than the 16-byte record header");
    }
    // Every published DBN record length is a multiple of 8 bytes.
    if (len % 8 != 0) {
      throw DbnError("record length " + std::to_string(len) +
                     " is not a multiple of 8, which would misalign the rest of the stream");
    }
    const std::size_t expected = record_size_for_rtype(hd.rtype);
    if (expected == 0) {
      throw DbnError("unknown rtype " + hex_byte(hd.rtype) +
                     "; this decoder reads mbo, trades, mbp-10, status and definition");
    }
    const std::size_t needed = expected + (meta.ts_out ? sizeof(std::uint64_t) : 0);
    if (len < needed) {
      throw DbnError("rtype " + hex_byte(hd.rtype) + " record declares " + std::to_string(len) +
                     " bytes but needs " + std::to_string(needed));
    }
  }
};

DbnReader::DbnReader(const std::filesystem::path& path) : impl_{std::make_unique<Impl>()} {
  impl_->file.open(path, std::ios::binary);
  if (!impl_->file) {
    throw DbnError("cannot open " + path.string());
  }
  impl_->grow(kInitialBufferSize);
  impl_->open_source();
  impl_->decode_metadata();
}

DbnReader::DbnReader(const std::byte* data, std::size_t size) : impl_{std::make_unique<Impl>()} {
  if (data == nullptr && size != 0) {
    throw DbnError("in-memory DBN input is a null pointer with a nonzero size");
  }
  impl_->from_memory = true;
  impl_->mem = data;
  impl_->mem_size = size;
  impl_->grow(kInitialBufferSize);
  impl_->open_source();
  impl_->decode_metadata();
}

DbnReader::~DbnReader() = default;
DbnReader::DbnReader(DbnReader&&) noexcept = default;
DbnReader& DbnReader::operator=(DbnReader&&) noexcept = default;

const DbnMetadata& DbnReader::metadata() const noexcept {
  return impl_->meta;
}

std::uint64_t DbnReader::record_count() const noexcept {
  return impl_->count;
}

const RecordHeader* DbnReader::next() {
  Impl& s = *impl_;
  for (;;) {
    if (s.available() >= sizeof(RecordHeader)) {
      const auto* hd = reinterpret_cast<const RecordHeader*>(s.buf.get() + s.rpos);
      s.validate(*hd);
      const std::size_t len = record_bytes(*hd);
      if (s.available() >= len) {
        s.rpos += len;
        ++s.count;
        return hd;
      }
      s.reserve_for(len);
    } else {
      s.reserve_for(kMaxKnownRecordLen);
    }

    if (s.fill() == 0) {
      if (s.available() == 0) {
        return nullptr;
      }
      throw DbnError("the stream ends mid-record with " + std::to_string(s.available()) +
                     " bytes left over");
    }
  }
}

}  // namespace bookreplay
