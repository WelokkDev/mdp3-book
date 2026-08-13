
#include "bookreplay/dbn.hpp"
#include "bookreplay/invariants.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "toy_book.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::FillAsDeltaBook;
using testing::px;
using testing::StreamBuilder;
using testing::ToyBook;
using testing::TradeMutatesBook;

static_assert(BookLike<ToyBook>);
static_assert(BookLike<FillAsDeltaBook>);
static_assert(BookLike<TradeMutatesBook>);

constexpr std::uint64_t k0 = 0;
constexpr std::uint64_t k1 = 1;

TEST(CorrectBook, CleanTradeEventSatisfiesEveryInvariant) {
  ToyBook book;
  const auto report = run_checked(book, testing::clean_trade_event(), SessionState::kTrading);

  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.passive_mutations, k0);
  EXPECT_EQ(report.materializations, k0);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_EQ(report.malformed_records, k0);
  EXPECT_TRUE(report.violations.empty());

  // 5 mutating in the snapshot (R + 4 adds) + 1 cancel; T and F mutate nothing.
  EXPECT_EQ(report.mutating_records, std::uint64_t{6});
  EXPECT_EQ(report.passive_records, std::uint64_t{2});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{6});

  // The F here names a resting order that really is in the book, so it is
  // neither a materialization nor an iceberg.
  EXPECT_EQ(report.unknown_order_fills, k0);
}

TEST(CorrectBook, IcebergFillIsCountedAsDataNotFailure) {
  ToyBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.unknown_order_fills, k1);
  EXPECT_EQ(report.materializations, k0);
  EXPECT_EQ(report.mutating_records, std::uint64_t{9});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{9});
}

TEST(FillAsDelta, TripsMutationReconciliation) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_FALSE(report.ok());
  EXPECT_FALSE(report.reconciles());
  EXPECT_EQ(report.mutating_records, std::uint64_t{9});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{10});  // the F
  EXPECT_EQ(report.passive_mutations, k1);
}

TEST(FillAsDelta, TripsMaterializationOneLayerEarlier) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_EQ(report.materializations, k1);
  EXPECT_EQ(report.unknown_order_fills, k0);  // it got materialized instead
}

TEST(FillAsDelta, CrossesTheBookOnceTheMarketWalksAway) {
  FillAsDeltaBook broken;
  const auto bad =
      run_checked(broken, testing::iceberg_then_market_moves_up(), SessionState::kTrading);
  EXPECT_GE(bad.cross_violations, k1);

  // Same stream, correct book: the phantom never exists, so nothing crosses.
  ToyBook good;
  const auto fine =
      run_checked(good, testing::iceberg_then_market_moves_up(), SessionState::kTrading);
  EXPECT_EQ(fine.cross_violations, k0);
  EXPECT_GE(fine.cross_checks, k1);  // and the check really did run
}

TEST(FillAsDelta, TheCrossedBookLooksLikeTheReportedBug) {
  FillAsDeltaBook book;
  const auto records = testing::iceberg_then_market_moves_up();
  run_checked(book, records, SessionState::kTrading);

  const std::uint32_t iid = records.front().instrument_id;
  // A stale phantom ask stranded below the live bid — the shape of #4445.
  EXPECT_GT(book.best_bid(iid), book.best_ask(iid));
  EXPECT_EQ(book.best_ask(iid), px(29000, 1));
  EXPECT_EQ(book.best_bid(iid), px(29005, 0));
}

TEST(TradeMutates, IsCaughtByReconciliationButNotByMaterialization) {
  TradeMutatesBook book;
  const auto report = run_checked(book, testing::clean_trade_event(), SessionState::kTrading);

  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.passive_mutations, k1);
  // A trade carries no resting order id, so invariant 3 has nothing to see.
  // This is exactly why the spec keeps both checks rather than one.
  EXPECT_EQ(report.materializations, k0);
}

