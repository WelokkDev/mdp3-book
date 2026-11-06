#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/queue_positions.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::px;
using testing::StreamBuilder;

using Kind = QueuePositions::ReachKind;

constexpr std::int64_t kPrice = px(29000);
constexpr std::uint64_t k0 = 0;
constexpr std::uint64_t k1 = 1;

struct Reached {
  std::uint64_t tag = 0;
  Kind kind = Kind::kAtPrice;
  std::uint64_t quantity = 0;
  std::uint32_t print = 0;

  friend bool operator==(const Reached&, const Reached&) = default;
};

/// The resting orders a book would hold, as far as the accounting asks: it
/// only ever looks one up by id.
class Resting {
 public:
  Resting& order(std::uint64_t id, Side side, std::int64_t price, std::uint32_t size,
                 std::uint64_t stamp, std::uint64_t filled = 0) {
    orders_[id] = RestingView{side, price, size, filled, stamp};
    return *this;
  }

  [[nodiscard]] std::optional<RestingView> find(std::uint64_t id) {
    ++lookups_;
    const auto it = orders_.find(id);
    return it == orders_.end() ? std::nullopt : std::optional<RestingView>{it->second};
  }

  [[nodiscard]] std::size_t lookups() const noexcept { return lookups_; }

 private:
  std::unordered_map<std::uint64_t, RestingView> orders_;
  std::size_t lookups_ = 0;
};

std::vector<Reached> run(QueuePositions& positions, Resting& resting, const StreamBuilder& event) {
  for (const MboMsg& rec : event.records()) {
    const auto lookup = [&resting, &rec] { return resting.find(rec.order_id); };
    switch (action_of(rec)) {
      case Action::kTrade:
        positions.on_trade(rec);
        break;
      case Action::kFill:
        positions.on_fill(rec, lookup);
        break;
      case Action::kModify:
        positions.on_modify(rec, lookup);
        break;
      default:
        break;
    }
  }
  std::vector<Reached> out;
  positions.close_event([&out](const QueuePositions::Reach& reach, const MboMsg&) {
    out.push_back(Reached{reach.tag, reach.kind, reach.quantity, reach.print});
  });
  return out;
}

TEST(QueuePositions, WhatReachesAPlaceIsThePrintLessTheFillsOfOrdersAhead) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5).order(101, Side::kBid, kPrice, 6, 12);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 7)
      .fill(100, Side::kBid, kPrice, 4)
      .fill(101, Side::kBid, kPrice, 3);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 3, 0}}));
}

TEST(QueuePositions, APrintThatStopsShortIsStillReportedWithNothing) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 4).fill(100, Side::kBid, kPrice, 4);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 0, 0}}));
}

TEST(QueuePositions, AnOrderStampedAtThePlaceIsAheadOfItUnlessThePlaceStandsInForIt) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  (void)positions.place(Side::kBid, kPrice, 10, 2, 100);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 10);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 4).fill(100, Side::kBid, kPrice, 4);

  // The second place is order 100's own: its four lots are what reached it.
  EXPECT_EQ(run(positions, resting, event),
            (std::vector<Reached>{{1, Kind::kAtPrice, 0, 0}, {2, Kind::kAtPrice, 4, 0}}));
}

TEST(QueuePositions, PlacesAtOnePriceComeOutByStampAndThenAsPlaced) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 9, 3);
  (void)positions.place(Side::kBid, kPrice, 5, 1);
  (void)positions.place(Side::kBid, kPrice, 9, 2);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 2, 7);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 2).fill(100, Side::kBid, kPrice, 2);

  EXPECT_EQ(run(positions, resting, event),
            (std::vector<Reached>{
                {1, Kind::kAtPrice, 2, 0}, {3, Kind::kAtPrice, 0, 0}, {2, Kind::kAtPrice, 0, 0}}));
}

