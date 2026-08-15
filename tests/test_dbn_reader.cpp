// The expected values are Databento's own v0.66.0 test vectors (tests/data),
// read back with their reference decoder.

#include "bookreplay/dbn_reader.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace bookreplay {
namespace {

std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path{BOOKREPLAY_TEST_DATA_DIR} / name;
}

template <typename T>
std::vector<T> read_all(DbnReader& reader) {
  std::vector<T> out;
  while (const RecordHeader* hd = reader.next()) {
    const T* rec = record_cast<T>(*hd);
    EXPECT_NE(rec, nullptr) << "unexpected rtype " << static_cast<int>(hd->rtype);
    if (rec != nullptr) {
      out.push_back(*rec);
    }
  }
  return out;
}

TEST(DbnReaderMetadata, DecodesEveryHeaderFieldIncludingSymbolMappings) {
  DbnReader reader{fixture("test_data.mbo.v3.dbn.zst")};
  const DbnMetadata& m = reader.metadata();

  EXPECT_EQ(m.version, kSupportedDbnVersion);
  EXPECT_EQ(m.dataset, "GLBX.MDP3");
  ASSERT_TRUE(m.schema.has_value());
  EXPECT_EQ(*m.schema, 0);  // mbo
  EXPECT_EQ(m.start, 1609160400000000000ULL);
  ASSERT_TRUE(m.end.has_value());
  EXPECT_EQ(*m.end, 1609200000000000000ULL);
  ASSERT_TRUE(m.limit.has_value());
  EXPECT_EQ(*m.limit, 2ULL);
  EXPECT_TRUE(m.stype_in.has_value());
  EXPECT_FALSE(m.ts_out);
  EXPECT_EQ(m.symbol_cstr_len, kSymbolCstrLen);

  ASSERT_EQ(m.symbols.size(), 1U);
  EXPECT_EQ(m.symbols.front(), "ESH1");
  EXPECT_TRUE(m.partial.empty());
  EXPECT_TRUE(m.not_found.empty());

  ASSERT_EQ(m.mappings.size(), 1U);
  EXPECT_EQ(m.mappings[0].raw_symbol, "ESH1");
  ASSERT_EQ(m.mappings[0].intervals.size(), 1U);
  EXPECT_EQ(m.mappings[0].intervals[0].start_date, 20201228U);
  EXPECT_EQ(m.mappings[0].intervals[0].end_date, 20201229U);
  EXPECT_EQ(m.mappings[0].intervals[0].symbol, "5482");
}

TEST(DbnReaderMbo, DecodesEveryFieldOfEveryRecord) {
  DbnReader reader{fixture("test_data.mbo.v3.dbn.zst")};
  const std::vector<MboMsg> recs = read_all<MboMsg>(reader);
  ASSERT_EQ(recs.size(), 2U);
  EXPECT_EQ(reader.record_count(), 2U);

  const MboMsg& a = recs[0];
  EXPECT_EQ(a.hd.length, kLengthUnits<MboMsg>);
  EXPECT_EQ(a.hd.rtype, kRTypeMbo);
  EXPECT_EQ(a.hd.publisher_id, 1U);
  EXPECT_EQ(a.hd.instrument_id, 5482U);
  EXPECT_EQ(a.hd.ts_event, 1609160400000429831ULL);
  EXPECT_EQ(a.order_id, 647784973705ULL);
  EXPECT_EQ(a.price, 3722750000000LL);
  EXPECT_EQ(a.size, 1U);
  EXPECT_EQ(a.flags, kFlagLast);
  EXPECT_EQ(a.channel_id, 0U);
  EXPECT_EQ(action_of(a), Action::kCancel);
  EXPECT_EQ(side_of(a), Side::kAsk);
  EXPECT_EQ(a.ts_recv, 1609160400000704060ULL);
  EXPECT_EQ(a.ts_in_delta, 22993);
  EXPECT_EQ(a.sequence, 1170352U);
  EXPECT_TRUE(is_event_boundary(a));

  const MboMsg& b = recs[1];
  EXPECT_EQ(b.hd.ts_event, 1609160400000431665ULL);
  EXPECT_EQ(b.order_id, 647784973631ULL);
  EXPECT_EQ(b.price, 3723000000000LL);
  EXPECT_EQ(b.ts_recv, 1609160400000711344ULL);
  EXPECT_EQ(b.ts_in_delta, 19621);
  EXPECT_EQ(b.sequence, 1170353U);
}

