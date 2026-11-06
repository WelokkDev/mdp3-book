#include "bookreplay/replay.hpp"

#include "bookreplay/order_ledger.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace bookreplay {

namespace {

enum class Mode : std::uint8_t {
  kInactive,
  kAggressive,
  kPassive,
};

/// What this engine keeps about an order beyond the ledger's entry for it.
struct Matching {
  Mode mode = Mode::kInactive;
  std::int64_t resting_from_ns = kNever;
  std::int64_t aggressive_from_ns = kNever;
};

}  // namespace

struct Replay::Impl {
  using Index = OrderLedger::Index;

  Impl(std::unique_ptr<TradeSource> src, ReplayConfig cfg)
      : source(std::move(src)),
        config(cfg),
        ledger(cfg.latency, cfg.scale, stats, cfg.reserve_live_orders,
               cfg.reserve_fills_per_advance) {
    matching.reserve(config.reserve_live_orders);
    if (config.retain_ticks) {
      ticks.reserve(config.reserve_fills_per_advance);
    }
  }

  std::unique_ptr<TradeSource> source;
  ReplayConfig config;
  ReplayStats stats{};

  OrderLedger ledger;
  /// Indexed as the ledger indexes its orders.
  std::vector<Matching> matching;
  std::vector<Tick> ticks;

  Tick stash{};
  std::int64_t last_print_price = 0;
  bool have_last_print = false;
  bool stash_valid = false;
  bool exhausted = false;
  bool poisoned = false;
  bool instrument_bound = false;
  std::uint32_t instrument = 0;
  std::int64_t now = kNotStarted;

  /// A decode failure ends the run, so no further work is accepted. Reading
  /// back what did happen stays legal.
  void refuse_when_poisoned() const {
    if (poisoned) {
      throw ReplayError("replay was poisoned by a decode failure and cannot advance");
    }
  }

  void submit(const Order& o) {
    refuse_when_poisoned();
    ledger.submit(o, now);
    matching.emplace_back();
  }

  void cancel_at(OrderId id, std::int64_t request_ts) {
    refuse_when_poisoned();
    ledger.cancel_at(id, request_ts, now);
  }

  void go_live(Index index) {
    OrderLedger::Entry& e = ledger[index];
    Matching& m = matching[index];
    e.status = OrderStatus::kLive;
    switch (e.order.type) {
      case OrderType::kMarket:
        m.mode = Mode::kAggressive;
        m.aggressive_from_ns = e.effective_live_ns;
        break;
      case OrderType::kLimit:
        m.mode = Mode::kPassive;
        m.resting_from_ns = e.effective_live_ns;
        break;
      case OrderType::kStop:
      case OrderType::kStopLimit:
        m.mode = Mode::kInactive;
        break;
    }
  }

  [[nodiscard]] std::uint32_t fill_qty(const OrderLedger::Entry& e, const Tick& t) const noexcept {
    return config.fill_size == FillSizePolicy::kPrintCapped ? std::min(e.remaining, t.size)
                                                            : e.remaining;
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

  // Databento's Side on a trade is the aggressor's, so the other side aggressing
  // means the print is the touch we are not crossing.
  [[nodiscard]] std::int64_t aggressive_price(Side ours, const Tick& t, std::uint32_t q) {
    if (t.aggressor != opposite(ours)) {
      return t.price;
    }
    const std::int64_t level = config.scale.to_ticks(t.price);
    if (!config.scale.representable(level)) {
      throw ReplayError("print price is outside the representable tick range");
    }
    const std::int64_t charged = config.scale.to_price(ours == Side::kBid ? level + 1 : level - 1);
    stats.tick_charged_qty += q;
    return charged;
  }

  void match_one(Index index, const Tick& t) {
    OrderLedger::Entry& e = ledger[index];
    Matching& m = matching[index];

    // CME stops are trade-elected: a quote at the trigger does not fire one,
    // and election is exchange-side, so no client latency follows it.
    if (uses_trigger(e.order.type) && !e.elected && e.effective_live_ns <= t.ts) {
      if (elects(e.order.side, e.trigger_price, t.price)) {
        ledger.elect(index, t.ts, t.ts_event);
        if (e.order.type == OrderType::kStop) {
          m.mode = Mode::kAggressive;
          m.aggressive_from_ns = t.ts;
        } else {
          m.mode = Mode::kPassive;
          m.resting_from_ns = t.ts;
        }
      }
    }

    Fill f;
    f.ts_ns = t.ts;
    f.ts_event = t.ts_event;
    f.print_size = t.size;
    f.sequence = t.sequence;
    f.aggressor = t.aggressor;

    if (m.mode == Mode::kAggressive && m.aggressive_from_ns <= t.ts) {
      f.qty = fill_qty(e, t);
      f.price = aggressive_price(e.order.side, t, f.qty);
      f.reason = e.elected ? FillReason::kStopElected : FillReason::kMarket;
    } else if (m.mode == Mode::kPassive && m.resting_from_ns < t.ts) {
      const bool through =
          e.order.side == Side::kBid ? t.price < e.limit_price : t.price > e.limit_price;
      if (through && aggressor_can_fill(e.order.side, t.aggressor)) {
        f.price = e.limit_price;
        f.reason = e.elected ? FillReason::kStopLimitThrough : FillReason::kLimitThrough;
        f.qty = fill_qty(e, t);
      }
    }

    if (f.qty > 0) {
      ledger.fill(index, f);
    }
  }

  void apply_schedule(std::int64_t at) {
    for (const Index index : ledger.working()) {
      OrderLedger::Entry& e = ledger[index];
      if (e.status != OrderStatus::kPending || e.effective_live_ns > at) {
        continue;
      }
      // Withdrawn before it could reach the venue, so the venue never sees it;
      // the cancel loop below retires it.
      if (e.effective_cancel_ns != kNever && e.effective_cancel_ns <= e.effective_live_ns) {
        continue;
      }
      // CME refuses at order entry a buy stop whose trigger is not above the
      // last trade price, and a sell stop whose trigger is not below it.
      if (uses_trigger(e.order.type) && have_last_print &&
          elects(e.order.side, e.trigger_price, last_print_price)) {
        ledger.reject_stop(index);
      } else {
        go_live(index);
      }
    }
    for (const Index index : ledger.working()) {
      OrderLedger::Entry& e = ledger[index];
      if (!is_terminal(e.status) && e.effective_cancel_ns != kNever &&
          e.effective_cancel_ns <= at) {
        ledger.apply_cancel(index);
      }
    }
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
    for (const Index index : ledger.working()) {
      if (is_terminal(ledger[index].status) || ledger[index].remaining == 0) {
        continue;
      }
      match_one(index, t);
    }
    ledger.compact();
    last_print_price = t.price;
    have_last_print = true;
  }

  std::span<const Fill> advance_to(std::int64_t target) {
    refuse_when_poisoned();
    if (target < now) {
      throw ReplayError("advance_to is monotonic; an earlier instant is not a rewind");
    }
    ledger.clear_fills();
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
      try {
        process_tick(t);
      } catch (...) {
        poisoned = true;
        throw;
      }
      if (config.retain_ticks) {
        if (ticks.size() == ticks.capacity()) {
          ++stats.reallocations;
        }
        ticks.push_back(t);
      }
    }

    apply_schedule(target);
    ledger.compact();
    now = target;
    return ledger.fills();
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
  return impl_->ledger.view(id);
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
