#ifndef BOOKREPLAY_TESTS_TOY_REPLAY_HPP
#define BOOKREPLAY_TESTS_TOY_REPLAY_HPP

#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/trade_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bookreplay::testing {

class ToyReplayBase {
 public:
  ToyReplayBase(std::vector<Tick> ticks, Latency latency, TickScale scale)
      : ticks_(std::move(ticks)), latency_(latency), scale_(scale) {}

  virtual ~ToyReplayBase() = default;
  ToyReplayBase(const ToyReplayBase&) = delete;
  ToyReplayBase& operator=(const ToyReplayBase&) = delete;
  ToyReplayBase(ToyReplayBase&&) = delete;
  ToyReplayBase& operator=(ToyReplayBase&&) = delete;

  void submit(const Order& o) {
    validate(o, scale_);
    State s;
    s.order = o;
    s.remaining = o.qty;
    s.live_ns = effective_live(o);
    if (uses_trigger(o.type)) {
      s.trigger_price = scale_.to_price(o.trigger_ticks);
    }
    if (uses_limit(o.type)) {
      s.limit_price = scale_.to_price(o.limit_ticks);
    }
    if (o.type == OrderType::kMarket) {
      s.mode = Mode::kAggressive;
      s.from_ns = s.live_ns;
    } else if (o.type == OrderType::kLimit) {
      s.mode = Mode::kPassive;
      s.from_ns = s.live_ns;
    }
    states_.push_back(s);
  }

  void cancel_at(OrderId id, std::int64_t request_ts_ns) {
    for (State& s : states_) {
      if (s.order.id == id && !is_terminal(s.status)) {
        s.cancel_ns = std::min(s.cancel_ns, effective_cancel(request_ts_ns));
      }
    }
  }

  std::span<const Fill> advance_to(std::int64_t target) {
    fills_.clear();
    while (pos_ < ticks_.size() && ticks_[pos_].ts <= target) {
      process(ticks_[pos_]);
      ++pos_;
    }
    retire_cancelled(target);
    finish_advance();
    return {fills_.data(), fills_.size()};
  }

  [[nodiscard]] OrderView order(OrderId id) const {
    for (const State& s : states_) {
      if (s.order.id != id) {
        continue;
      }
      OrderView v;
      v.status = s.status;
      v.type = s.order.type;
      v.side = s.order.side;
      v.oco_group = s.order.oco_group;
      v.qty = s.order.qty;
      v.remaining = s.remaining;
      v.filled = s.filled;
      v.oco_reduced = s.oco_reduced;
      v.effective_live_ns = s.live_ns;
      v.effective_cancel_ns = s.cancel_ns;
      v.trigger_price = s.trigger_price;
      v.limit_price = s.limit_price;
      return v;
    }
    throw ReplayError("order names an id that was never submitted");
  }

 protected:
  enum class Mode : std::uint8_t { kInactive, kAggressive, kPassive };

  struct State {
    Order order{};
    OrderStatus status = OrderStatus::kPending;
    Mode mode = Mode::kInactive;
    std::uint32_t remaining = 0;
    std::uint32_t filled = 0;
    std::uint32_t oco_reduced = 0;
    std::int64_t live_ns = 0;
    std::int64_t cancel_ns = kNever;
    std::int64_t from_ns = kNever;
    std::int64_t trigger_price = 0;
    std::int64_t limit_price = 0;
    bool elected = false;
  };

  [[nodiscard]] virtual std::int64_t effective_live(const Order& o) const {
    return o.live_from_ns + latency_.for_class(o.latency);
  }

  [[nodiscard]] virtual std::int64_t effective_cancel(std::int64_t request_ts_ns) const {
    return request_ts_ns + latency_.cancel_ns();
  }

  [[nodiscard]] virtual bool aggressor_ok(Side ours, Side aggressor) const {
    return aggressor == opposite(ours);
  }

  [[nodiscard]] virtual bool limit_through(Side ours, std::int64_t print,
                                           std::int64_t limit) const {
    return ours == Side::kBid ? print < limit : print > limit;
  }

  [[nodiscard]] virtual std::uint32_t fill_quantity(std::uint32_t remaining,
                                                    std::uint32_t print_size) const {
    return std::min(remaining, print_size);
  }

  virtual void apply_oco(std::size_t member, std::uint32_t q) {
    const OcoGroup group = states_[member].order.oco_group;
    if (group == kNoOcoGroup) {
      return;
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (i == member) {
        continue;
      }
      State& s = states_[i];
      if (s.order.oco_group != group || is_terminal(s.status)) {
        continue;
      }
      const std::uint32_t d = std::min(s.remaining, q);
      s.remaining -= d;
      s.oco_reduced += d;
      if (s.remaining == 0) {
        s.status = OrderStatus::kOcoCancelled;
      }
    }
    if (states_[member].remaining != 0) {
      return;
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (i == member) {
        continue;
      }
      State& s = states_[i];
      if (s.order.oco_group != group || is_terminal(s.status)) {
        continue;
      }
      s.status = OrderStatus::kOcoCancelled;
    }
  }