TEST(QueuePositions, APrintBringsItsWholeSizeToBetterPricedPlacesFirst) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  (void)positions.place(Side::kBid, px(29000, 1), 10, 2);
  (void)positions.place(Side::kBid, px(29000, 2), 10, 3);
  (void)positions.place(Side::kBid, px(28999, 3), 10, 4);
  (void)positions.place(Side::kAsk, px(28999, 2), 10, 5);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 2, 12);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 2).fill(100, Side::kBid, kPrice, 2);

  // The bid a tick lower was not reached, and an ask is not a bid however
  // much lower its price.
  EXPECT_EQ(run(positions, resting, event),
            (std::vector<Reached>{
                {3, Kind::kThrough, 2, 0}, {2, Kind::kThrough, 2, 0}, {1, Kind::kAtPrice, 2, 0}}));
}

TEST(QueuePositions, ForAnAskBetterIsLower) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kAsk, px(29001), 10, 1);
  (void)positions.place(Side::kAsk, px(29001, 2), 10, 2);
  Resting resting;
  resting.order(100, Side::kAsk, px(29001, 1), 2, 12);
  StreamBuilder event;
  event.trade(Side::kBid, px(29001, 1), 2).fill(100, Side::kAsk, px(29001, 1), 2);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kThrough, 2, 0}}));
  EXPECT_EQ(positions.best(Side::kAsk), px(29001));
  EXPECT_EQ(positions.best(Side::kBid), kUndefPrice);
}

TEST(QueuePositions, EachPrintOfAnEventIsTalliedOnItsOwn) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5).order(101, Side::kBid, kPrice, 6, 12);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 4).fill(100, Side::kBid, kPrice, 4);
  event.trade(Side::kAsk, kPrice, 2).fill(101, Side::kBid, kPrice, 2);

  EXPECT_EQ(run(positions, resting, event),
            (std::vector<Reached>{{1, Kind::kAtPrice, 0, 0}, {1, Kind::kAtPrice, 2, 1}}));
}

TEST(QueuePositions, OnlyTheTrancheStillShowingIsAhead) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  // Three of order 100's five were filled earlier in the event.
  resting.order(100, Side::kBid, kPrice, 5, 5, 3);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 6).fill(100, Side::kBid, kPrice, 6);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 4, 0}}));
  EXPECT_EQ(counters.hidden_lots, std::uint64_t{4});
}

TEST(QueuePositions, AFillOnAnOrderTheBookDoesNotHoldIsAheadOfEveryPlace) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  (void)positions.place(Side::kBid, kPrice, 2, 2);
  Resting resting;
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 5).fill(999, Side::kBid, kPrice, 3);

  EXPECT_EQ(run(positions, resting, event),
            (std::vector<Reached>{{2, Kind::kAtPrice, 2, 0}, {1, Kind::kAtPrice, 2, 0}}));
  EXPECT_EQ(counters.fills_on_unknown_orders, k1);
  EXPECT_EQ(counters.unattributed_lots, std::uint64_t{2});
}

TEST(QueuePositions, AFillOnAnOrderRestingAtAnotherPriceIsBehindEveryPlace) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, px(28999), 3, 5);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 3).fill(100, Side::kBid, kPrice, 3);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 3, 0}}));
  EXPECT_EQ(counters.fills_off_resting_price, k1);
}

TEST(QueuePositions, AFillOnAnOrderRestingOnTheOtherSideIsBehindEveryPlace) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kAsk, kPrice, 3, 5);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 3).fill(100, Side::kBid, kPrice, 3);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 3, 0}}));
  EXPECT_EQ(counters.fills_off_resting_price, k1);
}

TEST(QueuePositions, TheAggressorsOwnFillIsSetAside) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5).order(9, Side::kAsk, px(29001), 4, 6);
  StreamBuilder event;
  event.trade(Side::kAsk, kPrice, 4).aggressor(9).fill(9, Side::kAsk, kPrice, 4);
  event.fill(100, Side::kBid, kPrice, 4);

  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 0, 0}}));
  EXPECT_EQ(counters.aggressor_fills, k1);
  EXPECT_EQ(counters.prints_on_both_sides, k0);
}

