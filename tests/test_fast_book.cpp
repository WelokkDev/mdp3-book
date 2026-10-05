#include "bookreplay/book.hpp"
#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/depth.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "book_types.hpp"
#include "synthetic_stream.hpp"
#include "toy_stream.hpp"

namespace bookreplay {

/// Reaches inside the book to break one thing, so `verify()` can be shown to
/// notice. Named as a friend in the book's header and defined nowhere else.
struct FastBookProbe {
  static std::uint32_t slot(const FastBook& book, std::uint32_t instrument_id,
                            std::uint64_t order_id) {
    return book.ids_.find(book.find_instrument(instrument_id), order_id);
  }

  static FastBook::Node& node(FastBook& book, std::uint32_t instrument_id, std::uint64_t order_id) {
    return book.nodes_[slot(book, instrument_id, order_id)];
  }

  static void point_back_link_at_itself(FastBook& book, std::uint32_t instrument_id,
                                        std::uint64_t order_id) {
    FastBook::Node& n = node(book, instrument_id, order_id);
    n.prev = static_cast<std::uint32_t>(&n - book.nodes_.data());
  }

  static void add_to_level_total(FastBook& book, std::uint32_t instrument_id,
                                 std::uint64_t order_id) {
    const FastBook::Node& n = node(book, instrument_id, order_id);
    ++book.pages_[n.page]->levels[n.offset].total;
  }

  static void clear_occupancy_bit(FastBook& book, std::uint32_t instrument_id,
                                  std::uint64_t order_id) {
    const FastBook::Node& n = node(book, instrument_id, order_id);
    book.pages_[n.page]->words[n.offset >> 6] &= ~(std::uint64_t{1} << (n.offset & 63));
  }
};

namespace {

using testing::make_book;
using testing::px;
using testing::StreamBuilder;

constexpr std::uint32_t kIid = 42004177;
constexpr std::uint32_t kUnregistered = 42999999;

void apply_all(FastBook& book, std::span<const MboMsg> records) {
  for (const MboMsg& rec : records) {
    book.apply(rec);
  }
}

TEST(FastBookPages, APriceAMillionTicksAwayOpensOnePageAndIsCounted) {
  StreamBuilder s;
  s.add(1, Side::kAsk, px(29000, 1), 5).last();
  s.add(2, Side::kAsk, px(29000, 1'000'000), 3).last();
  FastBook book = make_book<FastBook>();
  apply_all(book, std::span<const MboMsg>{s.records()}.first(1));
  ASSERT_EQ(book.growth().pages, 1U);

  apply_all(book, std::span<const MboMsg>{s.records()}.subspan(1));

  EXPECT_EQ(book.growth().pages, 2U);
  EXPECT_EQ(book.best_ask(kIid), px(29000, 1));
  const std::vector<LevelView> asks = ladder_view(book, kIid, Side::kAsk);
  ASSERT_EQ(asks.size(), 2U);
  EXPECT_EQ(asks[1].price, px(29000, 1'000'000));
  EXPECT_NO_THROW(book.verify());
}

TEST(FastBookPages, TheTouchWalksAcrossEmptyPagesWhenItCloses) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).last();
  s.add(2, Side::kBid, px(29000, -5000), 5).last();
  s.add(3, Side::kBid, px(29000, -9000), 5).last();
  s.cancel(2, Side::kBid, px(29000, -5000), 5).last();
  s.cancel(1, Side::kBid, px(29000), 5).last();
  FastBook book = make_book<FastBook>();
  apply_all(book, s.records());

  EXPECT_EQ(book.best_bid(kIid), px(29000, -9000));
  EXPECT_NO_THROW(book.verify());
}

