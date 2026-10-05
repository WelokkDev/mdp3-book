#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/fast_book.hpp"
#include "bookreplay/invariants.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "book_types.hpp"
#include "toy_book.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::CompensatingCountBook;
using testing::FillAsDeleteBook;
using testing::FillAsDeltaBook;
using testing::px;
using testing::StreamBuilder;
using testing::ToyBook;
using testing::TradeMutatesBook;

static_assert(BookLike<Book>);
static_assert(BookLike<FastBook>);
static_assert(BookLike<ToyBook>);
static_assert(BookLike<FillAsDeltaBook>);
static_assert(BookLike<FillAsDeleteBook>);
static_assert(BookLike<CompensatingCountBook>);
static_assert(BookLike<TradeMutatesBook>);

constexpr std::uint64_t k0 = 0;
constexpr std::uint64_t k1 = 1;

/// A second id, so one instrument can be open while another is not.
constexpr std::uint32_t kSecondInstrument = 42013467;

/// `run_checked` pins one state for a whole stream. These tests need the
/// state to arrive partway through it, so they drive the harness themselves.
template <BookLike B>
void drive(InvariantHarness<B>& harness, B& book, std::span<const MboMsg> records) {
  for (const MboMsg& rec : records) {
    harness.before(rec);
    book.apply(rec);
    harness.after(rec);
  }
}

template <BookLike B>
void expect_clean_trade_event_is_clean(B& book) {
  const auto report = run_checked(book, testing::clean_trade_event(), SessionState::kTrading);

  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.passive_mutations, k0);
  EXPECT_EQ(report.materializations, k0);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_EQ(report.malformed_records, k0);
  EXPECT_TRUE(report.violations.empty());

  EXPECT_EQ(report.mutating_records, std::uint64_t{6});
  EXPECT_EQ(report.passive_records, std::uint64_t{2});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{6});
  EXPECT_EQ(report.unknown_order_fills, k0);
}

template <BookLike B>
void expect_iceberg_fill_is_data(B& book) {
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.unknown_order_fills, k1);
  EXPECT_EQ(report.materializations, k0);
  EXPECT_EQ(report.mutating_records, std::uint64_t{9});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{9});
}

TEST(CorrectBook, CleanTradeEventSatisfiesEveryInvariant) {
  ToyBook book;
  expect_clean_trade_event_is_clean(book);
}

TEST(CorrectBook, IcebergFillIsCountedAsDataNotFailure) {
  ToyBook book;
  expect_iceberg_fill_is_data(book);
}

// The same two fixtures, driven through the real books. ToyBook proves the
// harness; the books are what the harness exists for, and `run_checked` takes
// either with no change of its own.

TEST(CorrectBook, RealBookSatisfiesEveryInvariantOnTheCleanTradeEvent) {
  Book book;
  expect_clean_trade_event_is_clean(book);
  EXPECT_NO_THROW(book.verify());
}

TEST(CorrectBook, RealBookCountsTheIcebergFillAsDataNotFailure) {
  Book book;
  expect_iceberg_fill_is_data(book);
  EXPECT_NO_THROW(book.verify());
}

TEST(CorrectBook, FastBookSatisfiesEveryInvariantOnTheCleanTradeEvent) {
  FastBook book = testing::make_book<FastBook>();
  expect_clean_trade_event_is_clean(book);
  EXPECT_NO_THROW(book.verify());
}

TEST(CorrectBook, FastBookCountsTheIcebergFillAsDataNotFailure) {
  FastBook book = testing::make_book<FastBook>();
  expect_iceberg_fill_is_data(book);
  EXPECT_NO_THROW(book.verify());
}

TEST(FillAsDelta, TripsMutationReconciliation) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_FALSE(report.ok());
  EXPECT_FALSE(report.reconciles());
  EXPECT_EQ(report.mutating_records, std::uint64_t{9});
  EXPECT_EQ(report.observed_mutations, std::uint64_t{10});
  EXPECT_EQ(report.passive_mutations, k1);
}

TEST(FillAsDelta, TripsMaterializationOneLayerEarlier) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kTrading);

  EXPECT_EQ(report.materializations, k1);
  EXPECT_EQ(report.unknown_order_fills, k0);
}