TEST(QueuePositions, APrintFilledOnBothSidesReachesNothingAndLeavesNoTallyBehind) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5).order(200, Side::kAsk, kPrice, 4, 6);
  resting.order(101, Side::kBid, kPrice, 4, 12);
  StreamBuilder event;
  event.trade(Side::kNone, kPrice, 4)
      .fill(100, Side::kBid, kPrice, 4)
      .fill(200, Side::kAsk, kPrice, 4);
  event.trade(Side::kAsk, kPrice, 3).fill(101, Side::kBid, kPrice, 3);

  // Order 100's four lots were tallied before the ask's fill showed the print
  // for an uncross; they must not be held against the print that follows.
  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kAtPrice, 3, 1}}));
  EXPECT_EQ(counters.prints_on_both_sides, k1);
}

TEST(QueuePositions, AFillWithNoSideTakesItsOrdersOrFailingThatVoidsThePrint) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5);
  StreamBuilder known;
  known.trade(Side::kAsk, kPrice, 6).fill(100, Side::kNone, kPrice, 4);
  StreamBuilder unknown;
  unknown.trade(Side::kAsk, kPrice, 6).fill(999, Side::kNone, kPrice, 4);

  EXPECT_EQ(run(positions, resting, known), (std::vector<Reached>{{1, Kind::kAtPrice, 2, 0}}));
  EXPECT_TRUE(run(positions, resting, unknown).empty());
  EXPECT_EQ(counters.fills_without_a_side, k1);
  EXPECT_EQ(counters.prints_without_fills, k0);
}

TEST(QueuePositions, WhatCannotBePlacedIsCountedWhereAPlaceStoodToGain) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5);
  StreamBuilder stray_fill;
  stray_fill.fill(100, Side::kBid, kPrice, 4);
  StreamBuilder bare_print;
  bare_print.trade(Side::kAsk, kPrice, 4);
  StreamBuilder bare_print_elsewhere;
  bare_print_elsewhere.trade(Side::kBid, kPrice, 4);
  StreamBuilder no_aggressor;
  no_aggressor.trade(Side::kNone, kPrice, 5).fill(100, Side::kBid, kPrice, 4);

  EXPECT_TRUE(run(positions, resting, stray_fill).empty());
  EXPECT_EQ(counters.fills_before_a_print, k1);
  EXPECT_TRUE(run(positions, resting, bare_print).empty());
  EXPECT_TRUE(run(positions, resting, bare_print_elsewhere).empty());
  EXPECT_EQ(counters.prints_without_fills, k1);
  EXPECT_EQ(run(positions, resting, no_aggressor),
            (std::vector<Reached>{{1, Kind::kAtPrice, 1, 0}}));
  EXPECT_EQ(counters.no_aggressor_reaches, k1);
  EXPECT_EQ(counters.unattributed_lots, k1);
}

TEST(QueuePositions, APrintWithNoFillIsCountedThroughAPlaceAsWellAsAtIt) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  (void)positions.place(Side::kAsk, px(29001), 10, 2);
  Resting resting;
  StreamBuilder short_of_both;
  short_of_both.trade(Side::kAsk, px(29000, 1), 4).trade(Side::kBid, px(29000, 3), 4);
  StreamBuilder through_the_bid;
  through_the_bid.trade(Side::kAsk, px(28999, 3), 4);
  StreamBuilder through_the_ask;
  through_the_ask.trade(Side::kBid, px(29001, 1), 4);

  // A seller a tick above the bid never got to it, nor a buyer a tick below
  // the ask. To trade past either, a print had to pass it first.
  EXPECT_TRUE(run(positions, resting, short_of_both).empty());
  EXPECT_EQ(counters.prints_without_fills, k0);
  EXPECT_TRUE(run(positions, resting, through_the_bid).empty());
  EXPECT_EQ(counters.prints_without_fills, k1);
  EXPECT_TRUE(run(positions, resting, through_the_ask).empty());
  EXPECT_EQ(counters.prints_without_fills, std::uint64_t{2});
}

TEST(QueuePositions, APrintWithNoFillAndNoAggressorIsCountedOnEitherSide) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  (void)positions.place(Side::kAsk, px(29001), 10, 2);
  Resting resting;
  StreamBuilder between;
  between.trade(Side::kNone, px(29000, 2), 4);
  StreamBuilder at_the_ask;
  at_the_ask.trade(Side::kNone, px(29001), 4);
  StreamBuilder through_the_bid;
  through_the_bid.trade(Side::kNone, px(28999, 3), 4);

  EXPECT_TRUE(run(positions, resting, between).empty());
  EXPECT_EQ(counters.prints_without_fills, k0);
  EXPECT_TRUE(run(positions, resting, at_the_ask).empty());
  EXPECT_EQ(counters.prints_without_fills, k1);
  EXPECT_TRUE(run(positions, resting, through_the_bid).empty());
  EXPECT_EQ(counters.prints_without_fills, std::uint64_t{2});
}

