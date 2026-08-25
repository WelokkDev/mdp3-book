#include "bookreplay/replay.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace bookreplay {

namespace {

enum class Mode : std::uint8_t {
  kInactive,
  kAggressive,
  kPassive,
};

struct OrderState {
  Order order{};
  OrderStatus status = OrderStatus::kPending;
  Mode mode = Mode::kInactive;
  std::uint32_t remaining = 0;
  std::uint32_t filled = 0;
  std::uint32_t oco_reduced = 0;
  std::int64_t effective_live_ns = 0;
  std::int64_t effective_cancel_ns = kNever;
  std::int64_t elected_ns = kNever;
  std::int64_t resting_from_ns = kNever;
  std::int64_t aggressive_from_ns = kNever;
  std::int64_t trigger_price = 0;
  std::int64_t limit_price = 0;
  bool elected = false;
};

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

template <typename T>
void push_counted(std::vector<T>& v, const T& item, std::uint64_t& reallocations) {
  if (v.size() == v.capacity()) {
    ++reallocations;
  }
  v.push_back(item);
}

void go_live(OrderState& s) {
  s.status = OrderStatus::kLive;
  switch (s.order.type) {
    case OrderType::kMarket:
      s.mode = Mode::kAggressive;
      s.aggressive_from_ns = s.effective_live_ns;
      break;
    case OrderType::kLimit:
      s.mode = Mode::kPassive;
      s.resting_from_ns = s.effective_live_ns;
      break;
    case OrderType::kStop:
    case OrderType::kStopLimit:
      s.mode = Mode::kInactive;
      break;
  }
}

}  // namespace

struct Replay::Impl {
  Impl(std::unique_ptr<TradeSource> src, ReplayConfig cfg) : source(std::move(src)), config(cfg) {
    orders.reserve(config.reserve_live_orders);
    by_id.reserve(config.reserve_live_orders);
    active.reserve(config.reserve_live_orders);
    fills.reserve(config.reserve_fills_per_advance);
    if (config.retain_ticks) {
      ticks.reserve(config.reserve_fills_per_advance);
    }
  }

  std::unique_ptr<TradeSource> source;
  ReplayConfig config;
  ReplayStats stats{};

  std::vector<OrderState> orders;
  std::vector<std::pair<OrderId, std::uint32_t>> by_id;
  std::vector<std::uint32_t> active;
  std::vector<Fill> fills;
  std::vector<Tick> ticks;

  Tick stash{};
  bool stash_valid = false;
  bool exhausted = false;
  bool poisoned = false;
  bool instrument_bound = false;
  std::uint32_t instrument = 0;
  std::int64_t now = kNotStarted;
  std::uint64_t fill_seq = 0;

  [[nodiscard]] std::size_t find_index(OrderId id) const {
    const auto it = std::lower_bound(
        by_id.begin(), by_id.end(), id,
        [](const std::pair<OrderId, std::uint32_t>& e, OrderId v) { return e.first < v; });
    if (it == by_id.end() || it->first != id) {
      return kNoIndex;
    }
    return it->second;
  }

  void insert_by_id(OrderId id, std::uint32_t idx) {
    const auto it = std::lower_bound(
        by_id.begin(), by_id.end(), id,
        [](const std::pair<OrderId, std::uint32_t>& e, OrderId v) { return e.first < v; });
    by_id.insert(it, {id, idx});
  }

  void submit(const Order& o) {
    validate(o, config.scale);
    if (find_index(o.id) != kNoIndex) {
      throw ReplayError("an order with that id was already submitted");
    }
    const std::int64_t lat = config.latency.for_class(o.latency);
    if (o.live_from_ns > kNever - lat) {
      throw ReplayError("order live instant overflows once its latency is added");
    }

    OrderState s;
    s.order = o;
    s.remaining = o.qty;
    s.effective_live_ns = o.live_from_ns + lat;
    if (uses_trigger(o.type)) {
      s.trigger_price = config.scale.to_price(o.trigger_ticks);
    }
    if (uses_limit(o.type)) {
      s.limit_price = config.scale.to_price(o.limit_ticks);
    }
    if (now != kNotStarted && s.effective_live_ns < now) {
      ++stats.late_arm_orders;
      stats.late_arm_ns_total += now - s.effective_live_ns;
    }

    const auto idx = static_cast<std::uint32_t>(orders.size());
    orders.push_back(s);
    insert_by_id(o.id, idx);
    active.push_back(idx);
  }

