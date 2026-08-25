#include "bookreplay/dbn.hpp"
#include "bookreplay/order.hpp"
#include "bookreplay/trade_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "dbn_encoder.hpp"
#include "toy_stream.hpp"
#include "toy_trades.hpp"

namespace bookreplay {
namespace {

using testing::as_mbo_trades;
using testing::px;
using testing::TradeStreamBuilder;

TradeStreamBuilder two_prints() {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 1), 3);
  b.trade(Side::kAsk, px(29000, 0), 5);
  return b;
}

TEST(TradeSource, NormalizesATradeMsgAndAnMboTradeIntoTheSameTick) {
  const TradeStreamBuilder b = two_prints();

  RecordTradeSource from_trades{b.records()};
  RecordTradeSource from_mbo{as_mbo_trades(b)};

  const std::vector<Tick> trade_ticks = collect(from_trades);
  const std::vector<Tick> mbo_ticks = collect(from_mbo);

  ASSERT_EQ(trade_ticks.size(), mbo_ticks.size());
  ASSERT_EQ(trade_ticks.size(), std::size_t{2});
  for (std::size_t i = 0; i < trade_ticks.size(); ++i) {
    EXPECT_EQ(trade_ticks[i].ts, mbo_ticks[i].ts);
    EXPECT_EQ(trade_ticks[i].ts_event, mbo_ticks[i].ts_event);
    EXPECT_EQ(trade_ticks[i].price, mbo_ticks[i].price);
    EXPECT_EQ(trade_ticks[i].size, mbo_ticks[i].size);
    EXPECT_EQ(trade_ticks[i].instrument_id, mbo_ticks[i].instrument_id);
    EXPECT_EQ(trade_ticks[i].sequence, mbo_ticks[i].sequence);
    EXPECT_EQ(trade_ticks[i].aggressor, mbo_ticks[i].aggressor);
  }
}

TEST(TradeSource, TurnsOnlyActionTIntoATickOnAnMboStream) {
  RecordTradeSource source{testing::clean_trade_event()};
  const std::vector<Tick> ticks = collect(source);

  EXPECT_EQ(ticks.size(), std::size_t{1});
  EXPECT_EQ(source.stats().ticks, std::uint64_t{1});
  EXPECT_TRUE(source.stats().reconciles());
}

TEST(TradeSource, KeepsAndCountsAPrintWithNoAggressor) {
  TradeStreamBuilder b;
  b.auction(px(29000, 0), 4);

  RecordTradeSource source{b.records()};
  const std::vector<Tick> ticks = collect(source);

  ASSERT_EQ(ticks.size(), std::size_t{1});
  EXPECT_EQ(ticks[0].aggressor, Side::kNone);
  EXPECT_EQ(source.stats().no_aggressor, std::uint64_t{1});
  EXPECT_EQ(source.stats().ticks, std::uint64_t{1});
}

TEST(TradeSource, DropsAndCountsAPrintWithAnUndefinedPrice) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 4).undef_price();

  RecordTradeSource source{b.records()};
  EXPECT_TRUE(collect(source).empty());
  EXPECT_EQ(source.stats().undef_price, std::uint64_t{1});
  EXPECT_TRUE(source.stats().reconciles());
}

TEST(TradeSource, DropsAndCountsAZeroSizePrint) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 0);

  RecordTradeSource source{b.records()};
  EXPECT_TRUE(collect(source).empty());
  EXPECT_EQ(source.stats().zero_size, std::uint64_t{1});
  EXPECT_TRUE(source.stats().reconciles());
}

TEST(TradeSource, DropsAndCountsARecordFlaggedBadTsRecv) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 4).bad_ts_recv();

  RecordTradeSource source{b.records()};
  EXPECT_TRUE(collect(source).empty());
  EXPECT_EQ(source.stats().bad_ts_recv, std::uint64_t{1});
  EXPECT_TRUE(source.stats().reconciles());
}

TEST(TradeSource, KeepsOnlyTheRequestedInstrumentId) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 4);
  b.instrument(999).trade(Side::kBid, px(1, 0), 4);

  TradeSourceOptions opts;
  opts.instrument_id = 42004177;
  RecordTradeSource source{b.records(), opts};

  const std::vector<Tick> ticks = collect(source);
  ASSERT_EQ(ticks.size(), std::size_t{1});
  EXPECT_EQ(ticks[0].instrument_id, 42004177U);
  EXPECT_EQ(source.stats().skipped_other_instrument, std::uint64_t{1});
  EXPECT_TRUE(source.stats().reconciles());
}