TEST(QueuePositions, AModifyThatShowsNoChangeIsCountedOnlyAtAPriceSomeoneHolds) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, kPrice, 4, 5).order(101, Side::kBid, px(28999), 4, 6);
  resting.order(102, Side::kBid, kPrice, 4, 7, 2);
  StreamBuilder event;
  event.modify(100, Side::kBid, kPrice, 4).modify(101, Side::kBid, px(28999), 4);
  event.modify(100, Side::kBid, kPrice, 3).modify(102, Side::kBid, kPrice, 4);

  (void)run(positions, resting, event);

  // Order 101's is at a price nobody holds; the third changes the size; the
  // fourth follows a fill, which makes it an iceberg's refresh.
  EXPECT_EQ(counters.unchanged_modifies, k1);
  EXPECT_EQ(resting.lookups(), std::size_t{3});
}

TEST(QueuePositions, TheBookIsAskedOnlyAboutFillsAtAPriceSomeoneHolds) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kBid, kPrice, 10, 1);
  Resting resting;
  resting.order(100, Side::kBid, px(28999), 4, 5).order(200, Side::kAsk, px(29001), 4, 6);
  StreamBuilder event;
  event.trade(Side::kAsk, px(28999), 4).fill(100, Side::kBid, px(28999), 4);
  event.trade(Side::kBid, px(29001), 4).fill(200, Side::kAsk, px(29001), 4);

  // The first print is through the place and says so without a lookup.
  EXPECT_EQ(run(positions, resting, event), (std::vector<Reached>{{1, Kind::kThrough, 4, 0}}));
  EXPECT_EQ(resting.lookups(), std::size_t{0});
}

TEST(QueuePositions, ARemovedPlaceIsGoneAndItsSlotIsUsedAgain) {
  QueueCounters counters;
  QueuePositions positions{counters};
  const QueuePositions::Slot first = positions.place(Side::kBid, kPrice, 10, 1);
  const QueuePositions::Slot second = positions.place(Side::kBid, px(29000, 1), 10, 2);
  ASSERT_EQ(positions.best(Side::kBid), px(29000, 1));

  positions.remove(second);
  EXPECT_EQ(positions.best(Side::kBid), kPrice);
  EXPECT_EQ(positions.place(Side::kAsk, px(29001), 3, 3), second);

  std::vector<std::pair<QueuePositions::Slot, std::uint64_t>> seen;
  positions.for_each_at_or_better(
      Side::kBid, px(28999),
      [&seen](QueuePositions::Slot slot, std::uint64_t tag) { seen.emplace_back(slot, tag); });
  EXPECT_EQ(seen, (std::vector<std::pair<QueuePositions::Slot, std::uint64_t>>{{first, 1}}));

  positions.clear();
  EXPECT_TRUE(positions.empty());
  EXPECT_EQ(positions.best(Side::kAsk), kUndefPrice);
}

TEST(QueuePositions, PlacesAreVisitedBestPriceFirstDownToTheBound) {
  QueueCounters counters;
  QueuePositions positions{counters};
  (void)positions.place(Side::kAsk, px(29001, 2), 10, 3);
  (void)positions.place(Side::kAsk, px(29001), 10, 1);
  (void)positions.place(Side::kAsk, px(29001, 1), 12, 2);
  (void)positions.place(Side::kAsk, px(29001, 1), 11, 4);

  std::vector<std::uint64_t> tags;
  positions.for_each_at_or_better(
      Side::kAsk, px(29001, 1),
      [&tags](QueuePositions::Slot, std::uint64_t tag) { tags.push_back(tag); });

  EXPECT_EQ(tags, (std::vector<std::uint64_t>{1, 4, 2}));
}

}  // namespace
}  // namespace bookreplay
