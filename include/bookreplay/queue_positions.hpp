// Places in a book's queues held by things the book does not hold, and what
// each print of an event brought to them.
//
// A simulated order must never enter the book. The book is held to the venue
// record for record, and an order the venue never saw would end that. So it
// stands beside the book as a position: a side, a price, and a stamp that
// places it among the orders resting there (see `Book::Resting::stamp`).
//
// What reaches a position is the print's size less what the print's fills
// gave to visible orders ahead of it, not the sum of the fills behind it. The
// two agree when every lot a print reports is attributed to a resting order.
// Where they differ, the lots nobody was named for are implied liquidity,
// which the venue ranks below every outright order at the price, so they did
// pass the position. And a position standing in for a real order has to be
// sized without that order's own fills, which only the first form allows.

#ifndef BOOKREPLAY_QUEUE_POSITIONS_HPP
#define BOOKREPLAY_QUEUE_POSITIONS_HPP

#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bookreplay {

/// What the accounting met that the feed does not settle, each counted.
struct QueueCounters {
  /// F's naming their own print's aggressor: an order that rested, was moved
  /// into the market, and is reported as filled though nothing hit it.
  std::uint64_t aggressor_fills = 0;
  /// F's naming no order the book holds. Counted as ahead of every position.
  std::uint64_t fills_on_unknown_orders = 0;
  /// F's on an order the book holds at another price, or on the other side.
  /// It came to this level inside the event, so it is behind every position.
  std::uint64_t fills_off_resting_price = 0;
  /// F's with side 'N' on an order the book does not hold: which side the
  /// print hit is then unknown, and the print reaches nothing.
  std::uint64_t fills_without_a_side = 0;
  std::uint64_t fills_before_a_print = 0;
  /// Lots filled beyond an order's displayed size.
  std::uint64_t hidden_lots = 0;
  /// Prints whose fills rested on both sides: an uncross, which has no queue.
  std::uint64_t prints_on_both_sides = 0;
  /// Prints followed by no resting order's fill, at or through a price holding
  /// a position on the side the aggressor faced, or on either side if the print
  /// names no aggressor.
  std::uint64_t prints_without_fills = 0;
  /// Print size beyond the fills that followed it.
  std::uint64_t unattributed_lots = 0;
  std::uint64_t no_aggressor_reaches = 0;
  /// M's that change neither price nor size with no fill before them. The feed
  /// shows nothing of what changed; if it was the account, the venue re-queued
  /// the order and the book did not.
  std::uint64_t unchanged_modifies = 0;
};

class QueuePositions {
 public:
  using Slot = std::uint32_t;

  static constexpr Slot kNoSlot = std::numeric_limits<Slot>::max();

  enum class ReachKind : std::uint8_t {
    kAtPrice,
    kThrough,  ///< traded at a worse price, which the venue reaches only past this one
  };

  struct Reach {
    Slot slot = kNoSlot;
    std::uint64_t tag = 0;
    /// Which of the event's prints, in stream order.
    std::uint32_t print = 0;
    ReachKind kind = ReachKind::kAtPrice;
    /// For kAtPrice, the lots that got past everything ahead; 0 when the
    /// print stopped short. For kThrough, the print's whole size.
    std::uint64_t quantity = 0;
  };

  /// Counts into `counters`, which must outlive this.
  explicit QueuePositions(QueueCounters& counters) : counters_(&counters) {}

  /// Everything resting at (side, price) stamped at or below `stamp` is ahead
  /// of the new position, except the order `blind_to` names: a position that
  /// stands in for a real order takes that order's stamp and must not count
  /// the order's own fills against itself. `tag` comes back with each reach.
  [[nodiscard]] Slot place(Side side, std::int64_t price, std::uint64_t stamp, std::uint64_t tag,
                           std::uint64_t blind_to = 0);

  void remove(Slot slot);

  /// Drops every position, and whatever the open event had accumulated.
  void clear();

  [[nodiscard]] bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

  /// kUndefPrice when `side` holds no position.
  [[nodiscard]] std::int64_t best(Side side) const noexcept;

