#include "bookreplay/order_ledger.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace bookreplay {

namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

bool id_below(const std::pair<OrderId, OrderLedger::Index>& entry, OrderId id) noexcept {
  return entry.first < id;
}

}  // namespace

OrderLedger::OrderLedger(Latency latency, TickScale scale, OrderLedgerStats& stats,
                         std::size_t reserve_orders, std::size_t reserve_fills)
    : latency_(latency), scale_(scale), stats_(&stats) {
  orders_.reserve(reserve_orders);
  by_id_.reserve(reserve_orders);
  working_.reserve(reserve_orders);
  fills_.reserve(reserve_fills);
}

std::size_t OrderLedger::find(OrderId id) const {
  const auto it = std::lower_bound(by_id_.begin(), by_id_.end(), id, id_below);
  if (it == by_id_.end() || it->first != id) {
    return kNoIndex;
  }
  return it->second;
}

OrderLedger::Index OrderLedger::submit(const Order& order, std::int64_t now) {
  validate(order, scale_);
  const auto slot = std::lower_bound(by_id_.begin(), by_id_.end(), order.id, id_below);
  if (slot != by_id_.end() && slot->first == order.id) {
    throw ReplayError("an order with that id was already submitted");
  }
  const std::int64_t lat = latency_.for_class(order.latency);
  if (order.live_from_ns > kNever - lat) {
    throw ReplayError("order live instant overflows once its latency is added");
  }

  Entry e;
  e.order = order;
  e.remaining = order.qty;
  e.effective_live_ns = order.live_from_ns + lat;
  if (uses_trigger(order.type)) {
    e.trigger_price = scale_.to_price(order.trigger_ticks);
  }
  if (uses_limit(order.type)) {
    e.limit_price = scale_.to_price(order.limit_ticks);
  }
  if (now != kNotStarted && e.effective_live_ns < now) {
    ++stats_->late_arm_orders;
    stats_->late_arm_ns_total += static_cast<std::uint64_t>(now - e.effective_live_ns);
  }

  const auto index = static_cast<Index>(orders_.size());
  orders_.push_back(e);
  by_id_.insert(slot, {order.id, index});
  working_.push_back(index);
  return index;
}

void OrderLedger::cancel_at(OrderId id, std::int64_t request_ts_ns, std::int64_t now) {
  const std::size_t i = find(id);
  if (i == kNoIndex) {
    throw ReplayError("cancel names an order id that was never submitted");
  }
  if (request_ts_ns < 0) {
    throw ReplayError("cancel request instant must be non-negative");
  }
  if (now != kNotStarted && request_ts_ns < now) {
    throw ReplayError("cancel request precedes the cursor");
  }
  const std::int64_t lat = latency_.cancel_ns();
  // Not even kNever itself: that is how an order says it was never cancelled.
  if (request_ts_ns >= kNever - lat) {
    throw ReplayError("cancel instant overflows once its latency is added");
  }
  Entry& e = orders_[i];
  if (is_terminal(e.status)) {
    return;
  }
  e.effective_cancel_ns = std::min(e.effective_cancel_ns, request_ts_ns + lat);
}

OrderView OrderLedger::view(OrderId id) const {
  const std::size_t i = find(id);
  if (i == kNoIndex) {
    throw ReplayError("order names an id that was never submitted");
  }
  const Entry& e = orders_[i];
  OrderView v;
  v.status = e.status;
  v.type = e.order.type;
  v.side = e.order.side;
  v.oco_group = e.order.oco_group;
  v.qty = e.order.qty;
  v.remaining = e.remaining;
  v.filled = e.filled;
  v.oco_reduced = e.oco_reduced;
  v.effective_live_ns = e.effective_live_ns;
  v.effective_cancel_ns = e.effective_cancel_ns;
  v.elected_ns = e.elected_ns;
  v.elected_ts_event = e.elected_ts_event;
  v.trigger_price = e.trigger_price;
  v.limit_price = e.limit_price;
  return v;
}

void OrderLedger::compact() {
  working_.erase(std::remove_if(working_.begin(), working_.end(),
                                [this](Index i) { return is_terminal(orders_[i].status); }),
                 working_.end());
}

void OrderLedger::fill(Index index, Fill fill) {
  Entry& e = orders_[index];
  e.remaining -= fill.qty;
  e.filled += fill.qty;
  ++stats_->fills;
  if (e.remaining == 0) {
    e.status = OrderStatus::kFilled;
  } else {
    ++stats_->partial_fills;
  }

  fill.seq = ++fill_seq_;
  fill.order_id = e.order.id;
  fill.oco_group = e.order.oco_group;
  fill.price_ticks = scale_.to_ticks(fill.price);
  fill.remaining = e.remaining;
  fill.side = e.order.side;
  if (fills_.size() == fills_.capacity()) {
    ++stats_->reallocations;
  }
  fills_.push_back(fill);

  apply_oco(index, fill.qty);
}

void OrderLedger::elect(Index index, std::int64_t ts_ns, std::int64_t ts_event) {
  Entry& e = orders_[index];
  e.elected = true;
  e.elected_ns = ts_ns;
  e.elected_ts_event = ts_event;
  e.status = OrderStatus::kElected;
  ++stats_->elections;
}

void OrderLedger::reject_stop(Index index) {
  orders_[index].status = OrderStatus::kRejected;
  ++stats_->stop_entry_rejects;
}

void OrderLedger::apply_cancel(Index index) {
  orders_[index].status = OrderStatus::kCancelled;
  ++stats_->cancels_applied;
}

void OrderLedger::apply_oco(Index member, std::uint32_t qty) {
  const OcoGroup group = orders_[member].order.oco_group;
  if (group == kNoOcoGroup) {
    return;
  }
  for (const Index i : working_) {
    if (i == member) {
      continue;
    }
    Entry& e = orders_[i];
    if (e.order.oco_group != group || is_terminal(e.status)) {
      continue;
    }
    const std::uint32_t d = std::min(e.remaining, qty);
    if (d > 0) {
      e.remaining -= d;
      e.oco_reduced += d;
      ++stats_->oco_reductions;
    }
    if (e.remaining == 0) {
      e.status = OrderStatus::kOcoCancelled;
      ++stats_->oco_cancels;
    }
  }
  if (orders_[member].remaining != 0) {
    return;
  }
  for (const Index i : working_) {
    if (i == member) {
      continue;
    }
    Entry& e = orders_[i];
    if (e.order.oco_group != group || is_terminal(e.status)) {
      continue;
    }
    e.status = OrderStatus::kOcoCancelled;
    ++stats_->oco_cancels;
  }
}

}  // namespace bookreplay