TEST(CrossedBook, IsNormalOutsideTradingAndMustNotBeFlagged) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kPreOpen);

  // CME accepts orders without matching until the opening uncross, so a
  // crossed book here is not evidence of anything.
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_EQ(report.cross_checks, k0);
  EXPECT_EQ(report.boundaries, k0);

  // The other two invariants are not session-gated and still fire.
  EXPECT_EQ(report.passive_mutations, k1);
  EXPECT_EQ(report.materializations, k1);
}

TEST(CrossedBook, MidEventCrossingIsNotAViolation) {
  // Both CME and Databento state the book is undefined mid-event: "the
  // apparent best bid and offer may have already been traded."
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5);            // crossed, mid-event
  b.cancel(500, Side::kAsk, px(28999, 0), 5).last();  // resolved by the boundary

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_TRUE(report.ok());
}

TEST(CrossedBook, CrossingThatSurvivesToTheBoundaryIsAViolation) {
  // The positive control for the test above: same records, but the crossing
  // one carries F_LAST, so the book really is crossed at an event boundary.
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.cross_violations, k1);
  ASSERT_FALSE(report.violations.empty());
  EXPECT_EQ(report.violations.front().invariant, Invariant::kUncrossedBook);
  EXPECT_EQ(report.violations.front().best_bid, px(29000, 0));
  EXPECT_EQ(report.violations.front().best_ask, px(28999, 0));
}

TEST(CrossedBook, PeriodZeroDisablesTheCheckEntirely) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  InvariantHarness<ToyBook>::Options opts;
  opts.cross_check_period = 0;
  const auto report = run_checked(book, b.records(), SessionState::kTrading, opts);

  EXPECT_EQ(report.cross_checks, k0);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_GE(report.boundaries, k1);  // boundaries still counted
}

TEST(CrossedBook, EmptySideIsNotACrossing) {
  StreamBuilder b{};
  b.clear();
  b.add(100, Side::kBid, px(29000, 0), 10).last();  // bids only

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.cross_checks, k0);  // kUndefPrice guarded before comparing
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_TRUE(report.ok());
}

TEST(WellFormedness, UnknownActionIsRejectedLoudly) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kBid, px(28998, 0), 1).corrupt_action('X').last();

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.malformed_records, k1);
  EXPECT_FALSE(report.ok());
  ASSERT_FALSE(report.violations.empty());
  EXPECT_EQ(report.violations.front().invariant, Invariant::kWellFormedRecord);
}

TEST(Reporting, FirstViolationCarriesItsPrecedingRecords) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  ASSERT_FALSE(report.violations.empty());
  ASSERT_FALSE(report.first_violation_context.empty());
  EXPECT_LE(report.first_violation_context.size(),
            InvariantHarness<FillAsDeltaBook>::kContextDepth);

  const MboMsg& culprit = report.first_violation_context.back();
  EXPECT_EQ(culprit.action, 'F');
  EXPECT_EQ(culprit.order_id, report.violations.front().order_id);
}

TEST(Reporting, ViolationListIsCapped) {
  // 40 crossing boundaries, cap of 3.
  StreamBuilder b = testing::opening_snapshot();
  for (std::uint64_t i = 0; i < 40; ++i) {
    b.add(500 + i, Side::kAsk, px(28999, 0), 1).last();
  }

  ToyBook book;
  InvariantHarness<ToyBook>::Options opts;
  opts.max_violations = 3;
  const auto report = run_checked(book, b.records(), SessionState::kTrading, opts);

  EXPECT_EQ(report.violations.size(), std::size_t{3});
  EXPECT_EQ(report.cross_violations, std::uint64_t{40});  // counted, not stored
}

TEST(Reporting, RecordCountMatchesTheStream) {
  const auto records = testing::iceberg_then_market_moves_up();
  ToyBook book;
  const auto report = run_checked(book, records, SessionState::kTrading);

  EXPECT_EQ(report.records, static_cast<std::uint64_t>(records.size()));
  EXPECT_EQ(report.mutating_records + report.passive_records, report.records);
}

}  // namespace
}  // namespace bookreplay