  void cancel_at(OrderId id, std::int64_t request_ts) {
    const std::size_t i = find_index(id);
    if (i == kNoIndex) {
      throw ReplayError("cancel names an order id that was never submitted");
    }
    if (request_ts < 0) {
      throw ReplayError("cancel request instant must be non-negative");
    }
    if (now != kNotStarted && request_ts < now) {
      throw ReplayError("cancel request precedes the cursor");
    }
    const std::int64_t lat = config.latency.cancel_ns();
    if (request_ts >= kNever - lat) {
      throw ReplayError("cancel instant overflows once its latency is added");
    }
    OrderState& s = orders[i];
    if (is_terminal(s.status)) {
      return;
    }
    s.effective_cancel_ns = std::min(s.effective_cancel_ns, request_ts + lat);
  }

  [[nodiscard]] std::uint32_t fill_qty(const OrderState& s, const Tick& t) const noexcept {
    return config.fill_size == FillSizePolicy::kPrintCapped ? std::min(s.remaining, t.size)
                                                            : s.remaining;
  }

  [[nodiscard]] bool aggressor_can_fill(Side ours, Side aggressor) noexcept {
    if (aggressor == opposite(ours)) {
      return true;
    }
    if (aggressor == ours) {
      return false;
    }
    if (config.no_aggressor == NoAggressorPolicy::kBothSides) {
      return true;
    }
    ++stats.no_aggressor_passive_skips;
    return false;
  }

  void emit_fill(std::uint32_t idx, const Tick& t, std::int64_t price, std::uint32_t q,
                 FillReason reason) {
    OrderState& s = orders[idx];
    s.remaining -= q;
    s.filled += q;
    ++stats.fills;
    if (s.remaining == 0) {
      s.status = OrderStatus::kFilled;
    } else {
      ++stats.partial_fills;
    }

    Fill f;
    f.seq = ++fill_seq;
    f.order_id = s.order.id;
    f.oco_group = s.order.oco_group;
    f.ts_ns = t.ts;
    f.ts_event = t.ts_event;
    f.price = price;
    f.price_ticks = config.scale.to_ticks(price);
    f.qty = q;
    f.remaining = s.remaining;
    f.print_size = t.size;
    f.sequence = t.sequence;
    f.side = s.order.side;
    f.aggressor = t.aggressor;
    f.reason = reason;
    push_counted(fills, f, stats.reallocations);
  }

  void apply_oco(std::uint32_t member, std::uint32_t q) {
    const OcoGroup group = orders[member].order.oco_group;
    if (group == kNoOcoGroup) {
      return;
    }
    for (const std::uint32_t idx : active) {
      if (idx == member) {
        continue;
      }
      OrderState& s = orders[idx];
      if (s.order.oco_group != group || is_terminal(s.status)) {
        continue;
      }
      const std::uint32_t d = std::min(s.remaining, q);
      if (d > 0) {
        s.remaining -= d;
        s.oco_reduced += d;
        ++stats.oco_reductions;
      }
      if (s.remaining == 0) {
        s.status = OrderStatus::kOcoCancelled;
        ++stats.oco_cancels;
      }
    }
    if (orders[member].remaining != 0) {
      return;
    }
    for (const std::uint32_t idx : active) {
      if (idx == member) {
        continue;
      }
      OrderState& s = orders[idx];
      if (s.order.oco_group != group || is_terminal(s.status)) {
        continue;
      }
      s.status = OrderStatus::kOcoCancelled;
      ++stats.oco_cancels;
    }
  }

  void match_one(std::uint32_t idx, const Tick& t) {
    OrderState& s = orders[idx];

    // CME stops are trade-elected: a quote at the trigger does not fire one,
    // and election is exchange-side, so no client latency follows it.
    if (uses_trigger(s.order.type) && !s.elected && s.effective_live_ns <= t.ts) {
      const bool hit =
          s.order.side == Side::kBid ? t.price >= s.trigger_price : t.price <= s.trigger_price;
      if (hit) {
        s.elected = true;
        s.elected_ns = t.ts;
        s.status = OrderStatus::kElected;
        ++stats.elections;
        if (s.order.type == OrderType::kStop) {
          s.mode = Mode::kAggressive;
          s.aggressive_from_ns = t.ts;
        } else {
          s.mode = Mode::kPassive;
          s.resting_from_ns = t.ts;
        }
      }
    }

    std::int64_t price = 0;
    FillReason reason = FillReason::kMarket;
    std::uint32_t q = 0;

    if (s.mode == Mode::kAggressive && s.aggressive_from_ns <= t.ts) {
      price = t.price;
      reason = s.elected ? FillReason::kStopElected : FillReason::kMarket;
      q = fill_qty(s, t);
    } else if (s.mode == Mode::kPassive && s.resting_from_ns < t.ts) {
      const bool through =
          s.order.side == Side::kBid ? t.price < s.limit_price : t.price > s.limit_price;
      if (through && aggressor_can_fill(s.order.side, t.aggressor)) {
        price = s.limit_price;
        reason = s.elected ? FillReason::kStopLimitThrough : FillReason::kLimitThrough;
        q = fill_qty(s, t);
      }
    }

    if (q > 0) {
      emit_fill(idx, t, price, q, reason);
      apply_oco(idx, q);
    }
  }