// The walk behind a top ten skips what is better than the touch, and only in
// the bitmap word and on the page the touch is in. Here the tenth bid sits in
// the next word at a lower bit than the best, and the far ask on the next page
// at a lower offset than the near one, so a walk that carried the skip a step
// too far would lose both.
TEST(FastBookPages, ATopTenCrossesABitmapWordAndAPage) {
  StreamBuilder s;
  for (std::int64_t i = 0; i < 10; ++i) {
    s.add(100 + static_cast<std::uint64_t>(i), Side::kBid, px(29000, -30 - i), 10);
  }
  s.add(200, Side::kAsk, px(29000, 1), 4);
  s.add(201, Side::kAsk, px(29000, 801), 6).last();
  FastBook book = make_book<FastBook>();
  apply_all(book, s.records());
  const auto& best_bid = FastBookProbe::node(book, kIid, 100);
  const auto& tenth_bid = FastBookProbe::node(book, kIid, 109);
  ASSERT_GT(tenth_bid.offset >> 6, best_bid.offset >> 6) << "the bids share a word";
  ASSERT_LT(tenth_bid.offset & 63, best_bid.offset & 63);
  const auto& near_ask = FastBookProbe::node(book, kIid, 200);
  const auto& far_ask = FastBookProbe::node(book, kIid, 201);
  ASSERT_NE(far_ask.page, near_ask.page) << "the asks share a page";
  ASSERT_LT(far_ask.offset, near_ask.offset);

  const Depth10 top = top_ten(book, kIid);

  for (std::size_t i = 0; i < kDepthLevels; ++i) {
    EXPECT_EQ(top[i].bid_px, px(29000, -30 - static_cast<std::int64_t>(i))) << "level " << i;
    EXPECT_EQ(top[i].bid_sz, 10U) << "level " << i;
  }
  EXPECT_EQ(top[0].ask_px, px(29000, 1));
  EXPECT_EQ(top[1].ask_px, px(29000, 801));
  EXPECT_EQ(top[1].ask_sz, 6U);
  EXPECT_EQ(top[2].ask_px, kUndefPrice);
}

TEST(FastBookGrid, APriceOffItsInstrumentsTickIsRefused) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000) + 1, 5).last();
  FastBook book = make_book<FastBook>();

  EXPECT_THROW(book.apply(s.records().front()), TickGridError);
}

TEST(FastBookGrid, AModifyOffTheGridIsRefusedToo) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).last();
  s.modify(1, Side::kBid, px(29000) + testing::kSpreadTick, 5).last();
  FastBook book = make_book<FastBook>();
  book.apply(s.records()[0]);

  EXPECT_THROW(book.apply(s.records()[1]), TickGridError);
}

TEST(FastBookGrid, BothExtremePricesAreRefusedEvenWhereTheyDivideTheTick) {
  StreamBuilder s;
  s.add(1, Side::kAsk, kUndefPrice, 5).last();
  s.add(2, Side::kBid, std::numeric_limits<std::int64_t>::min(), 5).last();
  FastBook book;
  book.set_tick_size(kIid, 1);

  EXPECT_THROW(book.apply(s.records()[0]), TickGridError);
  EXPECT_THROW(book.apply(s.records()[1]), TickGridError);
  EXPECT_EQ(book.order_count(), 0U);
}

TEST(FastBookGrid, AnInstrumentWithNoTickCannotPlaceAnOrder) {
  StreamBuilder s{kUnregistered};
  s.add(1, Side::kBid, px(29000), 5).last();
  FastBook book = make_book<FastBook>();

  EXPECT_THROW(book.apply(s.records().front()), TickGridError);
}

// The reference ignores these without looking at the price, so the fast book
// must too, or the two stop agreeing on a stream the reference accepts.
TEST(FastBookGrid, WhatPlacesNothingIsNeverJudged) {
  StreamBuilder s{kUnregistered};
  s.add(1, Side::kNone, px(29000) + 1, 5);
  s.add(0, Side::kBid, px(29000) + 1, 5);
  s.cancel(7, Side::kBid, px(29000) + 1, 5);
  s.fill(7, Side::kBid, px(29000) + 1, 5);
  s.trade(Side::kAsk, kUndefPrice, 5).clear().none();
  FastBook book = make_book<FastBook>();

  EXPECT_NO_THROW(apply_all(book, s.records()));
  EXPECT_EQ(book.mutation_count(), 4U);
  EXPECT_EQ(book.instrument_count(), 0U);
}

