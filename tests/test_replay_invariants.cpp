#include "bookreplay/order.hpp"
#include "bookreplay/replay.hpp"
#include "bookreplay/replay_invariants.hpp"
#include "bookreplay/trade_source.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "toy_replay.hpp"
#include "toy_stream.hpp"
#include "toy_trades.hpp"

namespace bookreplay {
namespace {

using testing::CancelOnFirstFillReplay;
using testing::IgnoresCancelReplay;
using testing::IgnoresEntryLatencyReplay;
using testing::kTick;
using testing::LimitAtTheTouchReplay;
using testing::NoAggressorBothSidesReplay;
using testing::OverfillReplay;
using testing::px;
using testing::ToyReplayBase;
using testing::TradeStreamBuilder;
using testing::UnorderedFillReplay;

static_assert(ReplayLike<Replay>);
static_assert(ReplayLike<ToyReplayBase>);
static_assert(ReplayLike<CancelOnFirstFillReplay>);
static_assert(ReplayLike<NoAggressorBothSidesReplay>);
static_assert(ReplayLike<IgnoresEntryLatencyReplay>);
static_assert(ReplayLike<IgnoresCancelReplay>);
static_assert(ReplayLike<OverfillReplay>);
static_assert(ReplayLike<UnorderedFillReplay>);
static_assert(ReplayLike<LimitAtTheTouchReplay>);

constexpr std::int64_t kT0 = 1'785'888'000'000'000'000LL;
constexpr std::int64_t kMs = 1'000'000LL;
constexpr std::uint64_t k0 = 0;
constexpr std::uint64_t k1 = 1;

constexpr std::int64_t tk(std::int64_t whole, std::int64_t ticks = 0) {
  return whole * 4 + ticks;
}

std::vector<Tick> ticks_of(const TradeStreamBuilder& builder) {
  RecordTradeSource source{builder.records()};
  return collect(source);
}

Order order_of(OrderId id, OrderType type, Side side, std::uint32_t qty) {
  Order o;
  o.id = id;
  o.type = type;
  o.side = side;
  o.qty = qty;
  o.live_from_ns = kT0;
  return o;
}

std::vector<Tick> one_print(std::uint32_t size) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), size);
  return ticks_of(b);
}

template <typename Driver>
void submit_bracket(ReplayHarness<Driver>& harness, std::uint32_t qty) {
  Order taker = order_of(1, OrderType::kMarket, Side::kBid, qty);
  taker.oco_group = 7;
  Order protector = order_of(2, OrderType::kLimit, Side::kAsk, qty);
  protector.oco_group = 7;
  protector.limit_ticks = tk(29500, 0);
  harness.submit(taker);
  harness.submit(protector);
}

Replay real_engine(const TradeStreamBuilder& b, Latency latency) {
  TradeSourceOptions opts;
  opts.instrument_id = b.instrument_id();
  return Replay{std::make_unique<RecordTradeSource>(b.records(), opts),
                ReplayConfig{.latency = latency, .scale = TickScale{kTick}}};
}

TEST(CorrectDriver, ABracketRoundTripSatisfiesEveryInvariant) {
  ToyReplayBase driver{one_print(10), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  submit_bracket(harness, 10);

  EXPECT_EQ(harness.advance_to(kT0 + 10 * kMs).size(), std::size_t{1});
  EXPECT_TRUE(harness.ok());
  EXPECT_EQ(harness.report().fills, k1);
  EXPECT_TRUE(harness.report().violations.empty());
}

TEST(CorrectDriver, APartialFillLeavesTheGroupConsistent) {
  ToyReplayBase driver{one_print(3), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  submit_bracket(harness, 10);

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_TRUE(harness.ok());
  EXPECT_EQ(driver.order(2).remaining, 7U);
}

TEST(RealEngine, ABracketRoundTripOverTheRealReplaySatisfiesEveryInvariant) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 10);

  Replay engine = real_engine(b, Latency{0, 0, 0});
  ReplayHarness harness{engine, Latency{0, 0, 0}};
  submit_bracket(harness, 10);

  EXPECT_EQ(harness.advance_to(kT0 + 10 * kMs).size(), std::size_t{1});
  EXPECT_TRUE(harness.ok());
}

