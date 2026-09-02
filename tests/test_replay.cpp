#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/replay_invariants.hpp"
#include "bookreplay/trade_source.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "toy_stream.hpp"
#include "toy_trades.hpp"

namespace bookreplay {
namespace {

using testing::as_mbo_trades;
using testing::kTick;
using testing::px;
using testing::TradeStreamBuilder;

static_assert(!std::is_default_constructible_v<ReplayConfig>);
static_assert(std::is_base_of_v<BookreplayError, DbnError>);
static_assert(std::is_base_of_v<BookreplayError, ReplayError>);

constexpr std::int64_t kT0 = 1'785'888'000'000'000'000LL;
constexpr std::int64_t kMs = 1'000'000LL;

constexpr std::int64_t tk(std::int64_t whole, std::int64_t ticks = 0) {
  return whole * 4 + ticks;
}

ReplayConfig cfg(std::int64_t entry_ns = 0, std::int64_t arm_ns = 0, std::int64_t cancel_ns = 0) {
  return ReplayConfig{.latency = Latency{entry_ns, arm_ns, cancel_ns}, .scale = TickScale{kTick}};
}

std::unique_ptr<Replay> replay_over(const TradeStreamBuilder& b, ReplayConfig config) {
  TradeSourceOptions opts;
  opts.instrument_id = b.instrument_id();
  return std::make_unique<Replay>(std::make_unique<RecordTradeSource>(b.records(), opts), config);
}

Order order_of(OrderId id, OrderType type, Side side, std::uint32_t qty, std::int64_t live_ns) {
  Order o;
  o.id = id;
  o.type = type;
  o.side = side;
  o.qty = qty;
  o.live_from_ns = live_ns;
  return o;
}

Order market(OrderId id, Side side, std::uint32_t qty, std::int64_t live_ns) {
  return order_of(id, OrderType::kMarket, side, qty, live_ns);
}

Order limit(OrderId id, Side side, std::int64_t limit_ticks, std::uint32_t qty,
            std::int64_t live_ns) {
  Order o = order_of(id, OrderType::kLimit, side, qty, live_ns);
  o.limit_ticks = limit_ticks;
  return o;
}

Order stop(OrderId id, Side side, std::int64_t trigger_ticks, std::uint32_t qty,
           std::int64_t live_ns) {
  Order o = order_of(id, OrderType::kStop, side, qty, live_ns);
  o.trigger_ticks = trigger_ticks;
  return o;
}

Order stop_limit(OrderId id, Side side, std::int64_t trigger_ticks, std::int64_t limit_ticks,
                 std::uint32_t qty, std::int64_t live_ns) {
  Order o = order_of(id, OrderType::kStopLimit, side, qty, live_ns);
  o.trigger_ticks = trigger_ticks;
  o.limit_ticks = limit_ticks;
  return o;
}

TradeStreamBuilder rising() {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);
  b.trade(Side::kBid, px(29000, 2), 5);
  b.trade(Side::kBid, px(29000, 3), 5);
  return b;
}

TradeStreamBuilder falling() {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 5);
  b.trade(Side::kAsk, px(28999, 3), 5);
  b.trade(Side::kAsk, px(28999, 2), 5);
  return b;
}

TEST(Cursor, AdvanceToTheSameTimestampIsAnEmptyNoOp) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  EXPECT_EQ(r->advance_to(kT0 + kMs).size(), std::size_t{1});
  EXPECT_TRUE(r->advance_to(kT0 + kMs).empty());
  EXPECT_EQ(r->now_ns(), kT0 + kMs);
}

TEST(Cursor, AdvanceToAPastTimestampThrowsRatherThanRewinding) {
  auto r = replay_over(rising(), cfg());
  (void)r->advance_to(kT0 + 2 * kMs);
  EXPECT_THROW((void)r->advance_to(kT0 + kMs), ReplayError);
}

