#include "bookreplay/naked_window.hpp"
#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/trade_source.hpp"

#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "toy_stream.hpp"
#include "toy_trades.hpp"

namespace bookreplay {
namespace {

using testing::kTick;
using testing::px;
using testing::TradeStreamBuilder;

constexpr std::int64_t kT0 = 1'785'888'000'000'000'000LL;
constexpr std::int64_t kMs = 1'000'000LL;

TradeStreamBuilder falling() {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 2);
  b.trade(Side::kAsk, px(28999, 3), 3);
  b.trade(Side::kAsk, px(28999, 2), 4);
  return b;
}

NakedWindowQuery long_from(std::int64_t window_ns) {
  NakedWindowQuery q;
  q.entry_fill_ts = kT0 + kMs;
  q.window_ns = window_ns;
  q.entry_price = px(29000, 0);
  q.stop_price = px(28999, 2);
  q.position_side = Side::kBid;
  return q;
}

std::vector<NakedWindowResult> scan_over(const TradeStreamBuilder& builder,
                                         const std::vector<NakedWindowQuery>& queries,
                                         std::size_t max_concurrent = 64) {
  RecordTradeSource source{builder.records()};
  NakedWindowScan scan{TickScale{kTick}, max_concurrent};
  return scan.run(source, queries);
}

TEST(NakedWindow, ReportsAStopReachedInsideTheWindowAndTheInstantItWasReached) {
  const auto results = scan_over(falling(), {long_from(3 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_TRUE(results[0].stop_reached);
  EXPECT_EQ(results[0].first_reach_ts, kT0 + 3 * kMs);
  EXPECT_EQ(results[0].ticks, std::uint64_t{3});
  EXPECT_EQ(results[0].volume, std::uint64_t{9});
}

TEST(NakedWindow, NamesTheFirstPrintThroughTheStopWhenSeveralReachIt) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 2);
  b.trade(Side::kAsk, px(28999, 2), 3);
  b.trade(Side::kAsk, px(28999, 0), 4);

  const auto results = scan_over(b, {long_from(4 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_TRUE(results[0].stop_reached);
  EXPECT_EQ(results[0].first_reach_ts, kT0 + 2 * kMs);
}

TEST(NakedWindow, IgnoresAPrintFromBeforeThePositionExisted) {
  NakedWindowAccumulator acc{TickScale{kTick}, long_from(5 * kMs)};

  Tick early{};
  early.ts = kT0 + kMs - 1;
  early.price = px(28999, 0);
  early.size = 5;
  early.aggressor = Side::kAsk;
  acc.observe(early);

  EXPECT_EQ(acc.result().ticks, std::uint64_t{0});
  EXPECT_FALSE(acc.result().stop_reached);
}

TEST(NakedWindow, ReleasesAClosedWindowsConcurrencySlot) {
  TradeStreamBuilder b;
  for (std::int64_t i = 1; i <= 4; ++i) {
    b.at(kT0 + i * kMs).trade(Side::kAsk, px(29000, 0), 1);
  }

  std::vector<NakedWindowQuery> queries;
  for (std::int64_t i = 1; i <= 4; ++i) {
    NakedWindowQuery q = long_from(kMs);
    q.entry_fill_ts = kT0 + i * kMs;
    queries.push_back(q);
  }

  const auto results = scan_over(b, queries, 1);

  ASSERT_EQ(results.size(), std::size_t{4});
  for (const NakedWindowResult& r : results) {
    EXPECT_EQ(r.ticks, std::uint64_t{1});
  }
}

TEST(NakedWindow, RejectsAStopOnTheFavourableSideOfTheEntry) {
  NakedWindowQuery too_high = long_from(kMs);
  too_high.stop_price = px(29000, 1);
  EXPECT_THROW((NakedWindowAccumulator{TickScale{kTick}, too_high}), ReplayError);

  NakedWindowQuery too_low = long_from(kMs);
  too_low.position_side = Side::kAsk;
  too_low.stop_price = px(28999, 2);
  EXPECT_THROW((NakedWindowAccumulator{TickScale{kTick}, too_low}), ReplayError);
}

TEST(NakedWindow, RejectsAnUnsetStopPriceOnEitherSide) {
  for (const Side side : {Side::kBid, Side::kAsk}) {
    NakedWindowQuery unset = long_from(kMs);
    unset.position_side = side;
    unset.stop_price = 0;
    EXPECT_THROW((NakedWindowAccumulator{TickScale{kTick}, unset}), ReplayError);
  }
}

TEST(NakedWindow, UsesTheSameAtOrThroughPredicateStopElectionUses) {
  NakedWindowQuery at_the_touch = long_from(3 * kMs);
  at_the_touch.stop_price = px(28999, 2);
  EXPECT_TRUE(scan_over(falling(), {at_the_touch})[0].stop_reached);

  NakedWindowQuery one_tick_lower = long_from(3 * kMs);
  one_tick_lower.stop_price = px(28999, 1);
  EXPECT_FALSE(scan_over(falling(), {one_tick_lower})[0].stop_reached);
}

TEST(NakedWindow, IgnoresAPrintAtTheClosingInstantBecauseTheWindowIsHalfOpen) {
  const auto results = scan_over(falling(), {long_from(2 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_FALSE(results[0].stop_reached);
  EXPECT_EQ(results[0].ticks, std::uint64_t{2});
  EXPECT_EQ(results[0].window_end_ns, kT0 + 3 * kMs);
}

TEST(NakedWindow, MeasuresAdverseExcursionInTicksAgainstTheEntryPrice) {
  const auto results = scan_over(falling(), {long_from(3 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_EQ(results[0].worst_price, px(28999, 2));
  EXPECT_EQ(results[0].mae_ticks, std::int64_t{2});
}

TEST(NakedWindow, ReportsZeroExcursionWhenPriceOnlyMovedInOurFavour) {
  TradeStreamBuilder rising;
  rising.trade(Side::kBid, px(29000, 1), 2);
  rising.trade(Side::kBid, px(29000, 2), 2);

  const auto results = scan_over(rising, {long_from(3 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_FALSE(results[0].stop_reached);
  EXPECT_EQ(results[0].mae_ticks, std::int64_t{0});
  EXPECT_EQ(results[0].worst_price, px(29000, 0));
}

TEST(NakedWindow, MirrorsTheRuleForAShortPosition) {
  TradeStreamBuilder rising;
  rising.trade(Side::kBid, px(29000, 1), 2);
  rising.trade(Side::kBid, px(29000, 2), 2);
  rising.trade(Side::kBid, px(29000, 3), 2);

  NakedWindowQuery q;
  q.entry_fill_ts = kT0 + kMs;
  q.window_ns = 3 * kMs;
  q.entry_price = px(29000, 1);
  q.stop_price = px(29000, 3);
  q.position_side = Side::kAsk;

  const auto results = scan_over(rising, {q});
  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_TRUE(results[0].stop_reached);
  EXPECT_EQ(results[0].mae_ticks, std::int64_t{2});
}

TEST(NakedWindow, CountsNoAggressorPrintsSeparatelyInsideTheWindow) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 2);
  b.auction(px(28999, 3), 3);

  const auto results = scan_over(b, {long_from(3 * kMs)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_EQ(results[0].ticks, std::uint64_t{2});
  EXPECT_EQ(results[0].no_aggressor_ticks, std::uint64_t{1});
}

TEST(NakedWindow, ResolvesTwoOverlappingWindowsInOnePass) {
  NakedWindowQuery first = long_from(3 * kMs);
  NakedWindowQuery second = long_from(3 * kMs);
  second.entry_fill_ts = kT0 + 2 * kMs;
  second.entry_price = px(28999, 3);
  second.stop_price = px(28999, 2);

  const auto results = scan_over(falling(), {first, second});

  ASSERT_EQ(results.size(), std::size_t{2});
  EXPECT_EQ(results[0].ticks, std::uint64_t{3});
  EXPECT_EQ(results[1].ticks, std::uint64_t{2});
  EXPECT_TRUE(results[0].stop_reached);
  EXPECT_TRUE(results[1].stop_reached);
}

TEST(NakedWindow, AZeroLengthWindowReachesNothing) {
  const auto results = scan_over(falling(), {long_from(0)});

  ASSERT_EQ(results.size(), std::size_t{1});
  EXPECT_EQ(results[0].ticks, std::uint64_t{0});
  EXPECT_FALSE(results[0].stop_reached);
}

TEST(NakedWindow, ThrowsWhenMoreWindowsAreOpenThanMaxConcurrent) {
  const std::vector<NakedWindowQuery> queries{long_from(9 * kMs), long_from(9 * kMs)};
  EXPECT_THROW((void)scan_over(falling(), queries, 1), ReplayError);
}

TEST(NakedWindow, WindowsThatOpenAndCloseBetweenTwoPrintsAreNotConcurrent) {
  TradeStreamBuilder quiet;
  quiet.at(kT0).trade(Side::kAsk, px(29000, 0), 1);
  quiet.at(kT0 + 10'000 * kMs).trade(Side::kAsk, px(29000, 0), 1);

  std::vector<NakedWindowQuery> queries;
  for (std::int64_t i = 1; i <= 5; ++i) {
    NakedWindowQuery q = long_from(kMs);
    q.entry_fill_ts = kT0 + i * kMs;
    queries.push_back(q);
  }

  const auto results = scan_over(quiet, queries, 2);

  ASSERT_EQ(results.size(), std::size_t{5});
  for (const NakedWindowResult& r : results) {
    EXPECT_EQ(r.ticks, std::uint64_t{0});
    EXPECT_FALSE(r.stop_reached);
  }
}

TEST(NakedWindow, ThrowsWhenQueriesAreNotSortedByEntryInstant) {
  NakedWindowQuery late = long_from(kMs);
  late.entry_fill_ts = kT0 + 5 * kMs;
  const std::vector<NakedWindowQuery> queries{late, long_from(kMs)};

  EXPECT_THROW((void)scan_over(falling(), queries), ReplayError);
}

TEST(NakedWindow, ThrowsWhenThePrintsMixInstruments) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 2);
  b.instrument(999).trade(Side::kAsk, px(1, 0), 2);

  EXPECT_THROW((void)scan_over(b, {long_from(3 * kMs)}), ReplayError);
}

TEST(NakedWindow, RejectsAWindowWithNoPositionSide) {
  NakedWindowQuery q = long_from(kMs);
  q.position_side = Side::kNone;
  EXPECT_THROW((NakedWindowAccumulator{TickScale{kTick}, q}), ReplayError);
}

TEST(NakedWindow, RejectsANegativeWindowLength) {
  EXPECT_THROW((NakedWindowAccumulator{TickScale{kTick}, long_from(-1)}), ReplayError);
}

TEST(NakedWindow, TheOnlineAndOfflineDriversAgreeOnEveryResult) {
  const TradeStreamBuilder b = falling();
  const NakedWindowQuery query = long_from(3 * kMs);

  const NakedWindowResult offline = scan_over(b, {query}).front();

  ReplayConfig config{.latency = Latency{0, 0, 0}, .scale = TickScale{kTick}};
  config.retain_ticks = true;
  TradeSourceOptions opts;
  opts.instrument_id = b.instrument_id();
  Replay replay{std::make_unique<RecordTradeSource>(b.records(), opts), config};
  (void)replay.advance_to(kT0 + 100 * kMs);

  NakedWindowAccumulator accumulator{TickScale{kTick}, query};
  for (const Tick& tick : replay.last_ticks()) {
    accumulator.observe(tick);
  }
  const NakedWindowResult& online = accumulator.result();

  EXPECT_EQ(online.ticks, offline.ticks);
  EXPECT_EQ(online.volume, offline.volume);
  EXPECT_EQ(online.stop_reached, offline.stop_reached);
  EXPECT_EQ(online.first_reach_ts, offline.first_reach_ts);
  EXPECT_EQ(online.worst_price, offline.worst_price);
  EXPECT_EQ(online.mae_ticks, offline.mae_ticks);
}

}  // namespace
}  // namespace bookreplay