TEST(FillAsDelta, CrossesTheBookOnceTheMarketWalksAway) {
  FillAsDeltaBook broken;
  const auto bad =
      run_checked(broken, testing::iceberg_then_market_moves_up(), SessionState::kTrading);
  EXPECT_GE(bad.cross_violations, k1);

  ToyBook good;
  const auto fine =
      run_checked(good, testing::iceberg_then_market_moves_up(), SessionState::kTrading);
  EXPECT_EQ(fine.cross_violations, k0);
  EXPECT_GE(fine.cross_checks, k1);
}

TEST(FillAsDelta, TheCrossedBookLooksLikeTheReportedBug) {
  FillAsDeltaBook book;
  const auto records = testing::iceberg_then_market_moves_up();
  run_checked(book, records, SessionState::kTrading);

  const std::uint32_t iid = records.front().hd.instrument_id;
  EXPECT_GT(book.best_bid(iid), book.best_ask(iid));
  EXPECT_EQ(book.best_ask(iid), px(29000, 1));
  EXPECT_EQ(book.best_bid(iid), px(29005, 0));
}

TEST(FillAsDelete, IsInvisibleToEveryAggregateAndCaughtByMembership) {
  FillAsDeleteBook book;
  const auto report = run_checked(book, testing::clean_trade_event(), SessionState::kTrading);

  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.passive_mutations, k0);
  EXPECT_EQ(report.mutation_miscounts, k0);
  EXPECT_EQ(report.materializations, k0);

  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.dematerializations, k1);
  ASSERT_FALSE(report.violations.empty());
  EXPECT_EQ(report.violations.front().invariant, Invariant::kNoMaterialization);
  EXPECT_EQ(report.violations.front().action, 'F');
  EXPECT_EQ(report.violations.front().order_id, 200U);
}

TEST(FillAsDelete, APartialFillDestroysQueuePositionWhileAggregatesSelfHeal) {
  StreamBuilder b = testing::opening_snapshot();
  b.trade(Side::kBid, px(29000, 1), 4);
  b.fill(200, Side::kAsk, px(29000, 1), 4);
  b.modify(200, Side::kAsk, px(29000, 1), 6).last();

  FillAsDeleteBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);

  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.passive_mutations, k0);
  EXPECT_EQ(report.materializations, k0);
  EXPECT_EQ(report.cross_violations, k0);

  EXPECT_EQ(report.dematerializations, k1);
  EXPECT_FALSE(report.ok());
}

TEST(CompensatingCounts, BalanceInAggregateAndAreCaughtPerRecord) {
  StreamBuilder b = testing::opening_snapshot();
  b.modify(100, Side::kBid, px(29000, 0), 5);
  b.cancel(999, Side::kBid, px(29000, 0), 5).last();

  CompensatingCountBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);

  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.mutation_miscounts, std::uint64_t{2});
  EXPECT_FALSE(report.ok());
  ASSERT_EQ(report.violations.size(), 2U);
  EXPECT_EQ(report.violations.front().invariant, Invariant::kMutationReconciliation);
  EXPECT_EQ(report.violations.front().action, 'M');
  EXPECT_EQ(report.violations.front().order_id, 100U);
  EXPECT_FALSE(report.first_violation_context.empty());
}

TEST(TradeMutates, IsCaughtByReconciliationButNotByMaterialization) {
  TradeMutatesBook book;
  const auto report = run_checked(book, testing::clean_trade_event(), SessionState::kTrading);

  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.passive_mutations, k1);
  // A trade carries no resting order id, so membership has nothing to probe.
  EXPECT_EQ(report.materializations, k0);
}

TEST(CrossedBook, IsNormalOutsideTradingAndMustNotBeFlagged) {
  FillAsDeltaBook book;
  const auto report =
      run_checked(book, testing::iceberg_then_market_moves_up(), SessionState::kPreOpen);

  // CME accepts orders without matching until the opening uncross.
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_EQ(report.cross_checks, k0);
  EXPECT_EQ(report.boundaries, k0);
  EXPECT_EQ(report.boundaries_outside_trading, std::uint64_t{4});

  EXPECT_EQ(report.passive_mutations, k1);
  EXPECT_EQ(report.materializations, k1);
}

TEST(CrossedBook, MidEventCrossingIsNotAViolation) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5);
  b.cancel(500, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_TRUE(report.ok());
}