  /// Every position on `side` priced at or better than `price`, best price
  /// first and by stamp within one, as visit(slot, tag).
  template <typename Visit>
  void for_each_at_or_better(Side side, std::int64_t price, Visit&& visit) const {
    const Ladder& held = ladder(side);
    const std::int64_t bound = key(side, price);
    for (auto it = held.begin(); it != held.end() && it->first <= bound; ++it) {
      for (const Slot slot : it->second) {
        visit(slot, positions_[slot].tag);
      }
    }
  }

  // An event's records, in stream order and before the book applies each:
  // which orders a fill left ahead is read from the queue as it stood, and
  // the venue publishes the book's changes only after the event's last fill.
  // A print's reaches are worked out when the next print or `close_event`
  // ends it, against the positions standing then. So `place` and `remove`
  // must not be called between an event's first print and its close, and an
  // event that had a print must be closed.

  void on_trade(const MboMsg& trade);

  /// `resting` is called only if the answer matters, and returns the book's
  /// view of the order the fill names: std::optional<RestingView>.
  template <typename Lookup>
  void on_fill(const MboMsg& fill, Lookup&& resting) {
    if (!note_fill(fill)) {
      return;
    }
    const std::optional<RestingView> found = std::forward<Lookup>(resting)();
    tally(fill, found ? &*found : nullptr);
  }

  /// As `on_fill`; an M changes no accounting and is only ever counted.
  template <typename Lookup>
  void on_modify(const MboMsg& modify, Lookup&& resting) {
    if (level_at(side_of(modify), modify.price) == nullptr) {
      return;
    }
    const std::optional<RestingView> found = std::forward<Lookup>(resting)();
    if (found && unchanged(modify, *found)) {
      ++counters_->unchanged_modifies;
    }
  }

  /// Hands over every reach of the event as sink(reach, trade): prints in
  /// stream order; within a print the positions it traded through, best price
  /// first and by stamp within one, then those at its price by stamp. The sink
  /// must not place or remove positions.
  template <typename Sink>
  void close_event(Sink&& sink) {
    finish_print();
    for (const Reach& reach : reaches_) {
      sink(reach, prints_[reach.print].trade);
    }
    prints_.clear();
    reaches_.clear();
  }

 private:
  struct Position {
    Side side = Side::kNone;
    std::int64_t price = 0;
    std::uint64_t stamp = 0;
    std::uint64_t blind_to = 0;
    std::uint64_t tag = 0;
    /// Visible lots the open print's fills gave to orders ahead.
    std::uint64_t ahead = 0;
    Slot next_free = kNoSlot;
  };

  struct Print {
    MboMsg trade{};
    /// The side its fills rested on.
    Side hit = Side::kNone;
    bool reaches_nothing = false;
    std::uint64_t filled = 0;
  };

  using Level = std::vector<Slot>;
  /// Keyed so that the best price comes first on either side.
  using Ladder = std::map<std::int64_t, Level>;

  /// The complement reverses the order of bids without the overflow that
  /// negating the lowest price a record can carry would be. It is its own
  /// inverse.
  [[nodiscard]] static constexpr std::int64_t key(Side side, std::int64_t price) noexcept {
    return side == Side::kBid ? ~price : price;
  }

  [[nodiscard]] Ladder& ladder(Side side) noexcept { return side == Side::kBid ? bids_ : asks_; }

  [[nodiscard]] const Ladder& ladder(Side side) const noexcept {
    return side == Side::kBid ? bids_ : asks_;
  }

  [[nodiscard]] Level* level_at(Side side, std::int64_t price);
  [[nodiscard]] static bool unchanged(const MboMsg& modify, const RestingView& resting) noexcept;
  /// True when the fill's order has to be looked up.
  [[nodiscard]] bool note_fill(const MboMsg& fill);
  void tally(const MboMsg& fill, const RestingView* resting);
  void note_side(Print& print, Side side);
  void forget_tallies(Side side, std::int64_t price);
  void finish_print();

  QueueCounters* counters_;
  std::vector<Position> positions_;
  Slot free_ = kNoSlot;
  Ladder bids_;
  Ladder asks_;
  std::vector<Print> prints_;
  std::vector<Reach> reaches_;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_QUEUE_POSITIONS_HPP
