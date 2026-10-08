#include "bookreplay/book.hpp"
#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "book_types.hpp"
#include "toy_book.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::make_book;
using testing::px;
using testing::StreamBuilder;

/// NQU6 and NQM7 instrument ids, from the definition schema of the Aug 2026
/// corpus. StreamBuilder already defaults to the first.
constexpr std::uint32_t kIid = 42004177;
constexpr std::uint32_t kOtherIid = 42019315;

/// Every test replays through here, so every test also checks that the book
/// never disagrees with itself along the way.
template <typename B>
[[nodiscard]] B replay(const std::vector<MboMsg>& records) {
  B b = make_book<B>();
  for (const MboMsg& rec : records) {
    b.apply(rec);
    b.verify();
  }
  return b;
}

template <typename B>
[[nodiscard]] std::vector<std::uint64_t> queue_at(const B& b, Side side, std::int64_t price,
                                                  std::uint32_t instrument_id = kIid) {
  return queue_view(b, instrument_id, side, price);
}

/// Both return 0 for something that is not there, so a regression that drops
/// a level or an order fails as a diff rather than a segfault. No test here
/// expects a resting size of 0.
template <typename B>
[[nodiscard]] std::uint64_t level_size(const B& b, Side side, std::int64_t price,
                                       std::uint32_t instrument_id = kIid) {
  const std::optional<LevelView> lvl = level_view(b, instrument_id, side, price);
  return lvl ? lvl->total : 0;
}

template <typename B>
[[nodiscard]] std::uint32_t order_size(const B& b, std::uint64_t order_id,
                                       std::uint32_t instrument_id = kIid) {
  const std::optional<RestingView> o = resting_view(b, instrument_id, order_id);
  return o ? o->size : 0;
}

template <typename B>
[[nodiscard]] bool has_level(const B& b, Side side, std::int64_t price,
                             std::uint32_t instrument_id = kIid) {
  return level_view(b, instrument_id, side, price).has_value();
}

BOOKREPLAY_BOOK_SUITE(Queue);
BOOKREPLAY_BOOK_SUITE(Add);
BOOKREPLAY_BOOK_SUITE(Modify);
BOOKREPLAY_BOOK_SUITE(ModifyAfterFill);
BOOKREPLAY_BOOK_SUITE(Passive);
BOOKREPLAY_BOOK_SUITE(Cancel);
BOOKREPLAY_BOOK_SUITE(Clear);
BOOKREPLAY_BOOK_SUITE(MutationCount);
BOOKREPLAY_BOOK_SUITE(Reset);
BOOKREPLAY_BOOK_SUITE(BookVsToy);

TYPED_TEST(Queue, ThreeAddsAtOneLevelRestInArrivalOrder) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 7)
      .add(3, Side::kBid, px(29000), 2)
      .last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{14});
}

TYPED_TEST(Queue, AheadIsTheQuantityAFillMustConsumeFirst) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 7)
      .add(3, Side::kBid, px(29000), 2)
      .last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{0});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{5});
  EXPECT_EQ(b.queue_ahead(kIid, 3), std::uint64_t{12});
  EXPECT_EQ(b.queue_ahead(kIid, 99), kNoQueuePosition);
}

TYPED_TEST(Queue, BidsDescendAndAsksAscend) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(28999, 3), 5)
      .add(2, Side::kBid, px(29000), 5)
      .add(3, Side::kAsk, px(29000, 2), 5)
      .add(4, Side::kAsk, px(29000, 1), 5)
      .last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.best_bid(kIid), px(29000));
  EXPECT_EQ(b.best_ask(kIid), px(29000, 1));
}

TYPED_TEST(Add, ARepeatedIdReplacesTheStaleEntryAndIsCounted) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(1, Side::kBid, px(28999, 3), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_FALSE(has_level(b, Side::kBid, px(29000)));
  EXPECT_EQ(b.best_bid(kIid), px(28999, 3));
  EXPECT_EQ(b.order_count(), std::size_t{1});
  EXPECT_EQ(b.duplicate_adds(), std::uint64_t{1});
}