TEST(FastBookGrid, ACancelErasesByIdWhateverPriceItCarries) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).last();
  s.cancel(1, Side::kBid, px(29000) + 1, 5).last();
  FastBook book = make_book<FastBook>();
  apply_all(book, s.records());

  EXPECT_FALSE(book.contains(kIid, 1));
  EXPECT_EQ(book.best_bid(kIid), kUndefPrice);
}

TEST(FastBookGrid, ATickCannotBeChangedOnceSet) {
  FastBook book;
  book.set_tick_size(kIid, testing::kTick);

  EXPECT_NO_THROW(book.set_tick_size(kIid, testing::kTick));
  EXPECT_THROW(book.set_tick_size(kIid, testing::kSpreadTick), TickGridError);
  EXPECT_THROW(book.set_tick_size(kUnregistered, 0), TickGridError);
  EXPECT_THROW(book.set_tick_size(kUnregistered, -testing::kTick), TickGridError);
}

TEST(FastBookGrid, ARefusedRecordLeavesTheBookAsItWas) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).last();
  s.add(2, Side::kBid, px(29000) + 1, 5).last();
  s.modify(1, Side::kBid, px(29000) + 1, 5).last();
  FastBook book = make_book<FastBook>();
  book.apply(s.records()[0]);

  EXPECT_THROW(book.apply(s.records()[1]), TickGridError);
  EXPECT_THROW(book.apply(s.records()[2]), TickGridError);
  EXPECT_EQ(book.mutation_count(), 1U);
  EXPECT_EQ(book.order_count(), 1U);
  EXPECT_EQ(queue_view(book, kIid, Side::kBid, px(29000)), (std::vector<std::uint64_t>{1}));
  EXPECT_NO_THROW(book.verify());
}

TEST(FastBookMove, TheSourceIsLeftAnEmptyBookThatStillWorks) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kAsk, px(29000, 1), 4).last();
  FastBook source = make_book<FastBook>();
  apply_all(source, s.records());

  FastBook moved = std::move(source);
  EXPECT_EQ(moved.order_count(), 2U);
  EXPECT_EQ(moved.best_bid(kIid), px(29000));
  EXPECT_NO_THROW(moved.verify());

  EXPECT_EQ(source.order_count(), 0U);
  EXPECT_EQ(source.mutation_count(), 0U);
  EXPECT_FALSE(source.contains(kIid, 1));
  EXPECT_THROW(source.apply(s.records().front()), TickGridError);
  source.set_tick_size(kIid, testing::kTick);
  apply_all(source, s.records());
  EXPECT_EQ(source.order_count(), 2U);
  EXPECT_NO_THROW(source.verify());

  source = std::move(moved);
  EXPECT_EQ(source.order_count(), 2U);
  EXPECT_EQ(moved.order_count(), 0U);
  EXPECT_NO_THROW(source.verify());
  EXPECT_NO_THROW(moved.verify());
}

TEST(FastBookClear, ResetsTheTouchAndFreesEverySlot) {
  StreamBuilder s;
  for (std::uint64_t id = 1; id <= 50; ++id) {
    const auto away = static_cast<std::int64_t>(id % 10);
    s.add(id, Side::kBid, px(29000, -away), 2);
    s.add(1000 + id, Side::kAsk, px(29000, 1 + away), 2);
  }
  s.last();
  StreamBuilder cleared;
  cleared.clear();

  FastBook book = make_book<FastBook>();
  apply_all(book, s.records());
  const FastBook::Growth grown = book.growth();
  apply_all(book, cleared.records());

  EXPECT_EQ(book.best_bid(kIid), kUndefPrice);
  EXPECT_EQ(book.best_ask(kIid), kUndefPrice);
  EXPECT_EQ(book.order_count(), 0U);
  EXPECT_TRUE(book.instruments().empty());
  EXPECT_NO_THROW(book.verify());

  // The freed slots and pages take the same orders again without growing.
  apply_all(book, s.records());
  EXPECT_EQ(book.growth().total(), grown.total());
  EXPECT_EQ(book.best_bid(kIid), px(29000));
  EXPECT_NO_THROW(book.verify());
}

