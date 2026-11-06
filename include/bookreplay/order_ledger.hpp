// What an order is to a fill engine before any market data reaches it: an id
// that cannot repeat, the instants latency moves it to, the quantity still
// working, and the siblings an OCO group ties it to. Every engine keeps its
// orders here so that a script means the same thing to each, and whatever one
// fills that another does not is the fill model's doing.

#ifndef BOOKREPLAY_ORDER_LEDGER_HPP
#define BOOKREPLAY_ORDER_LEDGER_HPP

#include "bookreplay/dbn.hpp"
#include "bookreplay/order.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bookreplay {

struct OrderLedgerStats {
  std::uint64_t elections = 0;
  /// Stops whose trigger was already through the last print when they armed.
  std::uint64_t stop_entry_rejects = 0;
  std::uint64_t fills = 0;
  std::uint64_t partial_fills = 0;
  std::uint64_t oco_reductions = 0;
  std::uint64_t oco_cancels = 0;
  std::uint64_t cancels_applied = 0;
  /// Orders whose effective live instant was already behind the cursor at
  /// submit; accepted, not clamped.
  std::uint64_t late_arm_orders = 0;
  std::uint64_t late_arm_ns_total = 0;
  /// Growth of a per-advance buffer past what was reserved for it.
  std::uint64_t reallocations = 0;
};

/// CME elects a buy stop on a trade at or above its trigger and a sell stop on
/// one at or below, and refuses at entry a stop the last trade would elect.
[[nodiscard]] constexpr bool elects(Side ours, std::int64_t trigger, std::int64_t print) noexcept {
  return ours == Side::kBid ? print >= trigger : print <= trigger;
}

class OrderLedger {
 public:
  using Index = std::uint32_t;

  struct Entry {
    Order order{};
    OrderStatus status = OrderStatus::kPending;
    std::uint32_t remaining = 0;
    std::uint32_t filled = 0;
    std::uint32_t oco_reduced = 0;
    std::int64_t effective_live_ns = 0;
    std::int64_t effective_cancel_ns = kNever;
    std::int64_t elected_ns = kNever;
    std::int64_t elected_ts_event = kNever;
    std::int64_t trigger_price = 0;
    std::int64_t limit_price = 0;
    bool elected = false;
  };

  /// Counts into `stats`, which must outlive the ledger.
  OrderLedger(Latency latency, TickScale scale, OrderLedgerStats& stats, std::size_t reserve_orders,
              std::size_t reserve_fills);

  /// Throws ReplayError on a duplicate id or a malformed order. `now` is the
  /// engine's cursor, or kNotStarted before its first advance.
  Index submit(const Order& order, std::int64_t now);

  /// Throws ReplayError on an id never submitted, or a request that is
  /// negative, behind `now`, or too close to kNever to add the latency to.
  /// Inert on an order already terminal.
  void cancel_at(OrderId id, std::int64_t request_ts_ns, std::int64_t now);

  /// Throws ReplayError on an id that was never submitted.
  [[nodiscard]] OrderView view(OrderId id) const;

  [[nodiscard]] Entry& operator[](Index index) noexcept { return orders_[index]; }

  [[nodiscard]] const Entry& operator[](Index index) const noexcept { return orders_[index]; }

  /// Every order submitted and not yet dropped by `compact()`, in submit
  /// order. Filling an order never changes this list.
  [[nodiscard]] std::span<const Index> working() const noexcept {
    return {working_.data(), working_.size()};
  }

  /// Drops every terminal order from `working()`.
  void compact();

  /// Takes `fill.qty` off the order, completes the record with what the ledger
  /// knows (its place in the run, the order's id, group and side, the tick
  /// level, what remains) and takes the same quantity off every OCO sibling,
  /// cancelling them all once the order is filled. The caller sets what the
  /// market decided.
  void fill(Index index, Fill fill);

  void elect(Index index, std::int64_t ts_ns, std::int64_t ts_event);

  /// A stop the last print would already elect, refused at entry.
  void reject_stop(Index index);

  /// Retires the order once the cancel `cancel_at` scheduled takes effect.
  void apply_cancel(Index index);

  void clear_fills() noexcept { fills_.clear(); }

  [[nodiscard]] std::span<const Fill> fills() const noexcept {
    return {fills_.data(), fills_.size()};
  }

 private:
  [[nodiscard]] std::size_t find(OrderId id) const;
  void apply_oco(Index member, std::uint32_t qty);

  Latency latency_;
  TickScale scale_;
  OrderLedgerStats* stats_;
  std::vector<Entry> orders_;
  std::vector<std::pair<OrderId, Index>> by_id_;
  std::vector<Index> working_;
  std::vector<Fill> fills_;
  std::uint64_t fill_seq_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_ORDER_LEDGER_HPP