TYPED_TEST(Add, TheSameIdOnTwoInstrumentsIsTwoOrders) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.instrument(kOtherIid).add(1, Side::kBid, px(28000), 3).last();
  s.instrument(kIid).cancel(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_FALSE(b.contains(kIid, 1));
  EXPECT_TRUE(b.contains(kOtherIid, 1));
  EXPECT_EQ(b.best_bid(kOtherIid), px(28000));
  EXPECT_EQ(b.duplicate_adds(), std::uint64_t{0});
}

TYPED_TEST(Modify, SizeDecreaseAtTheSamePriceKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 2).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{2});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{2});
}

TYPED_TEST(Modify, AnUnchangedSizeWithNoFillBehindItKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{12});
}

TYPED_TEST(Modify, SizeIncreaseAtTheSamePriceGoesToTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 9).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{16});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TYPED_TEST(Modify, APriceChangeLosesPriorityEvenWhenTheSizeShrinks) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(28999, 3), 7)
      .add(3, Side::kBid, px(28999, 3), 4);
  s.modify(1, Side::kBid, px(28999, 3), 2).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(28999, 3)), (std::vector<std::uint64_t>{2, 3, 1}));
  EXPECT_FALSE(has_level(b, Side::kBid, px(29000)));
  EXPECT_EQ(b.best_bid(kIid), px(28999, 3));
}

TYPED_TEST(Modify, AnUnknownOrderJoinsAtTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.modify(9, Side::kBid, px(29000), 4).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 9}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.unknown_modifies(), std::uint64_t{1});
}

TYPED_TEST(Modify, AnUnknownOrderOpensItsInstrumentsBook) {
  StreamBuilder s;
  s.modify(9, Side::kBid, px(29000), 4).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{9}));
  EXPECT_EQ(b.instrument_count(), std::size_t{1});
  EXPECT_EQ(b.instruments(), (std::vector<std::uint32_t>{kIid}));
}

// A partial fill and an iceberg refresh both arrive as an M at the same
// price. The fill in front of the M is what tells them apart.
TYPED_TEST(ModifyAfterFill, TheRemainderOfAPartialFillKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 2).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{2});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{2});
}

TYPED_TEST(ModifyAfterFill, AnIcebergRefreshedAtItsOldSizeGoesToTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 1).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 1).fill(1, Side::kBid, px(29000), 1);
  s.modify(1, Side::kBid, px(29000), 1).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TYPED_TEST(ModifyAfterFill, AnIcebergOnTheAskSideFollowsTheSameRule) {
  StreamBuilder s;
  s.add(1, Side::kAsk, px(29000, 1), 1).add(2, Side::kAsk, px(29000, 1), 7);
  s.trade(Side::kBid, px(29000, 1), 1).fill(1, Side::kAsk, px(29000, 1), 1);
  s.modify(1, Side::kAsk, px(29000, 1), 1).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kAsk, px(29000, 1)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kAsk, px(29000, 1)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TYPED_TEST(ModifyAfterFill, ARefreshSmallerThanTheOldDisplayGoesToTheTailToo) {
  // The fill took the displayed 2 and 1 more from hidden quantity; the 1 now
  // shown is a new tranche, not a reduction of the old one.
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 2).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 1).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TYPED_TEST(ModifyAfterFill, ASecondTrancheIsJudgedAgainstItsOwnFills) {
  // Partial fill, refresh, then a fill that overruns the refreshed display.
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 2).fill(1, Side::kBid, px(29000), 2);
  s.modify(1, Side::kBid, px(29000), 3);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 5);
  s.trade(Side::kAsk, px(29000), 7).fill(1, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{3});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{10});
}

TYPED_TEST(ModifyAfterFill, ASecondPartialFillInOneEventIsJudgedOnItsOwn) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 2).fill(1, Side::kBid, px(29000), 2);
  s.modify(1, Side::kBid, px(29000), 3);
  s.trade(Side::kAsk, px(29000), 1).fill(1, Side::kBid, px(29000), 1);
  s.modify(1, Side::kBid, px(29000), 2).last();
  const TypeParam b = replay<TypeParam>(s.records());

  // Were the first fill still attributed, the second M would be judged
  // against 3 filled of the 3 showing, read as a refresh, and go to the tail.
  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{2});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{2});
}

