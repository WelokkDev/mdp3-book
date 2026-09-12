#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "toy_book.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::px;
using testing::StreamBuilder;

/// NQU6 and NQM7 instrument ids, from the definition schema of the Aug 2026
/// corpus. StreamBuilder already defaults to the first.
constexpr std::uint32_t kIid = 42004177;
constexpr std::uint32_t kOtherIid = 42019315;

/// Every test replays through here, so every test also checks that the book
/// never disagrees with itself along the way.
[[nodiscard]] Book replay(const std::vector<MboMsg>& records) {
  Book b;
  for (const MboMsg& rec : records) {
    b.apply(rec);
    b.verify();
  }
  return b;
}

[[nodiscard]] std::vector<std::uint64_t> queue_at(const Book& b, Side side, std::int64_t price,
                                                  std::uint32_t instrument_id = kIid) {
  const Book::Level* lvl = b.level(instrument_id, side, price);
  if (lvl == nullptr) {
    return {};
  }
  return {lvl->queue.begin(), lvl->queue.end()};
}

/// Both return 0 for something that is not there, so a regression that drops
/// a level or an order fails as a diff rather than a segfault. No test here
/// expects a resting size of 0.
[[nodiscard]] std::uint64_t level_size(const Book& b, Side side, std::int64_t price,
                                       std::uint32_t instrument_id = kIid) {
  const Book::Level* lvl = b.level(instrument_id, side, price);
  return lvl == nullptr ? 0 : lvl->size;
}

[[nodiscard]] std::uint32_t order_size(const Book& b, std::uint64_t order_id,
                                       std::uint32_t instrument_id = kIid) {
  const Book::Resting* o = b.order(instrument_id, order_id);
  return o == nullptr ? 0 : o->size;
}

TEST(Queue, ThreeAddsAtOneLevelRestInArrivalOrder) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 7)
      .add(3, Side::kBid, px(29000), 2)
      .last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{14});
}

TEST(Queue, AheadIsTheQuantityAFillMustConsumeFirst) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 7)
      .add(3, Side::kBid, px(29000), 2)
      .last();
  const Book b = replay(s.records());

  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{0});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{5});
  EXPECT_EQ(b.queue_ahead(kIid, 3), std::uint64_t{12});
  EXPECT_EQ(b.queue_ahead(kIid, 99), kNoQueuePosition);
}

TEST(Queue, BidsDescendAndAsksAscend) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(28999, 3), 5)
      .add(2, Side::kBid, px(29000), 5)
      .add(3, Side::kAsk, px(29000, 2), 5)
      .add(4, Side::kAsk, px(29000, 1), 5)
      .last();
  const Book b = replay(s.records());

  EXPECT_EQ(b.best_bid(kIid), px(29000));
  EXPECT_EQ(b.best_ask(kIid), px(29000, 1));
}

TEST(Add, ARepeatedIdReplacesTheStaleEntryAndIsCounted) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(1, Side::kBid, px(28999, 3), 5).last();
  const Book b = replay(s.records());

  EXPECT_EQ(b.level(kIid, Side::kBid, px(29000)), nullptr);
  EXPECT_EQ(b.best_bid(kIid), px(28999, 3));
  EXPECT_EQ(b.order_count(), std::size_t{1});
  EXPECT_EQ(b.duplicate_adds(), std::uint64_t{1});
}

TEST(Modify, SizeDecreaseAtTheSamePriceKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 2).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{2});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{2});
}

TEST(Modify, AnUnchangedSizeWithNoFillBehindItKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 5).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{12});
}

TEST(Modify, SizeIncreaseAtTheSamePriceGoesToTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 9).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{16});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TEST(Modify, APriceChangeLosesPriorityEvenWhenTheSizeShrinks) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(28999, 3), 7)
      .add(3, Side::kBid, px(28999, 3), 4);
  s.modify(1, Side::kBid, px(28999, 3), 2).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(28999, 3)), (std::vector<std::uint64_t>{2, 3, 1}));
  EXPECT_EQ(b.level(kIid, Side::kBid, px(29000)), nullptr);
  EXPECT_EQ(b.best_bid(kIid), px(28999, 3));
}

