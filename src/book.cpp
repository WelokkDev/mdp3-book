#include "bookreplay/book.hpp"

#include <algorithm>
#include <unordered_set>

namespace bookreplay {
namespace {

/// An order id of 0 is the venue's "no order" sentinel, and a side of 'N'
/// names no book to rest in. Neither can be placed.
[[nodiscard]] bool placeable(Side side, std::uint64_t order_id) noexcept {
  return order_id != 0 && (side == Side::kBid || side == Side::kAsk);
}

/// Templated only because the bid and ask maps order their keys differently.
template <typename LevelMap>
void drop_from(LevelMap& levels, std::int64_t price, std::uint64_t order_id, std::uint32_t size) {
  const auto it = levels.find(price);
  if (it == levels.end()) {
    throw BookError("resting order names a level that does not exist");
  }
  Book::Level& lvl = it->second;
  const auto q = std::find(lvl.queue.begin(), lvl.queue.end(), order_id);
  if (q == lvl.queue.end()) {
    throw BookError("resting order is missing from its level's queue");
  }
  if (lvl.size < size) {
    throw BookError("level total is smaller than the order leaving it");
  }
  lvl.queue.erase(q);
  lvl.size -= size;
  if (lvl.queue.empty()) {
    levels.erase(it);
  }
}

template <typename LevelMap>
void verify_side(const std::unordered_map<std::uint64_t, Book::Resting>& orders, Side side,
                 const LevelMap& levels, std::unordered_set<std::uint64_t>& queued) {
  for (const auto& [price, lvl] : levels) {
    if (lvl.queue.empty()) {
      throw BookError("a level with no orders is still in the map");
    }
    std::uint64_t total = 0;
    for (const std::uint64_t id : lvl.queue) {
      const auto it = orders.find(id);
      if (it == orders.end() || it->second.side != side || it->second.price != price) {
        throw BookError("a queued order id does not rest at that level");
      }
      if (!queued.insert(id).second) {
        throw BookError("an order id is queued twice");
      }
      total += it->second.size;
    }
    if (total != lvl.size) {
      throw BookError("a level's total disagrees with the sum of its queue");
    }
  }
}

}  // namespace

const Book::Level* Book::find_level(const Instrument& b, Side side, std::int64_t price) {
  if (side == Side::kBid) {
    const auto it = b.bids.find(price);
    return it == b.bids.end() ? nullptr : &it->second;
  }
  if (side == Side::kAsk) {
    const auto it = b.asks.find(price);
    return it == b.asks.end() ? nullptr : &it->second;
  }
  return nullptr;
}

Book::Level* Book::level_in(Instrument& b, Side side, std::int64_t price) {
  // The instrument is ours and non-const; only the lookup is shared.
  return const_cast<Level*>(find_level(b, side, price));
}

void Book::insert(Instrument& b, std::uint64_t order_id, Side side, std::int64_t price,
                  std::uint32_t size) {
  b.orders[order_id] = Resting{side, price, size, 0};
  Level& lvl = (side == Side::kBid) ? b.bids[price] : b.asks[price];
  lvl.queue.push_back(order_id);
  lvl.size += size;
}

void Book::erase(Instrument& b, std::uint64_t order_id) {
  const auto it = b.orders.find(order_id);
  if (it == b.orders.end()) {
    return;
  }
  const Resting r = it->second;
  b.orders.erase(it);
  if (r.side == Side::kBid) {
    drop_from(b.bids, r.price, order_id, r.size);
  } else if (r.side == Side::kAsk) {
    drop_from(b.asks, r.price, order_id, r.size);
  } else {
    throw BookError("resting order has no side");
  }
}

void Book::add(const MboMsg& rec) {
  const Side side = side_of(rec);
  if (!placeable(side, rec.order_id)) {
    return;
  }
  Instrument& b = books_[rec.hd.instrument_id];
  // An id still resting when its A arrives means the record that removed the
  // old order never reached us. The newest record wins, and the stale entry
  // must not be left pointing at a level it no longer sits in.
  if (b.orders.find(rec.order_id) != b.orders.end()) {
    ++duplicate_adds_;
    erase(b, rec.order_id);
  }
  insert(b, rec.order_id, side, rec.price, rec.size);
}

void Book::cancel(const MboMsg& rec) {
  const auto it = books_.find(rec.hd.instrument_id);
  if (it == books_.end()) {
    return;
  }
  // A cancel naming an order this book never saw rest is data, not failure:
  // the same reason an F for an unknown id is. Nothing to erase.
  erase(it->second, rec.order_id);
}

// An order keeps its place in the queue only when a modify leaves its price
// alone and does not increase its size. A price change, or a size increase,
// is a new order at the tail.
//
// A fill complicates the size test. The venue reports a partial fill as an F
// and then an M carrying the remainder, and that M keeps priority. But an
// iceberg whose displayed tranche was consumed is refreshed the same way (an
// M at the same price, usually at the same size as before and sometimes
// smaller when the fill ate into hidden quantity), and the new tranche queues
// at the tail. The fill tells the two apart: after an F, the M keeps priority
// only if its size is exactly what the fill left behind. On 2026-08-26, an NQ
// day from a post-renormalization pull, all 2,032 same-price, same-size M
// records followed an F for that order, and the next fill at the level went
// to an order queued behind it six times out of seven.
//
// tests/toy_book.hpp does erase-then-insert unconditionally. That is correct
// for an aggregate and destroys queue position every time, and no aggregate
// view can tell the two apart: a level's total is identical either way.
// Only fill timing can, which is why the mbp-10 diff cannot falsify this
// rule on its own.
void Book::modify(const MboMsg& rec) {
  const Side side = side_of(rec);
  if (!placeable(side, rec.order_id)) {
    return;
  }
  Instrument& b = books_[rec.hd.instrument_id];
  const auto it = b.orders.find(rec.order_id);
  if (it == b.orders.end()) {
    // Never seen resting, so this modify is the first we hear of the order and
    // it joins at the tail. ToyBook reaches the same state by a no-op erase
    // followed by an insert, which keeps the two comparable.
    ++unknown_modifies_;
    insert(b, rec.order_id, side, rec.price, rec.size);
    return;
  }

  const Resting before = it->second;
  const std::uint64_t remainder = before.size > before.filled ? before.size - before.filled : 0;
  const bool size_keeps =
      before.filled == 0 ? rec.size <= before.size : std::uint64_t{rec.size} == remainder;
  const bool keeps_priority = before.side == side && before.price == rec.price && size_keeps;
  if (!keeps_priority) {
    erase(b, rec.order_id);
    insert(b, rec.order_id, side, rec.price, rec.size);
    return;
  }

  // In place: the queue is untouched and only the aggregate moves.
  Level* lvl = level_in(b, side, before.price);
  const std::uint32_t reduction = before.size - rec.size;
  if (lvl == nullptr || lvl->size < reduction) {
    throw BookError("resting order's level cannot absorb its size reduction");
  }
  lvl->size -= reduction;
  it->second.size = rec.size;
  it->second.filled = 0;
}

// Read-only as the harness understands it: the count, membership and both
// touches are untouched. What it records is the attribution the next M needs.
void Book::fill(const MboMsg& rec) {
  const auto b = books_.find(rec.hd.instrument_id);
  if (b == books_.end()) {
    return;
  }
  const auto it = b->second.orders.find(rec.order_id);
  if (it == b->second.orders.end()) {
    return;
  }
  if (it->second.filled == 0) {
    filled_this_event_.emplace_back(rec.hd.instrument_id, rec.order_id);
  }
  it->second.filled += rec.size;
}

void Book::forget_fills() {
  for (const auto& [instrument_id, order_id] : filled_this_event_) {
    const auto b = books_.find(instrument_id);
    if (b == books_.end()) {
      continue;
    }
    const auto it = b->second.orders.find(order_id);
    if (it != b->second.orders.end()) {
      it->second.filled = 0;
    }
  }
  filled_this_event_.clear();
}

void Book::apply(const MboMsg& rec) {
  switch (action_of(rec)) {
    case Action::kAdd:
      ++mutations_;
      add(rec);
      break;
    case Action::kCancel:
      ++mutations_;
      cancel(rec);
      break;
    case Action::kModify:
      ++mutations_;
      modify(rec);
      break;
    case Action::kClear:
      ++mutations_;
      books_.erase(rec.hd.instrument_id);
      break;
    case Action::kFill:
      fill(rec);
      break;
    // A trade names the aggressor, not the order it hit: its id is the
    // aggressing order's, which may itself come to rest later in the same
    // event. The resting side's reduction arrives as its own C or M. N carries
    // only flags. Neither touches the book.
    case Action::kTrade:
    case Action::kNone:
      break;
  }
  // An action outside the enum is malformed. The harness reports it; the book
  // leaves its state alone and counts nothing, which is what `mutates_book`
  // expects of an unknown action.

  // Fill attribution is scoped to the event it arrived in, so an F with no
  // C or M behind it cannot recolour a later, unrelated M.
  if (is_event_boundary(rec)) {
    forget_fills();
  }
}

void Book::reset() {
  books_.clear();
  filled_this_event_.clear();
}

void Book::verify() const {
  for (const auto& entry : books_) {
    const Instrument& b = entry.second;
    std::unordered_set<std::uint64_t> queued;
    verify_side(b.orders, Side::kBid, b.bids, queued);
    verify_side(b.orders, Side::kAsk, b.asks, queued);
    if (queued.size() != b.orders.size()) {
      throw BookError("a resting order is queued at no level");
    }
  }
}

const Book::Resting* Book::order(std::uint32_t instrument_id, std::uint64_t order_id) const {
  const Instrument* b = find(instrument_id);
  if (b == nullptr) {
    return nullptr;
  }
  const auto it = b->orders.find(order_id);
  return it == b->orders.end() ? nullptr : &it->second;
}

const Book::Level* Book::level(std::uint32_t instrument_id, Side side, std::int64_t price) const {
  const Instrument* b = find(instrument_id);
  return b == nullptr ? nullptr : find_level(*b, side, price);
}

std::uint64_t Book::queue_ahead(std::uint32_t instrument_id, std::uint64_t order_id) const {
  const Instrument* b = find(instrument_id);
  if (b == nullptr) {
    return kNoQueuePosition;
  }
  const auto oit = b->orders.find(order_id);
  if (oit == b->orders.end()) {
    return kNoQueuePosition;
  }
  const Level* lvl = find_level(*b, oit->second.side, oit->second.price);
  if (lvl == nullptr) {
    throw BookError("resting order names a level that does not exist");
  }
  std::uint64_t ahead = 0;
  for (const std::uint64_t id : lvl->queue) {
    if (id == order_id) {
      return ahead;
    }
    const auto it = b->orders.find(id);
    if (it == b->orders.end()) {
      throw BookError("a queued order id does not rest at that level");
    }
    ahead += it->second.size;
  }
  throw BookError("resting order is missing from its level's queue");
}

std::vector<std::uint32_t> Book::instruments() const {
  std::vector<std::uint32_t> out;
  out.reserve(books_.size());
  for (const auto& entry : books_) {
    out.push_back(entry.first);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t Book::order_count() const noexcept {
  std::size_t n = 0;
  for (const auto& entry : books_) {
    n += entry.second.orders.size();
  }
  return n;
}

}  // namespace bookreplay
