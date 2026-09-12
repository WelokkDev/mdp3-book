// Hand-built MBO streams.
//
// Shapes here are copied from what a real GLBX.MDP3 file actually contains,
// not from what the format permits. The snapshot preamble in particular
// mirrors record #1 of glbx-mdp3-20260805.mbo.dbn.zst verbatim: action='R',
// order_id=0, size=0, price=UNDEF_PRICE, side='N', flags=F_SNAPSHOT|
// F_BAD_TS_RECV, with ts_event trailing ts_recv by days.

#ifndef BOOKREPLAY_TESTS_TOY_STREAM_HPP
#define BOOKREPLAY_TESTS_TOY_STREAM_HPP

#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <vector>

namespace bookreplay::testing {

inline constexpr std::int64_t kOne = kPriceScale;

/// MNQ/NQ tick: 0.25.
inline constexpr std::int64_t kTick = kPriceScale / 4;

[[nodiscard]] constexpr std::int64_t px(std::int64_t whole, std::int64_t ticks = 0) {
  return whole * kOne + ticks * kTick;
}

class StreamBuilder {
 public:
  explicit StreamBuilder(std::uint32_t instrument_id = 42004177) : instrument_id_(instrument_id) {}

  StreamBuilder& clear() {
    MboMsg& r = push('R', Side::kNone, 0, kUndefPrice, 0);
    r.flags = kFlagSnapshot | kFlagBadTsRecv;
    r.hd.ts_event = base_ts_ - 216'000'000'000'000;
    return *this;
  }

  StreamBuilder& add(std::uint64_t order_id, Side side, std::int64_t price, std::uint32_t size) {
    push('A', side, order_id, price, size);
    return *this;
  }

  StreamBuilder& cancel(std::uint64_t order_id, Side side, std::int64_t price, std::uint32_t size) {
    push('C', side, order_id, price, size);
    return *this;
  }

  StreamBuilder& modify(std::uint64_t order_id, Side side, std::int64_t price, std::uint32_t size) {
    push('M', side, order_id, price, size);
    return *this;
  }

  /// Aggressor print. `side` is the AGGRESSOR side in Databento MBO: Bid for
  /// a buy lifting the offer. It carries no resting order_id.
  StreamBuilder& trade(Side side, std::int64_t price, std::uint32_t size) {
    push('T', side, 0, price, size);
    return *this;
  }

  /// Per-resting-order fill attribution. READ-ONLY: the matching size
  /// reduction arrives separately as C or M.
  StreamBuilder& fill(std::uint64_t order_id, Side side, std::int64_t price, std::uint32_t size) {
    push('F', side, order_id, price, size);
    return *this;
  }

  /// The post-renormalization standalone event-boundary carrier.
  StreamBuilder& none() {
    MboMsg& r = push('N', Side::kNone, 0, kUndefPrice, 0);
    r.flags = kFlagLast;
    return *this;
  }

  StreamBuilder& last() {
    records_.back().flags |= kFlagLast;
    return *this;
  }

  StreamBuilder& snapshot() {
    records_.back().flags |= kFlagSnapshot | kFlagBadTsRecv;
    return *this;
  }

  StreamBuilder& set_flags(std::uint8_t flags) {
    records_.back().flags |= flags;
    return *this;
  }

  /// Deliberately corrupt the most recent record, for the well-formedness check.
  StreamBuilder& corrupt_action(char c) {
    records_.back().action = c;
    return *this;
  }

  /// Switch the stream to another instrument, keeping one sequence across
  /// both, as a real file has.
  StreamBuilder& instrument(std::uint32_t instrument_id) {
    instrument_id_ = instrument_id;
    return *this;
  }

  [[nodiscard]] const std::vector<MboMsg>& records() const noexcept { return records_; }

  [[nodiscard]] std::uint32_t instrument_id() const noexcept { return instrument_id_; }

 private:
  MboMsg& push(char action, Side side, std::uint64_t order_id, std::int64_t price,
               std::uint32_t size) {
    MboMsg r{};
    r.hd.length = kMboLengthUnits;
    r.hd.rtype = kRTypeMbo;
    r.hd.publisher_id = 1;
    r.hd.instrument_id = instrument_id_;
    r.hd.ts_event = base_ts_ + seq_ * 1'000'000;
    r.order_id = order_id;
    r.price = price;
    r.size = size;
    r.flags = 0;
    r.channel_id = 0;
    r.action = action;
    r.side = static_cast<char>(side);
    r.ts_recv = r.hd.ts_event + 500;
    r.ts_in_delta = 250;
    r.sequence = seq_;
    ++seq_;
    records_.push_back(r);
    return records_.back();
  }

  std::uint32_t instrument_id_;
  std::uint64_t base_ts_ = 1'785'888'000'000'000'000ULL;  // 2026-08-05T00:00:00Z
  std::uint32_t seq_ = 1;
  std::vector<MboMsg> records_;
};

/// A two-sided book with a one-tick spread, delivered as a snapshot.
///
///   bids  29000.00 (o100, 10)   28999.75 (o101, 7)
///   asks  29000.25 (o200, 10)   29000.50 (o201, 7)
inline StreamBuilder opening_snapshot() {
  StreamBuilder b;
  b.clear();
  b.add(100, Side::kBid, px(29000, 0), 10).snapshot();
  b.add(101, Side::kBid, px(28999, 3), 7).snapshot();
  b.add(200, Side::kAsk, px(29000, 1), 10).snapshot();
  b.add(201, Side::kAsk, px(29000, 2), 7).snapshot().last();
  return b;
}

inline std::vector<MboMsg> iceberg_then_market_moves_up() {
  StreamBuilder b = opening_snapshot();

  b.trade(Side::kBid, px(29000, 1), 5);
  b.fill(999'999, Side::kAsk, px(29000, 1), 5);  // order_id never Added
  b.none();                                      // standalone F_LAST carrier

  b.cancel(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(201, Side::kAsk, px(29000, 2), 7).last();

  b.add(300, Side::kBid, px(29005, 0), 12);
  b.add(400, Side::kAsk, px(29005, 1), 12).last();

  return b.records();
}

/// A clean event: aggressor print, per-order attribution, and the size
/// reduction arriving as its own Cancel. That is where every reduction
/// actually comes from.
inline std::vector<MboMsg> clean_trade_event() {
  StreamBuilder b = opening_snapshot();
  b.trade(Side::kBid, px(29000, 1), 10);
  b.fill(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(200, Side::kAsk, px(29000, 1), 10).last();
  return b.records();
}

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_TOY_STREAM_HPP