TEST(Cursor, EachAdvanceReportsOnlyTheFillsInsideItsOwnWindow) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  r->submit(market(2, Side::kBid, 1, kT0 + 2 * kMs));

  EXPECT_EQ(r->advance_to(kT0 + kMs).size(), std::size_t{1});
  EXPECT_TRUE(r->advance_to(kT0 + 2 * kMs - 1).empty());
  EXPECT_EQ(r->advance_to(kT0 + 3 * kMs).size(), std::size_t{1});
}

TEST(Cursor, ReportsExhaustionOnceTheStreamRunsOut) {
  auto r = replay_over(rising(), cfg());
  EXPECT_FALSE(r->exhausted());
  (void)r->advance_to(kT0 + 100 * kMs);
  EXPECT_TRUE(r->exhausted());
}

TEST(Cursor, RunningToTheEndOfTimeDoesNotCancelTheOrdersStillWorking) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(28000, 0), 1, kT0));
  r->submit(stop_limit(2, Side::kBid, tk(29000, 1), tk(29000, 1), 1, kT0));

  EXPECT_TRUE(r->advance_to(kNever).empty());
  EXPECT_TRUE(r->exhausted());
  EXPECT_EQ(r->order(1).status, OrderStatus::kLive);
  EXPECT_EQ(r->order(2).status, OrderStatus::kElected);
  EXPECT_EQ(r->stats().cancels_applied, std::uint64_t{0});
}

TEST(Cursor, ARealCancelStillLandsWhenTheRunGoesToTheEndOfTime) {
  auto r = replay_over(rising(), cfg());
  r->submit(limit(1, Side::kBid, tk(28000, 0), 1, kT0));
  r->cancel_at(1, kT0 + 5 * kMs);

  EXPECT_TRUE(r->advance_to(kNever).empty());
  EXPECT_EQ(r->order(1).status, OrderStatus::kCancelled);
  EXPECT_EQ(r->stats().cancels_applied, std::uint64_t{1});
}

TEST(MarketOrder, FillsAtTheFirstPrintAtOrAfterItBecomesLive) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + kMs);
  EXPECT_EQ(fills[0].reason, FillReason::kMarket);
}

TEST(MarketOrder, TakesThePrintPriceWhenThatPrintWasAggressedOnItsOwnSide) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(29000, 1));
  EXPECT_EQ(fills[0].price_ticks, tk(29000, 1));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{0});
}

TEST(MarketOrder, PaysOneTickThroughAPrintTheOtherSideAggressed) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 5);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(29000, 1));
  EXPECT_EQ(fills[0].price_ticks, tk(29000, 1));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{1});
}

TEST(MarketOrder, ASellPaysThatTickTheOtherWay) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 5);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kAsk, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(28999, 3));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{1});
}

TEST(MarketOrder, TakesAnAuctionPrintAtItsClearingPriceBecauseThereIsNoSpreadToCross) {
  TradeStreamBuilder b;
  b.auction(px(29000, 0), 5);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(29000, 0));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{0});
}

TEST(MarketOrder, ChargesTheTickOnEveryPrintOfAPartialFillSequence) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 3);
  b.trade(Side::kAsk, px(28999, 3), 3);
  b.trade(Side::kAsk, px(28999, 2), 3);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 9, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{3});
  EXPECT_EQ(fills[0].price, px(29000, 1));
  EXPECT_EQ(fills[1].price, px(29000, 0));
  EXPECT_EQ(fills[2].price, px(28999, 3));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{9});
}

TEST(MarketOrder, DoesNotFillOnThePrintThatPrecedesItsEntryLatency) {
  auto r = replay_over(rising(), cfg(kMs + kMs / 2));
  r->submit(market(1, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 2 * kMs);
}

TEST(MarketOrder, PartiallyFillsWhenThePrintIsSmallerThanItsQuantity) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 3);
  b.trade(Side::kBid, px(29000, 2), 3);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 10, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{2});
  EXPECT_EQ(fills[0].qty, 3U);
  EXPECT_EQ(fills[0].remaining, 7U);
  EXPECT_EQ(fills[1].qty, 3U);
  EXPECT_EQ(fills[1].remaining, 4U);
  EXPECT_EQ(r->order(1).remaining, 4U);
}

