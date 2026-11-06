#include "bookreplay/queue_positions.hpp"

#include "bookreplay/order.hpp"

#include <algorithm>

namespace bookreplay {

QueuePositions::Slot QueuePositions::place(Side side, std::int64_t price, std::uint64_t stamp,
                                           std::uint64_t tag, std::uint64_t blind_to) {
  Slot slot = free_;
  if (slot != kNoSlot) {
    free_ = positions_[slot].next_free;
  } else {
    slot = static_cast<Slot>(positions_.size());
    positions_.emplace_back();
  }
  positions_[slot] = Position{side, price, stamp, blind_to, tag, 0, kNoSlot};

  // Behind any position already holding the same stamp: it was placed first.
  Level& level = ladder(side)[key(side, price)];
  const auto behind =
      std::upper_bound(level.begin(), level.end(), stamp,
                       [this](std::uint64_t s, Slot other) { return s < positions_[other].stamp; });
  level.insert(behind, slot);
  return slot;
}

void QueuePositions::remove(Slot slot) {
  Position& position = positions_[slot];
  Ladder& side = ladder(position.side);
  const auto held = side.find(key(position.side, position.price));
  Level& level = held->second;
  level.erase(std::find(level.begin(), level.end(), slot));
  if (level.empty()) {
    side.erase(held);
  }
  position.next_free = free_;
  free_ = slot;
}

void QueuePositions::clear() {
  positions_.clear();
  free_ = kNoSlot;
  bids_.clear();
  asks_.clear();
  prints_.clear();
  reaches_.clear();
}

std::int64_t QueuePositions::best(Side side) const noexcept {
  const Ladder& held = ladder(side);
  return held.empty() ? kUndefPrice : key(side, held.begin()->first);
}

QueuePositions::Level* QueuePositions::level_at(Side side, std::int64_t price) {
  Ladder& held = ladder(side);
  const auto it = held.find(key(side, price));
  return it == held.end() ? nullptr : &it->second;
}

void QueuePositions::on_trade(const MboMsg& trade) {
  finish_print();
  prints_.push_back(Print{trade, Side::kNone, false, 0});
}

void QueuePositions::note_side(Print& print, Side side) {
  if (print.hit == Side::kNone) {
    print.hit = side;
  } else if (print.hit != side && !print.reaches_nothing) {
    print.reaches_nothing = true;
    ++counters_->prints_on_both_sides;
  }
}

bool QueuePositions::note_fill(const MboMsg& fill) {
  if (prints_.empty()) {
    ++counters_->fills_before_a_print;
    return false;
  }
  Print& print = prints_.back();
  if (fill.order_id != 0 && fill.order_id == print.trade.order_id) {
    ++counters_->aggressor_fills;
    return false;
  }
  print.filled += fill.size;
  const Side side = side_of(fill);
  if (side != Side::kBid && side != Side::kAsk) {
    return true;
  }
  note_side(print, side);
  return !print.reaches_nothing && level_at(side, print.trade.price) != nullptr;
}

void QueuePositions::tally(const MboMsg& fill, const RestingView* resting) {
  Print& print = prints_.back();
  Side side = side_of(fill);
  if (side != Side::kBid && side != Side::kAsk) {
    if (resting == nullptr) {
      ++counters_->fills_without_a_side;
      print.reaches_nothing = true;
      return;
    }
    side = resting->side;
    note_side(print, side);
    if (print.reaches_nothing) {
      return;
    }
  }
  Level* level = level_at(side, print.trade.price);
  if (level == nullptr) {
    return;
  }
  if (resting == nullptr) {
    ++counters_->fills_on_unknown_orders;
    for (const Slot slot : *level) {
      positions_[slot].ahead += fill.size;
    }
    return;
  }
  if (resting->side != side || resting->price != print.trade.price) {
    ++counters_->fills_off_resting_price;
    return;
  }
  // `filled` is what this event's earlier F's took from the order, so what is
  // left of `size` is the tranche still showing. Past it the fill is a hidden
  // tranche's, and the venue refreshes a tranche to the back of its level:
  // those lots went through every visible order first.
  const std::uint64_t shown = resting->size > resting->filled ? resting->size - resting->filled : 0;
  const std::uint64_t visible = std::min<std::uint64_t>(fill.size, shown);
  counters_->hidden_lots += fill.size - visible;
  for (auto it = level->rbegin(); it != level->rend(); ++it) {
    Position& position = positions_[*it];
    if (position.stamp < resting->stamp) {
      break;
    }
    if (position.blind_to != fill.order_id) {
      position.ahead += visible;
    }
  }
}

bool QueuePositions::unchanged(const MboMsg& modify, const RestingView& resting) noexcept {
  return resting.filled == 0 && resting.side == side_of(modify) && resting.price == modify.price &&
         resting.size == modify.size;
}

void QueuePositions::forget_tallies(Side side, std::int64_t price) {
  if (const Level* level = level_at(side, price)) {
    for (const Slot slot : *level) {
      positions_[slot].ahead = 0;
    }
  }
}

void QueuePositions::finish_print() {
  if (prints_.empty()) {
    return;
  }
  Print& print = prints_.back();
  const std::int64_t price = print.trade.price;
  const auto index = static_cast<std::uint32_t>(prints_.size() - 1);
  if (print.reaches_nothing || print.hit == Side::kNone) {
    // With no fill to say which side was hit, only the side the aggressor
    // faced could have been, or either side if the print names no aggressor.
    // Such a print met implied liquidity alone; it is left out rather than
    // guessed at, and counted where a position stood to gain: at its price,
    // or at a better one, which it had to pass to trade there.
    const auto stood_to_gain = [&](Side side) {
      const Ladder& held = ladder(side);
      return !held.empty() && held.begin()->first <= key(side, price);
    };
    const Side aggressor = side_of(print.trade);
    const bool named = aggressor == Side::kBid || aggressor == Side::kAsk;
    if (!print.reaches_nothing &&
        (named ? stood_to_gain(opposite(aggressor))
               : stood_to_gain(Side::kBid) || stood_to_gain(Side::kAsk))) {
      ++counters_->prints_without_fills;
    }
    forget_tallies(Side::kBid, price);
    forget_tallies(Side::kAsk, price);
    return;
  }

  bool reached = false;
  const Ladder& held = ladder(print.hit);
  const std::int64_t at = key(print.hit, price);
  for (auto it = held.begin(); it != held.end() && it->first < at; ++it) {
    for (const Slot slot : it->second) {
      reaches_.push_back(
          Reach{slot, positions_[slot].tag, index, ReachKind::kThrough, print.trade.size});
      reached = true;
    }
  }
  if (const Level* level = level_at(print.hit, price)) {
    const std::uint64_t traded = print.trade.size;
    if (print.filled < traded) {
      counters_->unattributed_lots += traded - print.filled;
    }
    for (const Slot slot : *level) {
      Position& position = positions_[slot];
      const std::uint64_t quantity = traded - std::min(traded, position.ahead);
      position.ahead = 0;
      reaches_.push_back(Reach{slot, position.tag, index, ReachKind::kAtPrice, quantity});
      reached = reached || quantity != 0;
    }
  }
  if (reached && side_of(print.trade) == Side::kNone) {
    ++counters_->no_aggressor_reaches;
  }
}

}  // namespace bookreplay
