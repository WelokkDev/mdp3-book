#ifndef BOOKREPLAY_TRADE_SOURCE_HPP
#define BOOKREPLAY_TRADE_SOURCE_HPP

#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/order.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace bookreplay {

inline constexpr std::uint32_t kAnyInstrument = 0;

/// One trade print, from a TradeMsg or from an MboMsg carrying action='T'.
struct Tick {
  std::int64_t ts = 0;  ///< ts_recv
  std::int64_t ts_event = 0;
  std::int64_t price = 0;  ///< 1e-9 fixed point
  std::uint32_t size = 0;
  std::uint32_t instrument_id = 0;
  std::uint32_t sequence = 0;
  Side aggressor = Side::kNone;  ///< kNone is an auction or implied print
};

static_assert(sizeof(Tick) == 40);
static_assert(alignof(Tick) == 8);

struct TradeSourceStats {
  std::uint64_t records = 0;
  std::uint64_t ticks = 0;
  std::uint64_t skipped_non_trade = 0;
  std::uint64_t skipped_other_instrument = 0;
  std::uint64_t undef_price = 0;
  std::uint64_t zero_size = 0;
  std::uint64_t bad_ts_recv = 0;
  std::uint64_t no_aggressor = 0;

  [[nodiscard]] bool reconciles() const noexcept {
    return records == ticks + skipped_non_trade + skipped_other_instrument + undef_price +
                          zero_size + bad_ts_recv;
  }
};

struct TradeSourceOptions {
  /// A parent-symbol pull carries every outright and every calendar spread.
  std::uint32_t instrument_id = kAnyInstrument;
};

class TradeSource {
 public:
  TradeSource() = default;
  virtual ~TradeSource();
  TradeSource(const TradeSource&) = delete;
  TradeSource& operator=(const TradeSource&) = delete;
  TradeSource(TradeSource&&) = delete;
  TradeSource& operator=(TradeSource&&) = delete;

  /// nullptr at end of stream; the returned pointer dies at the next call.
  [[nodiscard]] virtual const Tick* next() = 0;

  [[nodiscard]] virtual const TradeSourceStats& stats() const noexcept = 0;
};

/// On an mbo stream only action='T' becomes a tick; action='F' is
/// per-resting-order attribution of the same match.
class DbnTradeSource final : public TradeSource {
 public:
  explicit DbnTradeSource(const std::filesystem::path& path, TradeSourceOptions opts = {});
  DbnTradeSource(const std::byte* data, std::size_t size, TradeSourceOptions opts = {});
  ~DbnTradeSource() override;

  [[nodiscard]] const Tick* next() override;
  [[nodiscard]] const TradeSourceStats& stats() const noexcept override;
  [[nodiscard]] const DbnMetadata& metadata() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class RecordTradeSource final : public TradeSource {
 public:
  explicit RecordTradeSource(std::vector<TradeMsg> records, TradeSourceOptions opts = {});
  explicit RecordTradeSource(std::vector<MboMsg> records, TradeSourceOptions opts = {});
  ~RecordTradeSource() override;

  [[nodiscard]] const Tick* next() override;
  [[nodiscard]] const TradeSourceStats& stats() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class TickSpanSource final : public TradeSource {
 public:
  explicit TickSpanSource(std::span<const Tick> ticks);
  ~TickSpanSource() override;

  [[nodiscard]] const Tick* next() override;
  [[nodiscard]] const TradeSourceStats& stats() const noexcept override;
  void rewind() noexcept;

 private:
  std::span<const Tick> ticks_;
  std::size_t pos_ = 0;
  std::int64_t last_ts_ = kNotStarted;
  TradeSourceStats stats_{};
};

[[nodiscard]] std::vector<Tick> collect(TradeSource& source);

}  // namespace bookreplay

#endif  // BOOKREPLAY_TRADE_SOURCE_HPP