TEST(MarketOrder, TakesItsWholeQuantityUnderTheFullRemainingPolicy) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 3);

  ReplayConfig config = cfg();
  config.fill_size = FillSizePolicy::kFullRemaining;
  auto r = replay_over(b, config);
  r->submit(market(1, Side::kBid, 10, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].qty, 10U);
  EXPECT_EQ(fills[0].remaining, 0U);
}

TEST(RestingLimit, DoesNotFillOnAPrintAtItsOwnPrice) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
}

TEST(RestingLimit, FillsOnlyOnAPrintStrictlyThroughItsPrice) {
  auto r = replay_over(falling(), cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 2 * kMs);
  EXPECT_EQ(fills[0].reason, FillReason::kLimitThrough);
}

TEST(RestingLimit, FillsAtItsOwnPriceAndNeverAtTheBetterPrintPrice) {
  auto r = replay_over(falling(), cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(29000, 0));
}

TEST(RestingLimit, IgnoresAPrintWhoseAggressorIsOnItsOwnSide) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(28999, 3), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->stats().no_aggressor_passive_skips, std::uint64_t{0});
}

TEST(RestingLimit, IgnoresAPrintWithNoAggressorUnderTheDefaultPolicy) {
  TradeStreamBuilder b;
  b.auction(px(28999, 3), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->stats().no_aggressor_passive_skips, std::uint64_t{1});
  EXPECT_EQ(r->stats().no_aggressor_ticks, std::uint64_t{1});
}

TEST(RestingLimit, ANoAggressorPrintNotThroughItsPriceIsNotCountedAsAForgoneFill) {
  TradeStreamBuilder b;
  b.auction(px(29010, 0), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->stats().no_aggressor_passive_skips, std::uint64_t{0});
  EXPECT_EQ(r->stats().no_aggressor_ticks, std::uint64_t{1});
}

TEST(RestingLimit, FillsEarlyOnANoAggressorPrintUnderTheNaiveBothSidesPolicy) {
  TradeStreamBuilder b;
  b.auction(px(28999, 3), 5);

  ReplayConfig config = cfg();
  config.no_aggressor = NoAggressorPolicy::kBothSides;
  auto r = replay_over(b, config);
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0));

  EXPECT_EQ(r->advance_to(kT0 + 10 * kMs).size(), std::size_t{1});
}

TEST(RestingLimit, CannotFillOnThePrintThatArrivesAtTheInstantItGoesLive) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(28999, 3), 5);
  b.trade(Side::kAsk, px(28999, 2), 5);

  auto r = replay_over(b, cfg());
  r->submit(limit(1, Side::kBid, tk(29000, 0), 1, kT0 + kMs));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 2 * kMs);
}

TEST(MarketOrder, FillsOnThePrintThatArrivesAtTheInstantItGoesLive) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0 + kMs));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + kMs);
}

TEST(Stop, ElectsOnAPrintExactlyAtItsTrigger) {
  auto r = replay_over(rising(), cfg());
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + kMs);
  EXPECT_EQ(fills[0].reason, FillReason::kStopElected);
  EXPECT_EQ(r->stats().elections, std::uint64_t{1});
}

TEST(Stop, ElectsOnAPrintThroughItsTrigger) {
  auto r = replay_over(rising(), cfg());
  r->submit(stop(1, Side::kBid, tk(29000, 2), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 2 * kMs);
}

TEST(Stop, DoesNotElectOnAPrintOnTheFarSideOfItsTrigger) {
  auto r = replay_over(rising(), cfg());
  r->submit(stop(1, Side::kBid, tk(29010, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->stats().elections, std::uint64_t{0});
}

TEST(Stop, ElectsOnANoAggressorPrintBecauseCmeStopsAreTradeElected) {
  TradeStreamBuilder b;
  b.auction(px(29000, 1), 5);

  auto r = replay_over(b, cfg());
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].aggressor, Side::kNone);
}

TEST(Stop, FillsAtTheElectingPrintPriceWithNoAdditionalClientLatency) {
  auto r = replay_over(rising(), cfg(0, 0, 0));
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].price, px(29000, 1));
  EXPECT_EQ(fills[0].ts_ns, r->order(1).elected_ns);
}

