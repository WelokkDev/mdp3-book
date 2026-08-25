#ifndef BOOKREPLAY_TESTS_TOY_TRADES_HPP
#define BOOKREPLAY_TESTS_TOY_TRADES_HPP

#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <vector>

#include "toy_stream.hpp"

namespace bookreplay::testing {

inline constexpr std::int64_t kTsInDelta = 500;

class TradeStreamBuilder {
 public:
  explicit TradeStreamBuilder(std::uint32_t instrument_id = 42004177)
      : instrument_id_(instrument_id) {}

  TradeStreamBuilder& trade(Side aggressor, std::int64_t price, std::uint32_t size) {
    push(aggressor, price, size);
    return *this;
  }

  TradeStreamBuilder& auction(std::int64_t price, std::uint32_t size) {
    push(Side::kNone, price, size);
    return *this;
  }

  TradeStreamBuilder& at(std::int64_t ts_recv_ns) {
    pending_ts_ = ts_recv_ns;
    return *this;
  }

  TradeStreamBuilder& same_ts() {
    pending_ts_ = last_ts_;
    return *this;
  }

  TradeStreamBuilder& instrument(std::uint32_t instrument_id) {
    instrument_id_ = instrument_id;
    return *this;
  }

  TradeStreamBuilder& bad_ts_recv() {
    records_.back().flags |= kFlagBadTsRecv;
    return *this;
  }

  TradeStreamBuilder& undef_price() {
    records_.back().price = kUndefPrice;
    return *this;
  }

  TradeStreamBuilder& corrupt_side(char c) {
    records_.back().side = c;
    return *this;
  }

  TradeStreamBuilder& corrupt_action(char c) {
    records_.back().action = c;
    return *this;
  }

  [[nodiscard]] const std::vector<TradeMsg>& records() const noexcept { return records_; }

  [[nodiscard]] std::uint32_t instrument_id() const noexcept { return instrument_id_; }

 private:
  void push(Side side, std::int64_t price, std::uint32_t size) {
    const std::int64_t ts = pending_ts_ != kUnset ? pending_ts_ : last_ts_ + step_ns_;
    pending_ts_ = kUnset;
    last_ts_ = ts;

    TradeMsg r{};
    r.hd.length = kLengthUnits<TradeMsg>;
    r.hd.rtype = kRTypeTrade;
    r.hd.publisher_id = 1;
    r.hd.instrument_id = instrument_id_;
    r.hd.ts_event = static_cast<std::uint64_t>(ts - kTsInDelta);
    r.price = price;
    r.size = size;
    r.action = static_cast<char>(Action::kTrade);
    r.side = static_cast<char>(side);
    r.flags = 0;
    r.depth = 0;
    r.ts_recv = static_cast<std::uint64_t>(ts);
    r.ts_in_delta = static_cast<std::int32_t>(kTsInDelta);
    r.sequence = seq_++;
    records_.push_back(r);
  }

  static constexpr std::int64_t kUnset = -1;

  std::uint32_t instrument_id_;
  std::int64_t base_ts_ = 1'785'888'000'000'000'000LL;  // 2026-08-05T00:00:00Z
  std::int64_t step_ns_ = 1'000'000;
  std::int64_t last_ts_ = base_ts_;
  std::int64_t pending_ts_ = kUnset;
  std::uint32_t seq_ = 1;
  std::vector<TradeMsg> records_;
};

[[nodiscard]] inline std::vector<MboMsg> as_mbo_trades(const TradeStreamBuilder& builder) {
  std::vector<MboMsg> out;
  out.reserve(builder.records().size());
  for (const TradeMsg& t : builder.records()) {
    MboMsg m{};
    m.hd.length = kMboLengthUnits;
    m.hd.rtype = kRTypeMbo;
    m.hd.publisher_id = t.hd.publisher_id;
    m.hd.instrument_id = t.hd.instrument_id;
    m.hd.ts_event = t.hd.ts_event;
    m.order_id = 0;  // an aggressor print names no resting order
    m.price = t.price;
    m.size = t.size;
    m.flags = t.flags;
    m.channel_id = 0;
    m.action = t.action;
    m.side = t.side;
    m.ts_recv = t.ts_recv;
    m.ts_in_delta = t.ts_in_delta;
    m.sequence = t.sequence;
    out.push_back(m);
  }
  return out;
}

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_TOY_TRADES_HPP