TEST(CrossedBook, CrossingThatSurvivesToTheBoundaryIsAViolation) {
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
  EXPECT_GE(report.boundaries, k1);
}

TEST(CrossedBook, EmptySideIsNotACrossing) {
  StreamBuilder b{};
  b.clear();
  b.add(100, Side::kBid, px(29000, 0), 10).last();

  ToyBook book;
  const auto report = run_checked(book, b.records(), SessionState::kTrading);
  EXPECT_EQ(report.cross_checks, k0);
  EXPECT_EQ(report.cross_violations, k0);
  EXPECT_TRUE(report.ok());
}

TEST(SessionMapping, IsTradingDecidesAndTheActionOnlyNamesTheRest) {
  struct Case {
    std::uint16_t action;
    char is_trading;
    SessionState expected;
  };

  // The first four are every shape five real NQ days produced; the rest are
  // what the enum allows and another day could.
  constexpr Case kCases[] = {
      {kStatusActionTrading, kTriStateYes, SessionState::kTrading},
      {kStatusActionNewPriceIndication, kTriStateYes, SessionState::kTrading},
      {kStatusActionPreOpen, kTriStateNo, SessionState::kPreOpen},
      {kStatusActionClose, kTriStateNo, SessionState::kClosed},
      {kStatusActionHalt, kTriStateNo, SessionState::kHalted},
      {kStatusActionPause, kTriStateNo, SessionState::kHalted},
      {kStatusActionSuspend, kTriStateNo, SessionState::kHalted},
      {kStatusActionNotAvailableForTrading, kTriStateNo, SessionState::kClosed},
      {kStatusActionClose, kTriStateNotAvailable, SessionState::kClosed},
      {kStatusActionTrading, kTriStateNo, SessionState::kUnknown},
      {kStatusActionTrading, kTriStateNotAvailable, SessionState::kUnknown},
      {0, kTriStateNo, SessionState::kUnknown},
  };

  for (const Case& c : kCases) {
    EXPECT_EQ(session_state_of(testing::status(42004177, c.action, c.is_trading)), c.expected)
        << "action " << c.action << ", is_trading " << c.is_trading;
  }
}

TEST(SessionGate, AnInstrumentNoStatusRecordDescribedIsNotChecked) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  InvariantHarness<ToyBook> harness(book);
  drive(harness, book, b.records());

  EXPECT_EQ(harness.session_state(b.instrument_id()), SessionState::kUnknown);
  EXPECT_EQ(harness.report().boundaries, k0);
  EXPECT_EQ(harness.report().cross_checks, k0);
  EXPECT_EQ(harness.report().boundaries_outside_trading, std::uint64_t{2});
  EXPECT_TRUE(harness.ok());
}

TEST(SessionGate, TheOpenTurnsTheCheckOnMidStream) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();
  b.add(501, Side::kAsk, px(28999, 0), 5).last();
  const std::span<const MboMsg> records{b.records()};

  ToyBook book;
  InvariantHarness<ToyBook> harness(book);
  harness.observe(testing::status(b.instrument_id(), kStatusActionPreOpen, kTriStateNo));
  drive(harness, book, records.first(records.size() - 1));

  EXPECT_EQ(harness.report().cross_violations, k0);
  EXPECT_EQ(harness.report().boundaries_outside_trading, std::uint64_t{2});

  harness.observe(testing::status(b.instrument_id(), kStatusActionTrading, kTriStateYes));
  drive(harness, book, records.last(1));

  EXPECT_EQ(harness.report().boundaries, k1);
  EXPECT_EQ(harness.report().cross_violations, k1);
  EXPECT_EQ(harness.report().status_records, std::uint64_t{2});
  ASSERT_FALSE(harness.report().violations.empty());
  EXPECT_EQ(harness.report().violations.front().invariant, Invariant::kUncrossedBook);
}

TEST(SessionGate, AHaltTurnsTheCheckBackOff) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();
  b.add(501, Side::kAsk, px(28999, 0), 5).last();
  const std::span<const MboMsg> records{b.records()};

  ToyBook book;
  InvariantHarness<ToyBook> harness(book);
  harness.observe(testing::status(b.instrument_id(), kStatusActionTrading, kTriStateYes));
  drive(harness, book, records.first(records.size() - 1));

  EXPECT_EQ(harness.report().cross_violations, k1);
  EXPECT_EQ(harness.report().boundaries_outside_trading, k0);

  // The venue's shape for a mid-session halt: a pre-open that is not trading,
  // arriving on an instrument that was.
  harness.observe(testing::status(b.instrument_id(), kStatusActionPreOpen, kTriStateNo));
  drive(harness, book, records.last(1));

  EXPECT_EQ(harness.report().cross_violations, k1);
  EXPECT_EQ(harness.report().boundaries_outside_trading, k1);
}