TEST(Stop, AnElectingPrintFromTheOtherSideCostsTheSameTickAsAMarketOrder) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);

  auto r = replay_over(b, cfg());
  r->submit(stop(1, Side::kAsk, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].reason, FillReason::kStopElected);
  EXPECT_EQ(fills[0].price, px(29000, 0));
  EXPECT_EQ(r->stats().tick_charged_qty, std::uint64_t{1});
}

TEST(Stop, DoesNotElectBeforeItsOwnEntryLatencyHasElapsed) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 2), 5);
  b.trade(Side::kAsk, px(29000, 0), 5);
  b.trade(Side::kBid, px(29000, 1), 5);

  auto r = replay_over(b, cfg(2 * kMs + kMs / 2));
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 3 * kMs);
}

TEST(Stop, WhoseTriggerIsThroughTheMarketOnArrivalIsRejectedByTheVenue) {
  auto r = replay_over(rising(), cfg(kMs + kMs / 2));
  r->submit(stop(1, Side::kAsk, tk(29010, 0), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->order(1).status, OrderStatus::kRejected);
  EXPECT_EQ(r->stats().stop_entry_rejects, std::uint64_t{1});
  EXPECT_EQ(r->stats().elections, std::uint64_t{0});
}

TEST(Stop, IsJudgedOnTheMarketAtItsArrivalAndNotAtItsSubmission) {
  auto r = replay_over(rising(), cfg(2 * kMs + kMs / 2));
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->order(1).status, OrderStatus::kRejected);
}

TEST(Stop, WhoseTriggerEqualsTheLastPrintIsRejectedBecauseCmeRequiresStrictlyBeyond) {
  auto r = replay_over(rising(), cfg(kMs + kMs / 2));
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->order(1).status, OrderStatus::kRejected);
}

TEST(Stop, SubmittedBeforeTheFirstPrintIsAcceptedBecauseNoLastTradePriceExistsYet) {
  auto r = replay_over(rising(), cfg());
  r->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(r->stats().stop_entry_rejects, std::uint64_t{0});
}

TEST(Stop, IsJudgedOnTheSameMarketWhicheverWayTheCallerStepsAdvanceTo) {
  auto whole = replay_over(rising(), cfg(kMs + kMs / 2));
  whole->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));
  (void)whole->advance_to(kT0 + 10 * kMs);

  auto stepped = replay_over(rising(), cfg(kMs + kMs / 2));
  stepped->submit(stop(1, Side::kBid, tk(29000, 1), 1, kT0));
  (void)stepped->advance_to(kT0 + kMs + kMs / 4);
  (void)stepped->advance_to(kT0 + 10 * kMs);

  EXPECT_EQ(whole->order(1).status, stepped->order(1).status);
  EXPECT_EQ(whole->stats().stop_entry_rejects, stepped->stats().stop_entry_rejects);
}

TEST(Stop, WithdrawnBeforeItReachesTheVenueIsCancelledRatherThanRejected) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);
  b.at(kT0 + 10 * kMs).trade(Side::kBid, px(29000, 1), 5);

  const auto run = [&b](bool stepped) {
    auto r = replay_over(b, cfg(0, 5 * kMs, kMs));
    Order leg = stop(1, Side::kAsk, tk(29000, 2), 1, kT0 + kMs);
    leg.latency = LatencyClass::kProtectionArm;
    r->submit(leg);
    r->cancel_at(1, kT0 + kMs);
    if (stepped) {
      (void)r->advance_to(kT0 + 2 * kMs);
    }
    (void)r->advance_to(kNever);
    return r;
  };

  auto one_shot = run(false);
  auto stepped = run(true);

  EXPECT_EQ(one_shot->order(1).status, OrderStatus::kCancelled);
  EXPECT_EQ(stepped->order(1).status, one_shot->order(1).status);
  EXPECT_EQ(one_shot->stats().cancels_applied, std::uint64_t{1});
  EXPECT_EQ(one_shot->stats().stop_entry_rejects, std::uint64_t{0});
}