TEST(FastBookRequeue, QueueAheadFollowsAnOrderAcrossLevelsAndBack) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
  s.add(3, Side::kBid, px(28999, 3), 4).last();
  FastBook book = make_book<FastBook>();
  apply_all(book, s.records());
  ASSERT_EQ(book.queue_ahead(kIid, 1), 0U);

  StreamBuilder away;
  away.modify(1, Side::kBid, px(28999, 3), 5).last();
  apply_all(book, away.records());
  EXPECT_EQ(book.queue_ahead(kIid, 1), 4U);
  EXPECT_EQ(book.queue_ahead(kIid, 2), 0U);

  StreamBuilder back;
  back.modify(1, Side::kBid, px(29000), 5).last();
  apply_all(book, back.records());
  EXPECT_EQ(book.queue_ahead(kIid, 1), 7U);
  EXPECT_EQ(queue_view(book, kIid, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 1}));
  EXPECT_EQ(book.order_count(), 3U);
  EXPECT_NO_THROW(book.verify());
}

// A fill is forgotten at the event boundary by looking its order up, and an
// order cancelled inside that event is no longer there to find. Its slot goes
// back on the free list with the fill still written in it.
TEST(FastBookSlots, AReusedSlotCarriesNoFillFromTheOrderBeforeIt) {
  StreamBuilder s;
  s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7).last();
  s.trade(Side::kAsk, px(29000), 5).fill(1, Side::kBid, px(29000), 5);
  s.cancel(1, Side::kBid, px(29000), 5).last();
  s.add(3, Side::kBid, px(29000), 5).add(4, Side::kBid, px(29000), 1).last();
  s.modify(3, Side::kBid, px(29000), 4).last();
  const std::span<const MboMsg> records{s.records()};
  FastBook book = make_book<FastBook>();
  apply_all(book, records.first(2));
  const std::uint32_t freed = FastBookProbe::slot(book, kIid, 1);
  apply_all(book, records.subspan(2, 5));
  ASSERT_EQ(FastBookProbe::slot(book, kIid, 3), freed) << "order 3 took a slot of its own";

  book.apply(records.back());

  // Judged against order 1's fill of 5, a 4 is not what is left of order 3's 5
  // and would send it behind order 4.
  EXPECT_EQ(queue_view(book, kIid, Side::kBid, px(29000)), (std::vector<std::uint64_t>{2, 3, 4}));
  EXPECT_NO_THROW(book.verify());
}

void expect_same_after(const Book& reference, const FastBook& fast, const MboMsg& rec) {
  const std::uint32_t iid = rec.hd.instrument_id;
  EXPECT_EQ(fast.mutation_count(), reference.mutation_count());
  EXPECT_EQ(fast.unknown_modifies(), reference.unknown_modifies());
  EXPECT_EQ(fast.duplicate_adds(), reference.duplicate_adds());
  EXPECT_EQ(fast.order_count(), reference.order_count());
  EXPECT_EQ(fast.instrument_count(), reference.instrument_count());
  EXPECT_EQ(fast.best_bid(iid), reference.best_bid(iid));
  EXPECT_EQ(fast.best_ask(iid), reference.best_ask(iid));
  EXPECT_EQ(fast.queue_ahead(iid, rec.order_id), reference.queue_ahead(iid, rec.order_id));
  const std::optional<RestingView> ours = resting_view(fast, iid, rec.order_id);
  const std::optional<RestingView> theirs = resting_view(reference, iid, rec.order_id);
  ASSERT_EQ(ours.has_value(), theirs.has_value());
  if (ours) {
    EXPECT_EQ(ours->side, theirs->side);
    EXPECT_EQ(ours->price, theirs->price);
    EXPECT_EQ(ours->size, theirs->size);
    EXPECT_EQ(ours->filled, theirs->filled);
  }
  if (is_event_boundary(rec)) {
    const std::optional<DepthMismatch> mismatch =
        first_mismatch(top_ten(fast, iid), top_ten(reference, iid));
    EXPECT_FALSE(mismatch) << "top ten level " << mismatch->level << ' ' << mismatch->field;
  }
}