TEST(DbnReaderMbo, UncompressedFileDecodesToTheSameRecords) {
  DbnReader zstd{fixture("test_data.mbo.v3.dbn.zst")};
  DbnReader plain{fixture("test_data.mbo.v3.dbn")};
  const std::vector<MboMsg> from_zstd = read_all<MboMsg>(zstd);
  const std::vector<MboMsg> from_plain = read_all<MboMsg>(plain);

  ASSERT_EQ(from_zstd.size(), from_plain.size());
  for (std::size_t i = 0; i < from_zstd.size(); ++i) {
    EXPECT_EQ(std::memcmp(&from_zstd[i], &from_plain[i], sizeof(MboMsg)), 0) << "record " << i;
  }
}

TEST(DbnReaderTrades, DecodesTradePrints) {
  DbnReader reader{fixture("test_data.trades.v3.dbn.zst")};
  const std::vector<TradeMsg> recs = read_all<TradeMsg>(reader);
  ASSERT_EQ(recs.size(), 2U);

  const TradeMsg& a = recs[0];
  EXPECT_EQ(a.hd.rtype, kRTypeTrade);
  EXPECT_EQ(a.hd.instrument_id, 5482U);
  EXPECT_EQ(a.hd.ts_event, 1609160400098821953ULL);
  EXPECT_EQ(a.price, 3720250000000LL);
  EXPECT_EQ(a.size, 5U);
  EXPECT_EQ(a.action, 'T');
  EXPECT_EQ(a.side, 'A');
  EXPECT_EQ(a.flags, 129U);
  EXPECT_EQ(a.depth, 0U);
  EXPECT_EQ(a.ts_recv, 1609160400099150057ULL);
  EXPECT_EQ(a.ts_in_delta, 19251);
  EXPECT_EQ(a.sequence, 1170380U);

  EXPECT_EQ(recs[1].size, 21U);
  EXPECT_EQ(recs[1].sequence, 1170414U);
  EXPECT_EQ(recs[1].ts_recv, 1609160400108142648ULL);
}

TEST(DbnReaderMbp10, DecodesAllTenLevels) {
  DbnReader reader{fixture("test_data.mbp-10.v3.dbn.zst")};
  const std::vector<Mbp10Msg> recs = read_all<Mbp10Msg>(reader);
  ASSERT_EQ(recs.size(), 2U);

  const Mbp10Msg& a = recs[0];
  EXPECT_EQ(a.hd.rtype, kRTypeMbp10);
  EXPECT_EQ(a.hd.instrument_id, 5482U);
  EXPECT_EQ(a.action, 'C');
  EXPECT_EQ(a.side, 'A');
  EXPECT_EQ(a.price, 3722750000000LL);
  EXPECT_EQ(a.size, 1U);
  EXPECT_EQ(a.depth, 9U);
  EXPECT_EQ(a.flags, kFlagLast);
  EXPECT_EQ(a.sequence, 1170352U);
  EXPECT_EQ(a.ts_recv, 1609160400000704060ULL);
  EXPECT_EQ(a.ts_in_delta, 22993);

  EXPECT_EQ(a.levels[0].bid_px, 3720250000000LL);
  EXPECT_EQ(a.levels[0].ask_px, 3720500000000LL);
  EXPECT_EQ(a.levels[0].bid_sz, 24U);
  EXPECT_EQ(a.levels[0].ask_sz, 10U);
  EXPECT_EQ(a.levels[0].bid_ct, 15U);
  EXPECT_EQ(a.levels[0].ask_ct, 8U);
  EXPECT_EQ(a.levels[9].bid_px, 3718000000000LL);
  EXPECT_EQ(a.levels[9].ask_px, 3722750000000LL);
  EXPECT_EQ(a.levels[9].bid_sz, 67U);
  EXPECT_EQ(a.levels[9].ask_sz, 44U);

  EXPECT_EQ(recs[1].side, 'B');
  EXPECT_EQ(recs[1].depth, 1U);
  EXPECT_EQ(recs[1].sequence, 1170356U);
}