  void apply_schedule(std::int64_t at) {
    for (const std::uint32_t idx : active) {
      OrderState& s = orders[idx];
      if (s.status == OrderStatus::kPending && s.effective_live_ns <= at) {
        go_live(s);
      }
    }
    for (const std::uint32_t idx : active) {
      OrderState& s = orders[idx];
      if (!is_terminal(s.status) && s.effective_cancel_ns != kNever &&
          s.effective_cancel_ns <= at) {
        s.status = OrderStatus::kCancelled;
        ++stats.cancels_applied;
      }
    }
  }

  void compact() {
    active.erase(std::remove_if(active.begin(), active.end(),
                                [this](std::uint32_t i) { return is_terminal(orders[i].status); }),
                 active.end());
  }

  void process_tick(const Tick& t) {
    if (!instrument_bound) {
      instrument_bound = true;
      instrument = t.instrument_id;
    } else if (t.instrument_id != instrument) {
      poisoned = true;
      throw ReplayError(
          "a print from a second instrument reached the replay; filter the source to one "
          "instrument_id");
    }
    ++stats.ticks;
    if (t.aggressor == Side::kNone) {
      ++stats.no_aggressor_ticks;
    }
    apply_schedule(t.ts);
    for (const std::uint32_t idx : active) {
      if (is_terminal(orders[idx].status) || orders[idx].remaining == 0) {
        continue;
      }
      match_one(idx, t);
    }
    compact();
  }

  std::span<const Fill> advance_to(std::int64_t target) {
    if (poisoned) {
      throw ReplayError("replay was poisoned by a decode failure and cannot advance");
    }
    if (target < now) {
      throw ReplayError("advance_to is monotonic; an earlier instant is not a rewind");
    }
    fills.clear();
    ticks.clear();

    while (true) {
      if (!stash_valid) {
        const Tick* t = nullptr;
        try {
          t = source->next();
        } catch (...) {
          poisoned = true;
          throw;
        }
        if (t == nullptr) {
          exhausted = true;
          break;
        }
        stash = *t;
        stash_valid = true;
      }
      if (stash.ts > target) {
        break;
      }
      const Tick t = stash;
      stash_valid = false;
      process_tick(t);
      if (config.retain_ticks) {
        push_counted(ticks, t, stats.reallocations);
      }
    }

    apply_schedule(target);
    compact();
    now = target;
    return {fills.data(), fills.size()};
  }

  [[nodiscard]] OrderView view(OrderId id) const {
    const std::size_t i = find_index(id);
    if (i == kNoIndex) {
      throw ReplayError("order names an id that was never submitted");
    }
    const OrderState& s = orders[i];
    OrderView v;
    v.status = s.status;
    v.type = s.order.type;
    v.side = s.order.side;
    v.oco_group = s.order.oco_group;
    v.qty = s.order.qty;
    v.remaining = s.remaining;
    v.filled = s.filled;
    v.oco_reduced = s.oco_reduced;
    v.effective_live_ns = s.effective_live_ns;
    v.effective_cancel_ns = s.effective_cancel_ns;
    v.elected_ns = s.elected_ns;
    v.trigger_price = s.trigger_price;
    v.limit_price = s.limit_price;
    return v;
  }
};

Replay::Replay(std::unique_ptr<TradeSource> source, ReplayConfig config)
    : impl_(std::make_unique<Impl>(std::move(source), config)) {}

Replay::~Replay() = default;
Replay::Replay(Replay&&) noexcept = default;
Replay& Replay::operator=(Replay&&) noexcept = default;

void Replay::submit(const Order& order) {
  impl_->submit(order);
}

void Replay::cancel(OrderId id) {
  impl_->cancel_at(id, impl_->now == kNotStarted ? 0 : impl_->now);
}

void Replay::cancel_at(OrderId id, std::int64_t request_ts_ns) {
  impl_->cancel_at(id, request_ts_ns);
}

std::span<const Fill> Replay::advance_to(std::int64_t ts_ns) {
  return impl_->advance_to(ts_ns);
}

std::span<const Tick> Replay::last_ticks() const noexcept {
  return {impl_->ticks.data(), impl_->ticks.size()};
}

std::int64_t Replay::now_ns() const noexcept {
  return impl_->now;
}

bool Replay::exhausted() const noexcept {
  return impl_->exhausted;
}

OrderView Replay::order(OrderId id) const {
  return impl_->view(id);
}

const ReplayConfig& Replay::config() const noexcept {
  return impl_->config;
}

const ReplayStats& Replay::stats() const noexcept {
  return impl_->stats;
}

const TradeSourceStats& Replay::source_stats() const noexcept {
  return impl_->source->stats();
}

}  // namespace bookreplay