TEST(MarketOrder, RefusesAPrintPriceThatHasNoRoomLeftOnTheTickGrid) {
  Tick tick{};
  tick.ts = kT0 + kMs;
  tick.price = std::numeric_limits<std::int64_t>::min();
  tick.size = 5;
  tick.aggressor = Side::kBid;
  const std::vector<Tick> day{tick};

  Replay r{std::make_unique<TickSpanSource>(day),
           ReplayConfig{.latency = Latency{0, 0, 0}, .scale = TickScale{1}}};
  r.submit(market(1, Side::kAsk, 1, kT0));

  EXPECT_THROW((void)r.advance_to(kT0 + 10 * kMs), ReplayError);
  EXPECT_THROW((void)r.advance_to(kT0 + 20 * kMs), ReplayError);
}

TEST(StopLimit, BecomesARestingLimitAndCannotFillOnTheElectingPrint) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);
  b.trade(Side::kAsk, px(29000, 1), 5);

  auto r = replay_over(b, cfg());
  r->submit(stop_limit(1, Side::kBid, tk(29000, 1), tk(29000, 2), 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 2 * kMs);
  EXPECT_EQ(fills[0].reason, FillReason::kStopLimitThrough);
  EXPECT_EQ(fills[0].price, px(29000, 2));
}

TEST(StopLimit, NeverFillsWhenNoPrintEverCrossesItsCap) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);
  b.trade(Side::kAsk, px(29000, 2), 5);

  auto r = replay_over(b, cfg());
  r->submit(stop_limit(1, Side::kBid, tk(29000, 1), tk(29000, 2), 1, kT0));

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->stats().elections, std::uint64_t{1});
  EXPECT_EQ(r->order(1).status, OrderStatus::kElected);
}

TEST(Latency, AProtectiveLegPaysProtectionArmNsInsteadOfOrderEntryNs) {
  auto r = replay_over(rising(), cfg(0, 2 * kMs + kMs / 2, 0));
  Order leg = market(1, Side::kBid, 1, kT0);
  leg.latency = LatencyClass::kProtectionArm;
  r->submit(leg);

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + 3 * kMs);
}

TEST(Latency, ALegArmedBehindTheCursorIsAcceptedAndCountedNotRejected) {
  auto r = replay_over(rising(), cfg());
  (void)r->advance_to(kT0 + 2 * kMs);

  EXPECT_NO_THROW(r->submit(market(1, Side::kBid, 1, kT0)));
  EXPECT_EQ(r->stats().late_arm_orders, std::uint64_t{1});
  EXPECT_EQ(r->stats().late_arm_ns_total, std::uint64_t{2 * kMs});
}

TEST(Latency, TheSameScriptUnderTwoEntryLatenciesFillsOnDifferentPrints) {
  auto fast = replay_over(rising(), cfg(0));
  fast->submit(market(1, Side::kBid, 1, kT0));
  const std::span<const Fill> fast_fills = fast->advance_to(kT0 + 10 * kMs);

  auto slow = replay_over(rising(), cfg(kMs + kMs / 2));
  slow->submit(market(1, Side::kBid, 1, kT0));
  const std::span<const Fill> slow_fills = slow->advance_to(kT0 + 10 * kMs);

  ASSERT_EQ(fast_fills.size(), std::size_t{1});
  ASSERT_EQ(slow_fills.size(), std::size_t{1});
  EXPECT_NE(fast_fills[0].price, slow_fills[0].price);
}