TEST(SessionGate, AStatusRecordTheMappingCannotPlaceStopsTheCheck) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  InvariantHarness<ToyBook> harness(book);
  harness.observe(testing::status(b.instrument_id(), kStatusActionTrading, kTriStateYes));
  harness.observe(testing::status(b.instrument_id(), kStatusActionTrading, kTriStateNotAvailable));
  drive(harness, book, b.records());

  EXPECT_EQ(harness.report().status_records, std::uint64_t{2});
  EXPECT_EQ(harness.report().unmapped_status_records, k1);
  EXPECT_EQ(harness.report().boundaries, k0);
  EXPECT_EQ(harness.report().boundaries_outside_trading, std::uint64_t{2});
  EXPECT_EQ(harness.report().cross_violations, k0);
}

TEST(SessionGate, StateIsPerInstrument) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();
  const std::uint32_t open_instrument = b.instrument_id();

  b.instrument(kSecondInstrument);
  b.add(600, Side::kBid, px(29000, 0), 10);
  b.add(601, Side::kAsk, px(28999, 0), 5).last();

  ToyBook book;
  InvariantHarness<ToyBook> harness(book);
  harness.observe(testing::status(open_instrument, kStatusActionTrading, kTriStateYes));
  harness.observe(testing::status(kSecondInstrument, kStatusActionPreOpen, kTriStateNo));
  drive(harness, book, b.records());

  EXPECT_EQ(harness.report().cross_violations, k1);
  EXPECT_EQ(harness.report().boundaries_outside_trading, k1);
  ASSERT_FALSE(harness.report().violations.empty());
  EXPECT_EQ(harness.report().violations.front().instrument_id, open_instrument);
}

TEST(SessionGate, AStatusRecordOverridesThePinnedDefault) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(500, Side::kAsk, px(28999, 0), 5).last();

  const auto crossings_seen = [&b](SessionState pinned, const StatusMsg& observed) {
    ToyBook book;
    InvariantHarness<ToyBook> harness(book);
    harness.set_session_state(pinned);
    harness.observe(observed);
    drive(harness, book, b.records());
    return harness.report().cross_violations;
  };

  const StatusMsg closed =
      testing::status(b.instrument_id(), kStatusActionClose, kTriStateNo, kTriStateNo);
  const StatusMsg trading = testing::status(b.instrument_id(), kStatusActionTrading, kTriStateYes);

  EXPECT_EQ(crossings_seen(SessionState::kTrading, closed), k0);
  EXPECT_EQ(crossings_seen(SessionState::kUnknown, trading), k1);
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
  StreamBuilder b = testing::opening_snapshot();
  for (std::uint64_t i = 0; i < 40; ++i) {
    b.add(500 + i, Side::kAsk, px(28999, 0), 1).last();
  }

  ToyBook book;
  InvariantHarness<ToyBook>::Options opts;
  opts.max_violations = 3;
  const auto report = run_checked(book, b.records(), SessionState::kTrading, opts);

  EXPECT_EQ(report.violations.size(), std::size_t{3});
  EXPECT_EQ(report.cross_violations, std::uint64_t{40});
}

TEST(Reporting, AZeroCapStillKeepsTheFirstViolationsContext) {
  StreamBuilder b = testing::opening_snapshot();
  for (std::uint64_t i = 0; i < 40; ++i) {
    b.add(500 + i, Side::kAsk, px(28999, 0), 1).last();
  }

  ToyBook book;
  InvariantHarness<ToyBook>::Options opts;
  opts.max_violations = 0;
  const auto report = run_checked(book, b.records(), SessionState::kTrading, opts);

  EXPECT_TRUE(report.violations.empty());
  EXPECT_EQ(report.cross_violations, std::uint64_t{40});
  ASSERT_FALSE(report.first_violation_context.empty());
  EXPECT_EQ(report.first_violation_context.back().order_id, std::uint64_t{500});
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
