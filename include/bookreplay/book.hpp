// A limit order book rebuilt from market-by-order.
//
// This is the reference implementation: obvious containers, no attempt at
// speed. It is meant to survive the fast book rather than be replaced by it.
// Once both exist, this one is the oracle the fast one is differentially
// tested against, the same way databento-dbn is the oracle for the decoder.
// An oracle that quietly repairs itself is worthless, so an internal
// inconsistency here throws rather than clamps.

#ifndef BOOKREPLAY_BOOK_HPP
#define BOOKREPLAY_BOOK_HPP

#include "bookreplay/dbn.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bookreplay {

/// Thrown when the book finds its own state inconsistent. Nothing in the
/// stream can provoke it; it means a bug in the book itself.
class BookError : public BookreplayError {
 public:
  using BookreplayError::BookreplayError;
};

inline constexpr std::uint64_t kNoQueuePosition = std::numeric_limits<std::uint64_t>::max();

/// Every instrument the stream carries, each with a full order-by-order book.
///
/// Queues are FIFO, which is what NQ outrights use (`match_algorithm` 'F' in
/// the definition schema). A pro-rata or allocation product has no single
/// queue position, and this book does not check which it was handed.
///
/// `mutation_count()` counts applied *records*, not orders touched: A, C, M
/// and R count exactly 1 each (a Clear dropping a thousand orders counts
/// once, and a snapshot record counts the same as any other), while T, F and
/// N count 0. That is the arithmetic `InvariantHarness` reconciles against,
/// and it is why a book that bulk-loaded a snapshot would not reconcile here.
///
/// A record that names no placeable order is counted and otherwise ignored:
/// an A or M with side 'N', or with order id 0, moves the count and nothing
/// else.
///
/// Two things the harness cannot see are counted separately. An M for an id
/// that is not resting joins the book as if added, and an A for an id that is
/// still resting replaces it. In a stream that began with a snapshot both
/// mean a removal was missed, so a clean run reports both as zero.
class Book {
 public:
  struct Resting {
    Side side = Side::kNone;
    std::int64_t price = 0;
    std::uint32_t size = 0;
    /// Quantity F records have attributed to this order since its last M, or
    /// since the last event boundary. Read by the next M to tell a partial
    /// fill's remainder from an iceberg refresh. Never part of `size`.
    std::uint64_t filled = 0;
  };

  /// The orders resting at one (side, price), oldest first. `size` is their
  /// total, carried alongside so reading the aggregate is not a walk.
  struct Level {
    std::deque<std::uint64_t> queue;
    std::uint64_t size = 0;
  };

  /// Opposite comparators, so `begin()` is the touch on either side.
  using Bids = std::map<std::int64_t, Level, std::greater<>>;
  using Asks = std::map<std::int64_t, Level>;

  void apply(const MboMsg& rec);

  /// No counter is reset.
  void reset();

  /// Throws BookError unless every level's total is the sum of its queue,
  /// every queued id rests at exactly that (side, price) and nowhere else,
  /// every resting order is queued, and no level is empty.
  void verify() const;

  // The four BookLike requires; `invariants.hpp` will not compile without them.

  [[nodiscard]] std::uint64_t mutation_count() const noexcept { return mutations_; }

  [[nodiscard]] bool contains(std::uint32_t instrument_id, std::uint64_t order_id) const {
    const Instrument* b = find(instrument_id);
    return b != nullptr && b->orders.find(order_id) != b->orders.end();
  }

  [[nodiscard]] std::int64_t best_bid(std::uint32_t instrument_id) const {
    const Instrument* b = find(instrument_id);
    return (b == nullptr || b->bids.empty()) ? kUndefPrice : b->bids.begin()->first;
  }

  [[nodiscard]] std::int64_t best_ask(std::uint32_t instrument_id) const {
    const Instrument* b = find(instrument_id);
    return (b == nullptr || b->asks.empty()) ? kUndefPrice : b->asks.begin()->first;
  }

  // Beyond BookLike.

  /// M records whose order id was not resting; each joined at the tail.
  [[nodiscard]] std::uint64_t unknown_modifies() const noexcept { return unknown_modifies_; }

  /// A records whose order id was still resting; each replaced the old order.
  [[nodiscard]] std::uint64_t duplicate_adds() const noexcept { return duplicate_adds_; }

  /// The resting order, or nullptr if it is not resting here. The pointer is
  /// good until the next `apply()` or `reset()`, which may erase it.
  [[nodiscard]] const Resting* order(std::uint32_t instrument_id, std::uint64_t order_id) const;

  /// The queue at one (side, price), oldest first, or nullptr if no such
  /// level exists. A level exists exactly while its queue is non-empty, so
  /// the pointer is good only until the next `apply()` or `reset()`: the
  /// cancel that empties the level frees it.
  [[nodiscard]] const Level* level(std::uint32_t instrument_id, Side side,
                                   std::int64_t price) const;

  /// Resting quantity ahead of `order_id` in its own level: what a fill has
  /// to consume before reaching it. 0 at the front of the queue,
  /// `kNoQueuePosition` if the order is not resting.
  [[nodiscard]] std::uint64_t queue_ahead(std::uint32_t instrument_id,
                                          std::uint64_t order_id) const;

  [[nodiscard]] std::size_t instrument_count() const noexcept { return books_.size(); }

  /// Instruments holding a book, ascending so a report over them is stable
  /// across runs. A Clear drops one; cancelling its every order does not, so
  /// an instrument here may hold none, while one the stream merely named (an
  /// unplaceable A, a cancel, an F) never appears at all.
  [[nodiscard]] std::vector<std::uint32_t> instruments() const;

  /// Walks every instrument; unlike `instrument_count()` above, not O(1).
  [[nodiscard]] std::size_t order_count() const noexcept;

 private:
  struct Instrument {
    std::unordered_map<std::uint64_t, Resting> orders;
    Bids bids;
    Asks asks;
  };

  [[nodiscard]] const Instrument* find(std::uint32_t instrument_id) const {
    const auto it = books_.find(instrument_id);
    return it == books_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] static const Level* find_level(const Instrument& b, Side side, std::int64_t price);
  [[nodiscard]] static Level* level_in(Instrument& b, Side side, std::int64_t price);

  static void insert(Instrument& b, std::uint64_t order_id, Side side, std::int64_t price,
                     std::uint32_t size);
  static void erase(Instrument& b, std::uint64_t order_id);

  void add(const MboMsg& rec);
  void cancel(const MboMsg& rec);
  void modify(const MboMsg& rec);
  void fill(const MboMsg& rec);
  void forget_fills();

  std::unordered_map<std::uint32_t, Instrument> books_;
  /// Orders an F has touched since the last event boundary.
  std::vector<std::pair<std::uint32_t, std::uint64_t>> filled_this_event_;
  std::uint64_t mutations_ = 0;
  std::uint64_t unknown_modifies_ = 0;
  std::uint64_t duplicate_adds_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_BOOK_HPP