TEST(TradeSource, ThrowsOnABackwardTimestampRatherThanReorderingTheQueue) {
  TradeStreamBuilder b;
  b.at(2'000'000'000).trade(Side::kBid, px(29000, 0), 1);
  b.at(1'000'000'000).trade(Side::kBid, px(29000, 0), 1);

  RecordTradeSource source{b.records()};
  EXPECT_THROW((void)collect(source), ReplayError);
}

TEST(TradeSource, AcceptsTwoPrintsAtOneInstant) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 1);
  b.same_ts().trade(Side::kBid, px(29000, 1), 2);

  RecordTradeSource source{b.records()};
  const std::vector<Tick> ticks = collect(source);
  ASSERT_EQ(ticks.size(), std::size_t{2});
  EXPECT_EQ(ticks[0].ts, ticks[1].ts);
}

TEST(TradeSource, ThrowsOnAnActionByteOutsideTheDocumentedSet) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 1).corrupt_action('Z');

  RecordTradeSource source{b.records()};
  EXPECT_THROW((void)collect(source), ReplayError);
}

TEST(TradeSource, ThrowsOnASideByteOutsideTheDocumentedSet) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 1).corrupt_side('Z');

  RecordTradeSource source{b.records()};
  EXPECT_THROW((void)collect(source), ReplayError);
}

TEST(TradeSource, ReturnsNullptrAtACleanEndOfStream) {
  RecordTradeSource source{two_prints().records()};
  EXPECT_NE(source.next(), nullptr);
  EXPECT_NE(source.next(), nullptr);
  EXPECT_EQ(source.next(), nullptr);
  EXPECT_EQ(source.next(), nullptr);
}

TEST(TradeSource, EveryRecordItReadLandsInExactlyOneCounter) {
  TradeStreamBuilder b;
  b.trade(Side::kBid, px(29000, 0), 4);
  b.trade(Side::kAsk, px(29000, 0), 0);
  b.trade(Side::kBid, px(29000, 0), 4).undef_price();
  b.trade(Side::kBid, px(29000, 0), 4).bad_ts_recv();
  b.instrument(999).trade(Side::kBid, px(29000, 0), 4);

  TradeSourceOptions opts;
  opts.instrument_id = 42004177;
  RecordTradeSource source{b.records(), opts};
  (void)collect(source);

  const TradeSourceStats& s = source.stats();
  EXPECT_EQ(s.records, std::uint64_t{5});
  EXPECT_EQ(s.ticks, std::uint64_t{1});
  EXPECT_EQ(s.zero_size, std::uint64_t{1});
  EXPECT_EQ(s.undef_price, std::uint64_t{1});
  EXPECT_EQ(s.bad_ts_recv, std::uint64_t{1});
  EXPECT_EQ(s.skipped_other_instrument, std::uint64_t{1});
  EXPECT_EQ(s.skipped_non_trade, std::uint64_t{0});
  EXPECT_TRUE(s.reconciles());
}

TEST(DbnSource, RefusesAFileWhoseSchemaIsNeitherTradesNorMbo) {
  // Schema 2 is mbp-10.
  const std::vector<std::byte> bytes = testing::DbnEncoder{}.schema(std::uint16_t{2}).encode();
  EXPECT_THROW((DbnTradeSource{bytes.data(), bytes.size()}), ReplayError);
}

TEST(DbnSource, AcceptsTradesMboAndMixedSchemaMetadata) {
  for (const auto schema : {std::optional<std::uint16_t>{0}, std::optional<std::uint16_t>{4},
                            std::optional<std::uint16_t>{}}) {
    const std::vector<std::byte> bytes = testing::DbnEncoder{}.schema(schema).encode();
    EXPECT_NO_THROW((DbnTradeSource{bytes.data(), bytes.size()}));
  }
}

TEST(TickSpanSource, ThrowsOnABackwardTimestampRatherThanReplayingAReorderedBuffer) {
  Tick late{};
  late.ts = 2'000'000'000;
  late.price = px(29000, 0);
  late.size = 1;
  late.aggressor = Side::kBid;
  Tick early = late;
  early.ts = 1'000'000'000;

  const std::vector<Tick> day{late, early};
  TickSpanSource source{day};
  EXPECT_NE(source.next(), nullptr);
  EXPECT_THROW((void)source.next(), ReplayError);
}

TEST(TickSpanSource, ReplaysOneCollectedBufferFromManyIndependentCursors) {
  RecordTradeSource origin{two_prints().records()};
  const std::vector<Tick> day = collect(origin);

  TickSpanSource first{day};
  TickSpanSource second{day};
  EXPECT_EQ(collect(first).size(), day.size());
  EXPECT_EQ(collect(second).size(), day.size());

  second.rewind();
  EXPECT_EQ(collect(second).size(), day.size());
  EXPECT_EQ(second.stats().ticks, std::uint64_t{2});
}

}  // namespace
}  // namespace bookreplay