TEST(Modify, AnUnknownOrderJoinsAtTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.modify(9, Side::kBid, px(29000), 4).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 9}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.unknown_modifies(), std::uint64_t{1});
}

// A partial fill and an iceberg refresh both arrive as an M at the same
// price. The fill in front of the M is what tells them apart.
TEST(ModifyAfterFill, TheRemainderOfAPartialFillKeepsPriority) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 2).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{2});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{9});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{2});
}

TEST(ModifyAfterFill, AnIcebergRefreshedAtItsOldSizeGoesToTheTail) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 1).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 1).fill(1, Side::kBid, px(29000), 1);
  s.modify(1, Side::kBid, px(29000), 1).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TEST(ModifyAfterFill, AnIcebergOnTheAskSideFollowsTheSameRule) {
  StreamBuilder s;
  s.add(1, Side::kAsk, px(29000, 1), 1).add(2, Side::kAsk, px(29000, 1), 7);
  s.trade(Side::kBid, px(29000, 1), 1).fill(1, Side::kAsk, px(29000, 1), 1);
  s.modify(1, Side::kAsk, px(29000, 1), 1).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kAsk, px(29000, 1)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kAsk, px(29000, 1)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TEST(ModifyAfterFill, ARefreshSmallerThanTheOldDisplayGoesToTheTailToo) {
  // The fill took the displayed 2 and 1 more from hidden quantity; the 1 now
  // shown is a new tranche, not a reduction of the old one.
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 2).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 1).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{8});
  EXPECT_EQ(b.queue_ahead(kIid, 1), std::uint64_t{7});
}

TEST(ModifyAfterFill, ASecondTrancheIsJudgedAgainstItsOwnFills) {
  // Partial fill, refresh, then a fill that overruns the refreshed display.
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.trade(Side::kAsk, px(29000), 2).fill(1, Side::kBid, px(29000), 2);
  s.modify(1, Side::kBid, px(29000), 3);
  s.trade(Side::kAsk, px(29000), 3).fill(1, Side::kBid, px(29000), 3);
  s.modify(1, Side::kBid, px(29000), 5);
  s.trade(Side::kAsk, px(29000), 7).fill(1, Side::kBid, px(29000), 7);
  s.modify(1, Side::kBid, px(29000), 3).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(order_size(b, 1), std::uint32_t{3});
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{10});
}

TEST(ModifyAfterFill, AttributionEndsAtTheEventBoundary) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.fill(1, Side::kBid, px(29000), 5).last();
  s.modify(1, Side::kBid, px(29000), 5).last();
  const Book b = replay(s.records());

  EXPECT_EQ(queue_at(b, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(level_size(b, Side::kBid, px(29000)), std::uint64_t{12});
}

TEST(Passive, AFillLeavesTheBookAloneSoQueuePositionSurvivesIt) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.fill(1, Side::kBid, px(29000), 3).last();
  const Book b = replay(s.records());

  // The matching size reduction arrives separately as a C or M. Acting on the
  // F as well would count the same match twice.
  EXPECT_EQ(order_size(b, 1), std::uint32_t{5});
  EXPECT_EQ(b.queue_ahead(kIid, 2), std::uint64_t{5});
  EXPECT_EQ(b.mutation_count(), std::uint64_t{2});
}

TEST(Passive, AFillForAnUnknownOrderInventsNothing) {
  StreamBuilder s;
  s.fill(999, Side::kBid, px(29000), 3).last();
  const Book b = replay(s.records());

  EXPECT_FALSE(b.contains(kIid, 999));
  EXPECT_EQ(b.mutation_count(), std::uint64_t{0});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TEST(Passive, TradeAndNoneNeverMutate) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).trade(Side::kAsk, px(29000), 2).none();
  const Book b = replay(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{1});
  EXPECT_EQ(order_size(b, 1), std::uint32_t{5});
}