TYPED_TEST(ModifyAfterFill, TwoFillsBeforeOneModifyAreJudgedTogether) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 10).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 2).fill(1, Side::kBid, px(29000), 2);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  // Judged against the last fill alone, 5 is not what a 3 leaves of 10, and
  // the remainder would read as a refresh and go to the tail.
  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{5});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{5});
}

TYPED_TEST(ModifyAfterFill, ARefreshedTrancheKeepsPriorityThroughItsOwnPartialFill) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.trade(Side::kAsk, px(29000), 5).fill(1, Side::kBid, px(29000), 5);
  s.modify(1, Side::kBid, px(29000), 5);
  s.add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 2).fill(1, Side::kBid, px(29000), 2);
  s.modify(1, Side::kBid, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  // Order 2 arrives behind the new tranche so that a second trip to the tail
  // would show. The 5 that emptied the old tranche is not the new one's fill.
  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{3});
}

TYPED_TEST(ModifyAfterFill, AttributionEndsAtTheEventBoundary) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.fill(1, Side::kBid, px(29000), 5).last();
  s.modify(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{12});
}

TYPED_TEST(ModifyAfterFill, ANeighboursBoundaryLeavesThisInstrumentsAttributionAlone) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.fill(1, Side::kBid, px(29000), 5);
  s.instrument(kOtherIid).add(10, Side::kBid, px(28000), 3).last();
  s.instrument(kIid).modify(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  // The M is still judged against the fill, which makes it an iceberg refresh
  // that queues at the tail. Had the neighbour's boundary cleared the
  // attribution, the same M would read as a size that did not grow and would
  // keep its place.
  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TYPED_TEST(Passive, AFillLeavesTheBookAloneSoQueuePositionSurvivesIt) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.fill(1, Side::kBid, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  // The matching size reduction arrives separately as a C or M. Acting on the
  // F as well would count the same match twice.
  EXPECT_EQ(order_size(b, 1), std::uint32_t{5});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{5});
  EXPECT_EQ(b.mutation_count(), std::uint64_t{2});
}

TYPED_TEST(Passive, AFillForAnUnknownOrderInventsNothing) {
  StreamBuilder s;
  s.fill(999, Side::kBid, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_FALSE(b.contains(kIid, 999));
  EXPECT_EQ(b.mutation_count(), std::uint64_t{0});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TYPED_TEST(Passive, TradeAndNoneNeverMutate) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).trade(Side::kAsk, px(29000), 2).none();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{1});
  EXPECT_EQ(order_size(b, 1), std::uint32_t{5});
}

TYPED_TEST(Cancel, EmptyingALevelErasesIt) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).cancel(1, Side::kBid, px(29000), 5).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_FALSE(has_level(b, Side::kBid, px(29000)));
  EXPECT_EQ(b.best_bid(kIid), kUndefPrice);
  EXPECT_EQ(b.order_count(), std::size_t{0});
  // The instrument outlives its last order; only a Clear drops it.
  EXPECT_EQ(b.instruments(), (std::vector<std::uint32_t>{kIid}));
}

TYPED_TEST(Cancel, ErasesByIdWhateverSideTheRecordCarries) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 3).last();
  s.cancel(1, Side::kNone, px(29000), 5).cancel(2, Side::kAsk, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_FALSE(has_level(b, Side::kBid, px(29000)));
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TYPED_TEST(Cancel, AnUnknownOrderIsToleratedAndStillCountsAsOneMutation) {
  StreamBuilder s;
  s.cancel(999, Side::kBid, px(29000), 3).last();
  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{1});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TYPED_TEST(Clear, DropsOneInstrumentAndLeavesItsNeighbourStanding) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kAsk, px(29000, 1), 4);
  s.instrument(kOtherIid).add(10, Side::kBid, px(28000), 3).last();
  s.instrument(kIid).clear().last();

  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.best_bid(kIid), kUndefPrice);
  EXPECT_EQ(b.best_ask(kIid), kUndefPrice);
  EXPECT_EQ(b.best_bid(kOtherIid), px(28000));
  EXPECT_EQ(b.order_count(), std::size_t{1});
  // A Clear drops the instrument outright rather than leaving an empty shell,
  // so `instruments()` lists what holds state, not what the stream has named.
  EXPECT_EQ(b.instruments(), (std::vector<std::uint32_t>{kOtherIid}));
}