TEST(DbnReaderStatus, DecodesTheSessionTransitions) {
  DbnReader reader{fixture("test_data.status.v3.dbn.zst")};
  const std::vector<StatusMsg> recs = read_all<StatusMsg>(reader);
  ASSERT_EQ(recs.size(), 4U);

  const StatusMsg& a = recs[0];
  EXPECT_EQ(a.hd.rtype, kRTypeStatus);
  EXPECT_EQ(a.hd.instrument_id, 5482U);
  EXPECT_EQ(a.hd.ts_event, 1609110000000000000ULL);
  EXPECT_EQ(a.ts_recv, 1609113600000000000ULL);
  EXPECT_EQ(a.action, kStatusActionTrading);
  EXPECT_EQ(a.reason, 1U);  // scheduled
  EXPECT_EQ(a.trading_event, 0U);
  EXPECT_EQ(a.is_trading, kTriStateYes);
  EXPECT_EQ(a.is_quoting, kTriStateYes);
  EXPECT_EQ(a.is_short_sell_restricted, kTriStateNotAvailable);

  EXPECT_EQ(recs[1].action, 1U);  // pre-open
  EXPECT_EQ(recs[1].is_trading, kTriStateNo);
  EXPECT_EQ(recs[2].trading_event, 1U);  // no-cancel
  EXPECT_EQ(recs[3].action, 6U);         // new price indication
  EXPECT_EQ(recs[3].is_trading, kTriStateYes);
}

TEST(DbnReaderDefinition, DecodesTheFiveHundredAndTwentyByteRecord) {
  DbnReader reader{fixture("test_data.definition.v3.dbn.zst")};
  const std::vector<InstrumentDefMsg> recs = read_all<InstrumentDefMsg>(reader);
  ASSERT_EQ(recs.size(), 2U);

  const InstrumentDefMsg& a = recs[0];
  EXPECT_EQ(a.hd.rtype, kRTypeInstrumentDef);
  EXPECT_EQ(a.hd.length, kLengthUnits<InstrumentDefMsg>);
  EXPECT_EQ(a.hd.instrument_id, 6819U);
  EXPECT_EQ(a.ts_recv, 1633331241618029519ULL);
  EXPECT_EQ(a.display_factor, 100000000000000LL);
  EXPECT_EQ(a.min_price_increment, kUndefPrice);
  EXPECT_EQ(a.strike_price, kUndefPrice);
  EXPECT_EQ(a.expiration, kUndefTimestamp);
  EXPECT_EQ(a.activation, kUndefTimestamp);
  EXPECT_EQ(a.raw_instrument_id, 2147483647ULL);
  EXPECT_EQ(a.market_depth, 2147483647);
  EXPECT_EQ(a.maturity_year, 65535U);
  EXPECT_EQ(a.leg_count, 0U);
  EXPECT_EQ(a.tick_rule, 255U);
  EXPECT_EQ(a.channel_id, 0U);

  EXPECT_STREQ(a.raw_symbol.data(), "MSFT");
  EXPECT_STREQ(a.exchange.data(), "XNAS");
  EXPECT_STREQ(a.group.data(), "pxnas-1");
  EXPECT_STREQ(a.asset.data(), "");
  EXPECT_EQ(a.instrument_class, 'K');
  EXPECT_EQ(a.match_algorithm, 'F');

  EXPECT_EQ(recs[1].hd.instrument_id, 6830U);
  EXPECT_EQ(recs[1].ts_recv, 1633417621703120931ULL);
  EXPECT_STREQ(recs[1].raw_symbol.data(), "MSFT");
}

class DbnVersionGate : public ::testing::TestWithParam<const char*> {};

TEST_P(DbnVersionGate, OlderVersionsAreRefusedNotConverted) {
  EXPECT_THROW(DbnReader{fixture(GetParam())}, DbnError);
}

INSTANTIATE_TEST_SUITE_P(
    OlderFixtures, DbnVersionGate,
    ::testing::Values("test_data.mbo.v1.dbn.zst", "test_data.mbo.v2.dbn.zst",
                      "test_data.trades.v1.dbn.zst", "test_data.trades.v2.dbn.zst",
                      "test_data.mbp-10.v1.dbn.zst", "test_data.mbp-10.v2.dbn.zst",
                      "test_data.status.v2.dbn.zst", "test_data.definition.v1.dbn.zst",
                      "test_data.definition.v2.dbn.zst"));

TEST(DbnVersionGateMessage, NamesTheVersionItFound) {
  try {
    DbnReader reader{fixture("test_data.mbo.v1.dbn.zst")};
    FAIL() << "expected a DbnError";
  } catch (const DbnError& e) {
    const std::string msg = e.what();
    EXPECT_NE(msg.find("version 1"), std::string::npos) << msg;
    EXPECT_NE(msg.find("version 3"), std::string::npos) << msg;
  }
}

TEST(DbnReaderErrors, MissingFileFailsLoudly) {
  EXPECT_THROW(DbnReader{fixture("does_not_exist.dbn.zst")}, DbnError);
}

}  // namespace
}  // namespace bookreplay