TEST(RealEngine, ASiblingZeroedByReductionFromAPartialFillIsLegalPlayNotTermination) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 8);

  Replay engine = real_engine(b, Latency{0, 0, 0});
  ReplayHarness harness{engine, Latency{0, 0, 0}};
  Order taker = order_of(1, OrderType::kMarket, Side::kBid, 10);
  taker.oco_group = 7;
  Order sibling = order_of(2, OrderType::kLimit, Side::kAsk, 5);
  sibling.oco_group = 7;
  sibling.limit_ticks = tk(29500, 0);
  harness.submit(taker);
  harness.submit(sibling);

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_TRUE(harness.ok());
  EXPECT_EQ(engine.order(1).status, OrderStatus::kLive);
  EXPECT_EQ(engine.order(1).remaining, 2U);
  EXPECT_EQ(engine.order(2).status, OrderStatus::kOcoCancelled);
  EXPECT_EQ(engine.order(2).oco_reduced, 5U);
}

TEST(CancelOnFirstFill, AgreesWithQuantityLinkingAtOneLot) {
  CancelOnFirstFillReplay driver{one_print(5), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  submit_bracket(harness, 1);

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_TRUE(harness.ok());
}

TEST(CancelOnFirstFill, LeavesTheRemainderUnprotectedAtTenLotsAndTripsGroupTermination) {
  CancelOnFirstFillReplay driver{one_print(3), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  submit_bracket(harness, 10);

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_FALSE(harness.ok());
  EXPECT_EQ(harness.report().termination_violations, k1);
  EXPECT_EQ(harness.report().violations.front().invariant, ReplayInvariant::kGroupTermination);
  EXPECT_EQ(driver.order(2).status, OrderStatus::kOcoCancelled);
  EXPECT_EQ(driver.order(2).remaining, 10U);
}

TEST(NoAggressorBothSides, FillsARestingLimitEarlyAndTripsPassiveAttribution) {
  TradeStreamBuilder b;
  b.auction(px(28999, 3), 5);

  NoAggressorBothSidesReplay driver{ticks_of(b), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  Order resting = order_of(1, OrderType::kLimit, Side::kBid, 1);
  resting.limit_ticks = tk(29000, 0);
  harness.submit(resting);

  EXPECT_EQ(harness.advance_to(kT0 + 10 * kMs).size(), std::size_t{1});
  EXPECT_FALSE(harness.ok());
  EXPECT_EQ(harness.report().attribution_violations, k1);
}

TEST(IgnoresEntryLatency, TripsCausalityByFillingBeforeTheOrderIsLive) {
  const Latency latency{kMs + kMs / 2, 0, 0};
  IgnoresEntryLatencyReplay driver{one_print(5), latency, TickScale{kTick}};
  ReplayHarness harness{driver, latency};
  harness.submit(order_of(1, OrderType::kMarket, Side::kBid, 1));

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_FALSE(harness.ok());
  EXPECT_EQ(harness.report().causality_violations, k1);
}

TEST(IgnoresCancelLatency, TripsCausalityByFillingAfterTheEffectiveCancel) {
  const Latency latency{0, 0, 0};
  IgnoresCancelReplay driver{one_print(5), latency, TickScale{kTick}};
  ReplayHarness harness{driver, latency};
  harness.submit(order_of(1, OrderType::kMarket, Side::kBid, 1));
  harness.cancel_at(1, kT0);

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_FALSE(harness.ok());
  EXPECT_EQ(harness.report().causality_violations, k1);
}

TEST(Overfills, TripsQuantityConservation) {
  OverfillReplay driver{one_print(3), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  harness.submit(order_of(1, OrderType::kMarket, Side::kBid, 3));

  (void)harness.advance_to(kT0 + 10 * kMs);
  EXPECT_FALSE(harness.ok());
  EXPECT_GT(harness.report().conservation_violations, k0);
}

TEST(UnorderedFills, TripsMonotoneFillTimestamps) {
  UnorderedFillReplay driver{one_print(10), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  harness.submit(order_of(1, OrderType::kMarket, Side::kBid, 1));
  harness.submit(order_of(2, OrderType::kMarket, Side::kBid, 1));

  EXPECT_EQ(harness.advance_to(kT0 + 10 * kMs).size(), std::size_t{2});
  EXPECT_FALSE(harness.ok());
  EXPECT_EQ(harness.report().monotone_violations, k1);
}

TEST(FillsAtTheTouch, IsAModellingChoiceThatNoInvariantCanCatch) {
  TradeStreamBuilder b;
  b.trade(Side::kAsk, px(29000, 0), 5);

  LimitAtTheTouchReplay driver{ticks_of(b), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  Order resting = order_of(1, OrderType::kLimit, Side::kBid, 1);
  resting.limit_ticks = tk(29000, 0);
  harness.submit(resting);

  EXPECT_EQ(harness.advance_to(kT0 + 10 * kMs).size(), std::size_t{1});
  EXPECT_TRUE(harness.ok());
}

TEST(Reporting, ARunWithNoOrdersIsCleanRatherThanVacuouslyBroken) {
  ToyReplayBase driver{one_print(5), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};

  EXPECT_TRUE(harness.advance_to(kT0 + 10 * kMs).empty());
  EXPECT_TRUE(harness.ok());
  EXPECT_EQ(harness.report().fills, k0);
}

TEST(Reporting, TheFirstViolationCarriesThePrecedingFills) {
  CancelOnFirstFillReplay driver{one_print(3), Latency{0, 0, 0}, TickScale{kTick}};
  ReplayHarness harness{driver, Latency{0, 0, 0}};
  submit_bracket(harness, 10);

  (void)harness.advance_to(kT0 + 10 * kMs);
  ASSERT_FALSE(harness.report().violations.empty());
  EXPECT_EQ(harness.report().first_violation_context.size(), std::size_t{1});
}

TEST(Differential, TheEngineAndTheToyOracleAgreeOnRandomStreamsAndScripts) {
  std::mt19937_64 rng{0xB00CD1FFULL};

  for (int round = 0; round < 40; ++round) {
    TradeStreamBuilder b;
    std::int64_t level = tk(29000, 0);
    const std::uint64_t prints = 20 + rng() % 20;
    for (std::uint64_t i = 0; i < prints; ++i) {
      level += static_cast<std::int64_t>(rng() % 5) - 2;
      const std::uint32_t size = 1 + static_cast<std::uint32_t>(rng() % 5);
      switch (rng() % 5) {
        case 0:
          b.auction(level * kTick, size);
          break;
        case 1:
          b.trade(Side::kAsk, level * kTick, size);
          break;
        default:
          b.trade(Side::kBid, level * kTick, size);
          break;
      }
    }

    const Latency latency{static_cast<std::int64_t>(rng() % 3) * kMs / 2,
                          static_cast<std::int64_t>(rng() % 3) * kMs / 2,
                          static_cast<std::int64_t>(rng() % 3) * kMs / 2};
    Replay engine = real_engine(b, latency);
    ReplayHarness harness{engine, latency};
    ToyReplayBase oracle{ticks_of(b), latency, TickScale{kTick}};

    const int orders = 2 + static_cast<int>(rng() % 5);
    for (int i = 0; i < orders; ++i) {
      Order o;
      o.id = static_cast<OrderId>(i + 1);
      o.qty = 1 + static_cast<std::uint32_t>(rng() % 8);
      o.side = rng() % 2 == 0 ? Side::kBid : Side::kAsk;
      o.live_from_ns = kT0 + static_cast<std::int64_t>(rng() % prints) * kMs;
      o.latency = rng() % 2 == 0 ? LatencyClass::kOrderEntry : LatencyClass::kProtectionArm;
      o.oco_group = rng() % 3 == 0 ? 1 + static_cast<OcoGroup>(rng() % 2) : kNoOcoGroup;
      const std::int64_t near = tk(29000, 0) + static_cast<std::int64_t>(rng() % 9) - 4;
      switch (rng() % 4) {
        case 0:
          o.type = OrderType::kMarket;
          break;
        case 1:
          o.type = OrderType::kLimit;
          o.limit_ticks = near;
          break;
        case 2:
          o.type = OrderType::kStop;
          o.trigger_ticks = near;
          break;
        default:
          o.type = OrderType::kStopLimit;
          o.trigger_ticks = near;
          o.limit_ticks = o.side == Side::kBid ? near + static_cast<std::int64_t>(rng() % 3)
                                               : near - static_cast<std::int64_t>(rng() % 3);
          break;
      }
      harness.submit(o);
      oracle.submit(o);
      if (rng() % 4 == 0) {
        const std::int64_t at = kT0 + static_cast<std::int64_t>(rng() % prints) * kMs;
        harness.cancel_at(o.id, at);
        oracle.cancel_at(o.id, at);
      }
    }

    std::int64_t target = kT0;
    for (int step = 0; step < 4; ++step) {
      target += static_cast<std::int64_t>(rng() % prints) * kMs / 2;
      EXPECT_EQ(fill_digest(harness.advance_to(target)), fill_digest(oracle.advance_to(target)))
          << "round " << round << " step " << step;
    }
    EXPECT_EQ(fill_digest(harness.advance_to(kNever)), fill_digest(oracle.advance_to(kNever)))
        << "round " << round;
    EXPECT_TRUE(harness.ok()) << "round " << round;

    for (int i = 0; i < orders; ++i) {
      const OrderView ours = engine.order(static_cast<OrderId>(i + 1));
      const OrderView theirs = oracle.order(static_cast<OrderId>(i + 1));
      EXPECT_EQ(ours.remaining, theirs.remaining) << "round " << round << " order " << i + 1;
      EXPECT_EQ(ours.filled, theirs.filled) << "round " << round << " order " << i + 1;
      EXPECT_EQ(ours.oco_reduced, theirs.oco_reduced) << "round " << round << " order " << i + 1;
    }
  }
}

TEST(Digest, DiffersWhenAnyOneFillFieldDiffers) {
  Fill a{};
  a.seq = 1;
  a.order_id = 2;
  a.price = 3;
  a.qty = 4;

  Fill b = a;
  b.qty = 5;

  const std::array<Fill, 1> one{a};
  const std::array<Fill, 1> two{b};
  EXPECT_NE(fill_digest(one), fill_digest(two));
}

TEST(Digest, IgnoresStructPaddingSoTwoEqualFillsAlwaysAgree) {
  Fill clean{};
  clean.seq = 9;
  clean.order_id = 11;
  clean.price = 13;
  clean.qty = 2;
  clean.side = Side::kBid;
  clean.aggressor = Side::kAsk;
  clean.reason = FillReason::kLimitThrough;

  // Dirtying the padding is the whole point, so the bytes go in through a
  // void* -- writing a class type through memset is a warning otherwise.
  static_assert(std::is_trivially_copyable_v<Fill>);
  Fill dirty;
  std::memset(static_cast<void*>(&dirty), 0xFF, sizeof(Fill));
  dirty.seq = clean.seq;
  dirty.order_id = clean.order_id;
  dirty.oco_group = clean.oco_group;
  dirty.ts_ns = clean.ts_ns;
  dirty.ts_event = clean.ts_event;
  dirty.price = clean.price;
  dirty.price_ticks = clean.price_ticks;
  dirty.qty = clean.qty;
  dirty.remaining = clean.remaining;
  dirty.print_size = clean.print_size;
  dirty.sequence = clean.sequence;
  dirty.side = clean.side;
  dirty.aggressor = clean.aggressor;
  dirty.reason = clean.reason;

  const std::array<Fill, 1> one{clean};
  const std::array<Fill, 1> two{dirty};
  EXPECT_EQ(fill_digest(one), fill_digest(two));
}

TEST(Digest, IsOrderSensitive) {
  Fill a{};
  a.seq = 1;
  Fill b{};
  b.seq = 2;

  const std::array<Fill, 2> forward{a, b};
  const std::array<Fill, 2> backward{b, a};
  EXPECT_NE(fill_digest(forward), fill_digest(backward));
}

}  // namespace
}  // namespace bookreplay