TYPED_TEST(MutationCount, AClearCountsOnceHoweverManyOrdersItDrops) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 5)
      .add(3, Side::kAsk, px(29000, 1), 5);
  s.clear().last();

  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{4});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TYPED_TEST(MutationCount, SnapshotRecordsCountLikeAnyOther) {
  StreamBuilder s;
  s.clear();
  s.add(1, Side::kBid, px(29000), 10).snapshot();
  s.add(2, Side::kAsk, px(29000, 1), 10).snapshot().last();

  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{3});
  EXPECT_EQ(b.best_bid(kIid), px(29000));
  EXPECT_EQ(b.best_ask(kIid), px(29000, 1));
}

TYPED_TEST(MutationCount, AnUnplaceableRecordCountsButRestsNothing) {
  StreamBuilder s;
  s.add(1, Side::kNone, px(29000), 5);
  s.add(0, Side::kBid, px(29000), 5).last();

  const TypeParam b = replay<TypeParam>(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{2});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TYPED_TEST(Reset, DropsEveryInstrumentAndKeepsEveryCounter) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.modify(9, Side::kBid, px(29000), 4).last();
  TypeParam b = replay<TypeParam>(s.records());

  b.reset();

  EXPECT_EQ(b.instrument_count(), std::size_t{0});
  EXPECT_EQ(b.order_count(), std::size_t{0});
  EXPECT_EQ(b.best_bid(kIid), kUndefPrice);
  // A reset is for reusing the book across days, not for restarting the
  // audit: what the stream got wrong before it stays counted.
  EXPECT_EQ(b.mutation_count(), std::uint64_t{2});
  EXPECT_EQ(b.unknown_modifies(), std::uint64_t{1});
}

// run_checked over the shared fixtures lives in test_invariants.cpp, beside
// the other BookLike implementations. What belongs here is the comparison the
// two books make possible: the same top of book and the same count, from
// different queue fidelity. ToyBook keeps no per-level queue, so the touch is
// all it can be asked about.

template <typename B>
void expect_agreement(const std::vector<MboMsg>& records, B& book, testing::ToyBook& toy) {
  for (const MboMsg& rec : records) {
    book.apply(rec);
    book.verify();
    toy.apply(rec);
    EXPECT_EQ(book.mutation_count(), toy.mutation_count());
    EXPECT_EQ(book.best_bid(kIid), toy.best_bid(kIid));
    EXPECT_EQ(book.best_ask(kIid), toy.best_ask(kIid));
  }
}

TYPED_TEST(BookVsToy, TopOfBookAndCountAgreeThroughTheIcebergFixture) {
  TypeParam book = make_book<TypeParam>();
  testing::ToyBook toy;
  expect_agreement(testing::iceberg_then_market_moves_up(), book, toy);

  EXPECT_EQ(book.unknown_modifies(), std::uint64_t{0});
  EXPECT_EQ(book.duplicate_adds(), std::uint64_t{0});
}

TYPED_TEST(BookVsToy, ARepeatedIdOrphansSizeInNeitherBook) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(1, Side::kBid, px(28999, 3), 5).last();

  TypeParam book = make_book<TypeParam>();
  testing::ToyBook toy;
  expect_agreement(s.records(), book, toy);

  // Were ToyBook to insert over the stale entry instead of erasing it, the
  // orphaned 5 would hold 29000 as its bid for good. The touch is the only
  // place an aggregate view can show that.
  EXPECT_EQ(book.best_bid(kIid), px(28999, 3));
  EXPECT_EQ(book.duplicate_adds(), std::uint64_t{1});
}

}  // namespace
}  // namespace bookreplay