TEST(Cancel, EmptyingALevelErasesIt) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).cancel(1, Side::kBid, px(29000), 5).last();
  const Book b = replay(s.records());

  EXPECT_EQ(b.level(kIid, Side::kBid, px(29000)), nullptr);
  EXPECT_EQ(b.best_bid(kIid), kUndefPrice);
  EXPECT_EQ(b.order_count(), std::size_t{0});
  // The instrument outlives its last order; only a Clear drops it.
  EXPECT_EQ(b.instruments(), (std::vector<std::uint32_t>{kIid}));
}

TEST(Cancel, AnUnknownOrderIsToleratedAndStillCountsAsOneMutation) {
  StreamBuilder s;
  s.cancel(999, Side::kBid, px(29000), 3).last();
  const Book b = replay(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{1});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TEST(Clear, DropsOneInstrumentAndLeavesItsNeighbourStanding) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kAsk, px(29000, 1), 4);
  s.instrument(kOtherIid).add(10, Side::kBid, px(28000), 3).last();
  s.instrument(kIid).clear().last();

  const Book b = replay(s.records());

  EXPECT_EQ(b.best_bid(kIid), kUndefPrice);
  EXPECT_EQ(b.best_ask(kIid), kUndefPrice);
  EXPECT_EQ(b.best_bid(kOtherIid), px(28000));
  EXPECT_EQ(b.order_count(), std::size_t{1});
  // A Clear drops the instrument outright rather than leaving an empty shell,
  // so `instruments()` lists what holds state, not what the stream has named.
  EXPECT_EQ(b.instruments(), (std::vector<std::uint32_t>{kOtherIid}));
}

TEST(MutationCount, AClearCountsOnceHoweverManyOrdersItDrops) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5)
      .add(2, Side::kBid, px(29000), 5)
      .add(3, Side::kAsk, px(29000, 1), 5);
  s.clear().last();

  const Book b = replay(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{4});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TEST(MutationCount, SnapshotRecordsCountLikeAnyOther) {
  StreamBuilder s;
  s.clear();
  s.add(1, Side::kBid, px(29000), 10).snapshot();
  s.add(2, Side::kAsk, px(29000, 1), 10).snapshot().last();

  const Book b = replay(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{3});
  EXPECT_EQ(b.best_bid(kIid), px(29000));
  EXPECT_EQ(b.best_ask(kIid), px(29000, 1));
}

TEST(MutationCount, AnUnplaceableRecordCountsButRestsNothing) {
  StreamBuilder s;
  s.add(1, Side::kNone, px(29000), 5);
  s.add(0, Side::kBid, px(29000), 5).last();

  const Book b = replay(s.records());

  EXPECT_EQ(b.mutation_count(), std::uint64_t{2});
  EXPECT_EQ(b.order_count(), std::size_t{0});
}

TEST(Reset, DropsEveryInstrumentAndKeepsEveryCounter) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5);
  s.modify(9, Side::kBid, px(29000), 4).last();
  Book b = replay(s.records());

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

void expect_agreement(const std::vector<MboMsg>& records, Book& book, testing::ToyBook& toy) {
  for (const MboMsg& rec : records) {
    book.apply(rec);
    book.verify();
    toy.apply(rec);
    EXPECT_EQ(book.mutation_count(), toy.mutation_count());
    EXPECT_EQ(book.best_bid(kIid), toy.best_bid(kIid));
    EXPECT_EQ(book.best_ask(kIid), toy.best_ask(kIid));
  }
}

TEST(BookVsToy, TopOfBookAndCountAgreeThroughTheIcebergFixture) {
  Book book;
  testing::ToyBook toy;
  expect_agreement(testing::iceberg_then_market_moves_up(), book, toy);

  EXPECT_EQ(book.unknown_modifies(), std::uint64_t{0});
  EXPECT_EQ(book.duplicate_adds(), std::uint64_t{0});
}

TEST(BookVsToy, ARepeatedIdOrphansSizeInNeitherBook) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(1, Side::kBid, px(28999, 3), 5).last();

  Book book;
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