TEST(Latency, TheWholeSweepRunsFromOneDecodedTickBuffer) {
  RecordTradeSource origin{rising().records()};
  const std::vector<Tick> day = collect(origin);

  std::vector<std::int64_t> prices;
  for (const std::int64_t entry_ns : {std::int64_t{0}, kMs + kMs / 2, 2 * kMs + kMs / 2}) {
    Replay r{std::make_unique<TickSpanSource>(day), cfg(entry_ns)};
    r.submit(market(1, Side::kBid, 1, kT0));
    const std::span<const Fill> fills = r.advance_to(kT0 + 10 * kMs);
    ASSERT_EQ(fills.size(), std::size_t{1});
    prices.push_back(fills[0].price);
  }

  EXPECT_EQ(prices[0], px(29000, 1));
  EXPECT_EQ(prices[1], px(29000, 2));
  EXPECT_EQ(prices[2], px(29000, 3));
}

TEST(Cancel, TakesEffectAtRequestPlusCancelNsAndNotBefore) {
  auto r = replay_over(rising(), cfg(0, 0, 2 * kMs));
  r->submit(limit(1, Side::kAsk, tk(29000, 0), 1, kT0));
  r->cancel_at(1, kT0);

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{1});
  EXPECT_EQ(fills[0].ts_ns, kT0 + kMs);
}

TEST(Cancel, APrintAtTheEffectiveCancelInstantDoesNotFillTheOrder) {
  auto r = replay_over(rising(), cfg(0, 0, kMs));
  r->submit(limit(1, Side::kAsk, tk(29000, 0), 1, kT0));
  r->cancel_at(1, kT0);

  EXPECT_TRUE(r->advance_to(kT0 + 10 * kMs).empty());
  EXPECT_EQ(r->order(1).status, OrderStatus::kCancelled);
  EXPECT_EQ(r->stats().cancels_applied, std::uint64_t{1});
}

TEST(Cancel, OfAnUnknownIdThrows) {
  auto r = replay_over(rising(), cfg());
  EXPECT_THROW(r->cancel(7), ReplayError);
}

TEST(Cancel, OfAnAlreadyFilledOrderIsAcceptedAndInert) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  (void)r->advance_to(kT0 + 10 * kMs);

  EXPECT_NO_THROW(r->cancel(1));
  EXPECT_EQ(r->order(1).status, OrderStatus::kFilled);
}

TEST(Cancel, AtAnInstantBeforeTheCursorThrows) {
  auto r = replay_over(rising(), cfg());
  r->submit(limit(1, Side::kAsk, tk(29000, 0), 1, kT0));
  (void)r->advance_to(kT0 + 2 * kMs);

  EXPECT_THROW(r->cancel_at(1, kT0), ReplayError);
}

TEST(Cancel, IsReportedByOrderViewEvenWhenNoPrintFollowedIt) {
  auto r = replay_over(rising(), cfg());
  r->submit(limit(1, Side::kAsk, tk(29500, 0), 1, kT0));
  r->cancel_at(1, kT0 + 5 * kMs);
  (void)r->advance_to(kT0 + 6 * kMs);

  EXPECT_EQ(r->order(1).status, OrderStatus::kCancelled);
}

TEST(Submit, RejectsADuplicateOrderId) {
  auto r = replay_over(rising(), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  EXPECT_THROW(r->submit(market(1, Side::kBid, 1, kT0)), ReplayError);
}

TEST(Submit, RejectsAMalformedOrder) {
  auto r = replay_over(rising(), cfg());
  EXPECT_THROW(r->submit(market(1, Side::kNone, 1, kT0)), ReplayError);
}

TEST(OrderLookup, OfAnUnsubmittedIdThrows) {
  auto r = replay_over(rising(), cfg());
  EXPECT_THROW((void)r->order(9), ReplayError);
}

TEST(Oco, AFillOnOneMemberReducesEverySiblingByTheSameQuantity) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 3);

  auto r = replay_over(b, cfg());
  Order taker = market(1, Side::kBid, 10, kT0);
  taker.oco_group = 7;
  Order sibling = limit(2, Side::kAsk, tk(29500, 0), 10, kT0);
  sibling.oco_group = 7;
  r->submit(taker);
  r->submit(sibling);

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(r->order(1).remaining, 7U);
  EXPECT_EQ(r->order(2).remaining, 7U);
  EXPECT_EQ(r->order(2).oco_reduced, 3U);
  EXPECT_EQ(r->order(2).status, OrderStatus::kLive);
}