  virtual void finish_advance() {}

  std::vector<State> states_;
  std::vector<Fill> fills_;

 private:
  void retire_cancelled(std::int64_t at) {
    for (State& s : states_) {
      if (!is_terminal(s.status) && s.cancel_ns != kNever && s.cancel_ns <= at) {
        s.status = OrderStatus::kCancelled;
      }
    }
  }

  void process(const Tick& t) {
    retire_cancelled(t.ts);
    for (std::size_t i = 0; i < states_.size(); ++i) {
      State& s = states_[i];
      if (is_terminal(s.status) || s.remaining == 0) {
        continue;
      }
      if (uses_trigger(s.order.type) && !s.elected && s.live_ns <= t.ts) {
        const bool hit =
            s.order.side == Side::kBid ? t.price >= s.trigger_price : t.price <= s.trigger_price;
        if (hit) {
          s.elected = true;
          s.mode = s.order.type == OrderType::kStop ? Mode::kAggressive : Mode::kPassive;
          s.from_ns = t.ts;
        }
      }

      std::uint32_t q = 0;
      std::int64_t price = 0;
      FillReason reason = FillReason::kMarket;
      if (s.mode == Mode::kAggressive && s.from_ns <= t.ts) {
        price = t.price;
        reason = s.elected ? FillReason::kStopElected : FillReason::kMarket;
        q = fill_quantity(s.remaining, t.size);
      } else if (s.mode == Mode::kPassive && s.from_ns < t.ts &&
                 aggressor_ok(s.order.side, t.aggressor) &&
                 limit_through(s.order.side, t.price, s.limit_price)) {
        price = s.limit_price;
        reason = s.elected ? FillReason::kStopLimitThrough : FillReason::kLimitThrough;
        q = fill_quantity(s.remaining, t.size);
      }

      if (q > 0) {
        emit(i, t, price, q, reason);
        apply_oco(i, q);
      }
    }
  }

  void emit(std::size_t i, const Tick& t, std::int64_t price, std::uint32_t q, FillReason reason) {
    State& s = states_[i];
    s.remaining = q >= s.remaining ? 0 : s.remaining - q;
    s.filled += q;
    if (s.remaining == 0) {
      s.status = OrderStatus::kFilled;
    }

    Fill f;
    f.seq = ++seq_;
    f.order_id = s.order.id;
    f.oco_group = s.order.oco_group;
    f.ts_ns = t.ts;
    f.ts_event = t.ts_event;
    f.price = price;
    f.price_ticks = scale_.to_ticks(price);
    f.qty = q;
    f.remaining = s.remaining;
    f.print_size = t.size;
    f.sequence = t.sequence;
    f.side = s.order.side;
    f.aggressor = t.aggressor;
    f.reason = reason;
    fills_.push_back(f);
  }

  std::vector<Tick> ticks_;
  Latency latency_;
  TickScale scale_;
  std::size_t pos_ = 0;
  std::uint64_t seq_ = 0;
};

class CancelOnFirstFillReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  void apply_oco(std::size_t member, std::uint32_t) override {
    const OcoGroup group = states_[member].order.oco_group;
    if (group == kNoOcoGroup) {
      return;
    }
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (i == member) {
        continue;
      }
      State& s = states_[i];
      if (s.order.oco_group != group || is_terminal(s.status)) {
        continue;
      }
      s.status = OrderStatus::kOcoCancelled;
    }
  }
};

class NoAggressorBothSidesReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  [[nodiscard]] bool aggressor_ok(Side ours, Side aggressor) const override {
    return aggressor != ours;
  }
};

class IgnoresEntryLatencyReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  [[nodiscard]] std::int64_t effective_live(const Order& o) const override {
    return o.live_from_ns;
  }
};

class IgnoresCancelReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  [[nodiscard]] std::int64_t effective_cancel(std::int64_t) const override { return kNever; }
};

class OverfillReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  [[nodiscard]] std::uint32_t fill_quantity(std::uint32_t remaining,
                                            std::uint32_t print_size) const override {
    return std::min(remaining, print_size) + 1;
  }
};

class UnorderedFillReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  void finish_advance() override { std::reverse(fills_.begin(), fills_.end()); }
};

class LimitAtTheTouchReplay : public ToyReplayBase {
 public:
  using ToyReplayBase::ToyReplayBase;

 protected:
  [[nodiscard]] bool limit_through(Side ours, std::int64_t print,
                                   std::int64_t limit) const override {
    return ours == Side::kBid ? print <= limit : print >= limit;
  }
};

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_TOY_REPLAY_HPP
