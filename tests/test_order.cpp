#include "bookreplay/order.hpp"

#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::kTick;
using testing::px;

static_assert(!std::is_default_constructible_v<Latency>);
static_assert(!std::is_default_constructible_v<TickScale>);

Order market_order() {
  Order o;
  o.id = 1;
  o.type = OrderType::kMarket;
  o.side = Side::kBid;
  o.qty = 1;
  o.live_from_ns = 1'000;
  return o;
}

TEST(TickScale, ConvertsWholeTicksToWirePricesExactly) {
  const TickScale scale{kTick};
  EXPECT_EQ(scale.to_price(4), px(1, 0));
  EXPECT_EQ(scale.to_price(-4), -px(1, 0));
  EXPECT_EQ(scale.to_ticks(px(1, 0)), std::int64_t{4});
}

TEST(TickScale, FloorsTowardNegativeInfinityRatherThanTowardZero) {
  const TickScale scale{kTick};
  EXPECT_EQ(scale.to_ticks(-1), std::int64_t{-1});
  EXPECT_EQ(scale.to_ticks(-kTick), std::int64_t{-1});
  EXPECT_EQ(scale.to_ticks(kTick - 1), std::int64_t{0});
}

TEST(TickScale, RejectsATickCountThatWouldOverflowTheWirePrice) {
  const TickScale scale{kTick};
  EXPECT_NO_THROW((void)scale.to_price(scale.max_ticks()));
  EXPECT_THROW((void)scale.to_price(scale.max_ticks() + 1), ReplayError);
  EXPECT_THROW((void)scale.to_price(-scale.max_ticks() - 1), ReplayError);
}

TEST(TickScale, RecognizesAPrintThatDoesNotLandOnATick) {
  const TickScale scale{kTick};
  EXPECT_TRUE(scale.on_tick(px(29000, 1)));
  EXPECT_FALSE(scale.on_tick(px(29000, 1) + 1));
}

TEST(TickScale, RejectsANonPositiveTickSize) {
  EXPECT_THROW((TickScale{0}), ReplayError);
  EXPECT_THROW((TickScale{-kTick}), ReplayError);
}

TEST(Latency, RejectsANegativeParameter) {
  EXPECT_THROW((Latency{-1, 0, 0}), ReplayError);
  EXPECT_THROW((Latency{0, -1, 0}), ReplayError);
  EXPECT_THROW((Latency{0, 0, -1}), ReplayError);
}

TEST(Latency, SelectsProtectionArmNsForAProtectiveLeg) {
  const Latency latency{10, 250, 5};
  EXPECT_EQ(latency.for_class(LatencyClass::kOrderEntry), std::int64_t{10});
  EXPECT_EQ(latency.for_class(LatencyClass::kProtectionArm), std::int64_t{250});
}

TEST(OrderValidation, AcceptsAPlainMarketOrder) {
  const TickScale scale{kTick};
  EXPECT_NO_THROW(validate(market_order(), scale));
}

TEST(OrderValidation, RejectsZeroQuantity) {
  const TickScale scale{kTick};
  Order o = market_order();
  o.qty = 0;
  EXPECT_THROW(validate(o, scale), ReplayError);
}

TEST(OrderValidation, RejectsSideNone) {
  const TickScale scale{kTick};
  Order o = market_order();
  o.side = Side::kNone;
  EXPECT_THROW(validate(o, scale), ReplayError);
}

TEST(OrderValidation, RejectsANegativeDecisionInstant) {
  const TickScale scale{kTick};
  Order o = market_order();
  o.live_from_ns = -1;
  EXPECT_THROW(validate(o, scale), ReplayError);
}

TEST(OrderValidation, RejectsAStopWhoseTriggerCannotBeRepresented) {
  const TickScale scale{kTick};
  Order o = market_order();
  o.type = OrderType::kStop;
  o.trigger_ticks = scale.max_ticks() + 1;
  EXPECT_THROW(validate(o, scale), ReplayError);
}

TEST(OrderValidation, RejectsAStopLimitWhoseCapIsOnTheWrongSideOfItsTrigger) {
  const TickScale scale{kTick};
  Order buy = market_order();
  buy.type = OrderType::kStopLimit;
  buy.side = Side::kBid;
  buy.trigger_ticks = 100;
  buy.limit_ticks = 98;
  EXPECT_THROW(validate(buy, scale), ReplayError);

  buy.limit_ticks = 102;
  EXPECT_NO_THROW(validate(buy, scale));

  Order sell = buy;
  sell.side = Side::kAsk;
  EXPECT_THROW(validate(sell, scale), ReplayError);

  sell.limit_ticks = 98;
  EXPECT_NO_THROW(validate(sell, scale));
}

TEST(OrderNames, AreStableAcrossEveryEnumerator) {
  EXPECT_STREQ(order_type_name(OrderType::kStopLimit), "stop_limit");
  EXPECT_STREQ(order_status_name(OrderStatus::kOcoCancelled), "oco_cancelled");
  EXPECT_STREQ(fill_reason_name(FillReason::kStopElected), "stop_elected");
  EXPECT_STREQ(latency_class_name(LatencyClass::kProtectionArm), "protection_arm");
}

}  // namespace
}  // namespace bookreplay