TEST(Oco, AMemberReachingZeroQuantityCancelsItsSiblings) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 10);

  auto r = replay_over(b, cfg());
  Order taker = market(1, Side::kBid, 10, kT0);
  taker.oco_group = 7;
  Order sibling = limit(2, Side::kAsk, tk(29500, 0), 10, kT0);
  sibling.oco_group = 7;
  r->submit(taker);
  r->submit(sibling);

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(r->order(1).status, OrderStatus::kFilled);
  EXPECT_EQ(r->order(2).status, OrderStatus::kOcoCancelled);
}

TEST(Oco, AtOneLotQuantityLinkingAndCancelOnFirstFillAgree) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);

  auto r = replay_over(b, cfg());
  Order taker = market(1, Side::kBid, 1, kT0);
  taker.oco_group = 7;
  Order sibling = limit(2, Side::kAsk, tk(29500, 0), 1, kT0);
  sibling.oco_group = 7;
  r->submit(taker);
  r->submit(sibling);

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(r->order(2).status, OrderStatus::kOcoCancelled);
  EXPECT_EQ(r->order(2).remaining, 0U);
}

TEST(Oco, AtTenLotsAPartialFillLeavesTheSiblingLiveForTheRemainder) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 3);

  auto r = replay_over(b, cfg());
  Order taker = market(1, Side::kBid, 10, kT0);
  taker.oco_group = 7;
  Order protector = limit(2, Side::kAsk, tk(29500, 0), 10, kT0);
  protector.oco_group = 7;
  r->submit(taker);
  r->submit(protector);

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_NE(r->order(2).status, OrderStatus::kOcoCancelled);
  EXPECT_EQ(r->order(2).remaining, 7U);
}

TEST(Oco, MembersWithUnequalQuantitiesStillShareOneReductionAmount) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 2);

  auto r = replay_over(b, cfg());
  Order taker = market(1, Side::kBid, 5, kT0);
  taker.oco_group = 7;
  Order sibling = limit(2, Side::kAsk, tk(29500, 0), 10, kT0);
  sibling.oco_group = 7;
  r->submit(taker);
  r->submit(sibling);

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(r->order(1).qty - r->order(1).remaining, 2U);
  EXPECT_EQ(r->order(2).qty - r->order(2).remaining, 2U);
}

TEST(Oco, LeavesAnUngroupedOrderUntouched) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 10);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 10, kT0));
  r->submit(limit(2, Side::kAsk, tk(29500, 0), 10, kT0));

  (void)r->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(r->order(2).status, OrderStatus::kLive);
  EXPECT_EQ(r->order(2).remaining, 10U);
}

TEST(Instrument, APrintFromASecondInstrumentPoisonsTheReplayRatherThanFillingAnything) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 5);
  b.instrument(999).trade(Side::kBid, px(1, 0), 5);

  Replay r{std::make_unique<RecordTradeSource>(b.records()), cfg()};
  r.submit(stop(1, Side::kAsk, tk(28000, 0), 1, kT0));

  EXPECT_THROW((void)r.advance_to(kT0 + 10 * kMs), ReplayError);
  EXPECT_THROW((void)r.advance_to(kT0 + 20 * kMs), ReplayError);
}

TEST(Instrument, APoisonedReplayRefusesNewWorkButStillReportsWhatHappened) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 5);
  b.instrument(999).trade(Side::kBid, px(1, 0), 5);

  auto r = std::make_unique<Replay>(std::make_unique<RecordTradeSource>(b.records()), cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  EXPECT_THROW((void)r->advance_to(kT0 + 10 * kMs), ReplayError);

  EXPECT_THROW(r->submit(market(2, Side::kBid, 1, kT0)), ReplayError);
  EXPECT_THROW(r->cancel(1), ReplayError);
  EXPECT_EQ(r->order(1).filled, 1U);
}