void expect_same_books(const Book& reference, const FastBook& fast) {
  ASSERT_EQ(fast.instruments(), reference.instruments());
  for (const std::uint32_t iid : reference.instruments()) {
    for (const Side side : {Side::kBid, Side::kAsk}) {
      const std::vector<LevelView> ours = ladder_view(fast, iid, side);
      const std::vector<LevelView> theirs = ladder_view(reference, iid, side);
      ASSERT_EQ(ours.size(), theirs.size());
      for (std::size_t i = 0; i < theirs.size(); ++i) {
        ASSERT_EQ(ours[i].price, theirs[i].price);
        EXPECT_EQ(ours[i].total, theirs[i].total);
        EXPECT_EQ(ours[i].count, theirs[i].count);
        EXPECT_EQ(queue_view(fast, iid, side, theirs[i].price),
                  queue_view(reference, iid, side, theirs[i].price));
      }
    }
  }
  EXPECT_NO_THROW(fast.verify());
  EXPECT_NO_THROW(reference.verify());
}

TEST(FastBookVsReference, TheTwoAgreeRecordForRecordOnARoughStream) {
  const std::vector<MboMsg> records = testing::rough_stream(50'000, testing::kTestInstruments);
  Book reference;
  FastBook fast = make_book<FastBook>();

  for (std::size_t i = 0; i < records.size(); ++i) {
    reference.apply(records[i]);
    fast.apply(records[i]);
    expect_same_after(reference, fast, records[i]);
    if ((i + 1) % 5'000 == 0 || i + 1 == records.size()) {
      expect_same_books(reference, fast);
    }
    ASSERT_FALSE(HasFailure()) << "the books first differ at record " << i;
  }

  EXPECT_GT(reference.unknown_modifies(), 0U);
  EXPECT_GT(reference.duplicate_adds(), 0U);
  EXPECT_GT(fast.growth().pages, 2 * testing::kTestInstruments.size())
      << "no side left its first page, so the walk across pages went unchecked";
}

class FastBookVerify : public ::testing::Test {
 protected:
  void SetUp() override {
    StreamBuilder s;
    s.add(1, Side::kBid, px(29000), 5).add(2, Side::kBid, px(29000), 7);
    s.add(3, Side::kAsk, px(29000, 1), 4).add(4, Side::kAsk, px(29000, 2), 4).last();
    book_ = make_book<FastBook>();
    apply_all(book_, s.records());
    ASSERT_NO_THROW(book_.verify());
  }

  FastBook book_;
};

TEST_F(FastBookVerify, ABrokenBackLinkIsFound) {
  FastBookProbe::point_back_link_at_itself(book_, kIid, 2);
  EXPECT_THROW(book_.verify(), BookError);
}

TEST_F(FastBookVerify, ALevelTotalOutOfStepWithItsQueueIsFound) {
  FastBookProbe::add_to_level_total(book_, kIid, 1);
  EXPECT_THROW(book_.verify(), BookError);
}

// Order 3 rests one tick away in the same bitmap word, so the word stays
// non-zero and the page's summary stays true: only the check of each level
// against its own bit can notice.
TEST_F(FastBookVerify, AnOccupiedLevelMissingFromTheBitmapIsFound) {
  FastBookProbe::clear_occupancy_bit(book_, kIid, 4);
  EXPECT_THROW(book_.verify(), BookError);
}

}  // namespace
}  // namespace bookreplay