TEST(Ties, TwoPrintsAtOneNanosecondAreProcessedInStreamOrderAndNeverSorted) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 1);
  b.same_ts().trade(Side::kBid, px(29000, 5), 1);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 2, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{2});
  EXPECT_EQ(fills[0].price, px(29000, 1));
  EXPECT_EQ(fills[1].price, px(29000, 5));
  EXPECT_EQ(fills[0].ts_ns, fills[1].ts_ns);
}

TEST(Ties, TwoOrdersFillingOnOnePrintAreReportedInSubmitOrder) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 10);

  auto r = replay_over(b, cfg());
  r->submit(market(41, Side::kBid, 1, kT0));
  r->submit(market(17, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{2});
  EXPECT_EQ(fills[0].order_id, OrderId{41});
  EXPECT_EQ(fills[1].order_id, OrderId{17});
}

TEST(Ties, FillTimestampsRepeatButFillSequenceNumbersDoNot) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 10);

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 1, kT0));
  r->submit(market(2, Side::kBid, 1, kT0));

  const std::span<const Fill> fills = r->advance_to(kT0 + 10 * kMs);
  ASSERT_EQ(fills.size(), std::size_t{2});
  EXPECT_EQ(fills[0].ts_ns, fills[1].ts_ns);
  EXPECT_LT(fills[0].seq, fills[1].seq);
}

TEST(Determinism, TheSameStreamAndScriptProduceAByteIdenticalFillList) {
  const auto run = [] {
    auto r = replay_over(rising(), cfg());
    r->submit(market(1, Side::kBid, 4, kT0));
    return fill_digest(r->advance_to(kT0 + 10 * kMs));
  };
  EXPECT_EQ(run(), run());
}

TEST(Determinism, ATradesFileAndAnMboFileOfTheSamePrintsProduceIdenticalFills) {
  const TradeStreamBuilder b = rising();
  TradeSourceOptions opts;
  opts.instrument_id = b.instrument_id();

  Replay from_trades{std::make_unique<RecordTradeSource>(b.records(), opts), cfg()};
  Replay from_mbo{std::make_unique<RecordTradeSource>(as_mbo_trades(b), opts), cfg()};
  from_trades.submit(market(1, Side::kBid, 4, kT0));
  from_mbo.submit(market(1, Side::kBid, 4, kT0));

  EXPECT_EQ(fill_digest(from_trades.advance_to(kT0 + 10 * kMs)),
            fill_digest(from_mbo.advance_to(kT0 + 10 * kMs)));
}

TEST(Allocation, TheSteadyStateLoopReportsNoBufferReallocations) {
  TradeStreamBuilder b;
  for (int i = 0; i < 200; ++i) {
    b.trade(Side::kBid, px(29000, 1), 1);
  }

  auto r = replay_over(b, cfg());
  r->submit(market(1, Side::kBid, 150, kT0));
  (void)r->advance_to(kT0 + 1000 * kMs);

  EXPECT_EQ(r->stats().fills, std::uint64_t{150});
  EXPECT_EQ(r->stats().reallocations, std::uint64_t{0});
}

TEST(Ticks, AreNotRetainedUnlessTheConfigAsksForThem) {
  auto quiet = replay_over(rising(), cfg());
  (void)quiet->advance_to(kT0 + 10 * kMs);
  EXPECT_TRUE(quiet->last_ticks().empty());

  ReplayConfig config = cfg();
  config.retain_ticks = true;
  auto loud = replay_over(rising(), config);
  (void)loud->advance_to(kT0 + 10 * kMs);
  EXPECT_EQ(loud->last_ticks().size(), std::size_t{3});
}

}  // namespace
}  // namespace bookreplay
