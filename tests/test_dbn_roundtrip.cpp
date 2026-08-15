#include "bookreplay/dbn_reader.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dbn_encoder.hpp"

namespace bookreplay {
namespace {

using testing::DbnEncoder;
using testing::EncodedMapping;

MboMsg make_mbo(std::uint64_t order_id, char action, char side, std::int64_t price,
                std::uint32_t size) {
  MboMsg r{};
  r.hd.length = kLengthUnits<MboMsg>;
  r.hd.rtype = kRTypeMbo;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = 42004177;
  r.hd.ts_event = 1'785'888'000'000'000'000ULL + order_id;
  r.order_id = order_id;
  r.price = price;
  r.size = size;
  r.flags = kFlagLast;
  r.channel_id = 3;
  r.action = action;
  r.side = side;
  r.ts_recv = r.hd.ts_event + 500;
  r.ts_in_delta = -250;
  r.sequence = static_cast<std::uint32_t>(order_id);
  return r;
}

TradeMsg make_trade() {
  TradeMsg r{};
  r.hd.length = kLengthUnits<TradeMsg>;
  r.hd.rtype = kRTypeTrade;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = 42004177;
  r.hd.ts_event = 1'785'888'000'000'000'001ULL;
  r.price = 29000LL * kPriceScale;
  r.size = 7;
  r.action = 'T';
  r.side = 'B';
  r.flags = kFlagLast;
  r.depth = 0;
  r.ts_recv = r.hd.ts_event + 900;
  r.ts_in_delta = 1234;
  r.sequence = 99;
  return r;
}

Mbp10Msg make_mbp10() {
  Mbp10Msg r{};
  r.hd.length = kLengthUnits<Mbp10Msg>;
  r.hd.rtype = kRTypeMbp10;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = 42004177;
  r.hd.ts_event = 1'785'888'000'000'000'002ULL;
  r.price = 29000LL * kPriceScale;
  r.size = 3;
  r.action = 'C';
  r.side = 'A';
  r.flags = kFlagLast;
  r.depth = 4;
  r.ts_recv = r.hd.ts_event + 700;
  r.ts_in_delta = 4321;
  r.sequence = 100;
  for (std::size_t i = 0; i < r.levels.size(); ++i) {
    const auto tick = static_cast<std::int64_t>(i) * (kPriceScale / 4);
    r.levels[i].bid_px = 29000LL * kPriceScale - tick;
    r.levels[i].ask_px = 29001LL * kPriceScale + tick;
    r.levels[i].bid_sz = static_cast<std::uint32_t>(10 + i);
    r.levels[i].ask_sz = static_cast<std::uint32_t>(20 + i);
    r.levels[i].bid_ct = static_cast<std::uint32_t>(1 + i);
    r.levels[i].ask_ct = static_cast<std::uint32_t>(2 + i);
  }
  return r;
}

StatusMsg make_status() {
  StatusMsg r{};
  r.hd.length = kLengthUnits<StatusMsg>;
  r.hd.rtype = kRTypeStatus;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = 42004177;
  r.hd.ts_event = 1'785'888'000'000'000'003ULL;
  r.ts_recv = r.hd.ts_event + 100;
  r.action = kStatusActionTrading;
  r.reason = 1;
  r.trading_event = 0;
  r.is_trading = kTriStateYes;
  r.is_quoting = kTriStateYes;
  r.is_short_sell_restricted = kTriStateNotAvailable;
  return r;
}

InstrumentDefMsg make_definition() {
  InstrumentDefMsg r{};
  r.hd.length = kLengthUnits<InstrumentDefMsg>;
  r.hd.rtype = kRTypeInstrumentDef;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = 42004177;
  r.hd.ts_event = 1'785'888'000'000'000'004ULL;
  r.ts_recv = r.hd.ts_event + 10;
  r.min_price_increment = kPriceScale / 4;
  r.display_factor = kPriceScale;
  r.expiration = 1'790'000'000'000'000'000ULL;
  r.activation = 1'780'000'000'000'000'000ULL;
  r.raw_instrument_id = 987654321ULL;
  r.market_depth = 10;
  r.maturity_year = 2026;
  r.leg_count = 0;
  r.tick_rule = 1;
  std::memcpy(r.raw_symbol.data(), "NQU6", 4);
  std::memcpy(r.exchange.data(), "XCME", 4);
  std::memcpy(r.asset.data(), "NQ", 2);
  std::memcpy(r.currency.data(), "USD", 3);
  r.instrument_class = 'F';
  r.match_algorithm = 'F';
  return r;
}

template <typename T>
void expect_same(const T& a, const T& b, const char* what) {
  EXPECT_EQ(std::memcmp(&a, &b, sizeof(T)), 0) << what << " did not round-trip bit-exact";
}

void drain(const std::vector<std::byte>& bytes) {
  DbnReader reader{bytes.data(), bytes.size()};
  while (reader.next() != nullptr) {
  }
}

DbnEncoder every_schema() {
  DbnEncoder enc;
  enc.schema(std::nullopt)
      .symbol("NQ.FUT")
      .mapping(EncodedMapping{"NQU6", 20260805, 20260806, "42004177"})
      .mapping(EncodedMapping{"NQZ6", 20260805, 20260806, "261401"});
  enc.add(make_mbo(100, 'A', 'B', 29000LL * kPriceScale, 5));
  enc.add(make_trade());
  enc.add(make_mbp10());
  enc.add(make_status());
  enc.add(make_definition());
  return enc;
}

TEST(DbnRoundTrip, EverySchemaSurvivesEncodeAndDecodeBitExact) {
  const DbnEncoder enc = every_schema();

  for (const bool compressed : {false, true}) {
    const std::vector<std::byte> bytes = compressed ? enc.encode_compressed() : enc.encode();
    SCOPED_TRACE(compressed ? "zstd-framed" : "uncompressed");

    DbnReader reader{bytes.data(), bytes.size()};

    const RecordHeader* hd = reader.next();
    ASSERT_NE(hd, nullptr);
    const MboMsg* mbo = record_cast<MboMsg>(*hd);
    ASSERT_NE(mbo, nullptr);
    expect_same(*mbo, make_mbo(100, 'A', 'B', 29000LL * kPriceScale, 5), "MboMsg");

    hd = reader.next();
    ASSERT_NE(hd, nullptr);
    const TradeMsg* trade = record_cast<TradeMsg>(*hd);
    ASSERT_NE(trade, nullptr);
    expect_same(*trade, make_trade(), "TradeMsg");

    hd = reader.next();
    ASSERT_NE(hd, nullptr);
    const Mbp10Msg* mbp10 = record_cast<Mbp10Msg>(*hd);
    ASSERT_NE(mbp10, nullptr);
    expect_same(*mbp10, make_mbp10(), "Mbp10Msg");

    hd = reader.next();
    ASSERT_NE(hd, nullptr);
    const StatusMsg* status = record_cast<StatusMsg>(*hd);
    ASSERT_NE(status, nullptr);
    expect_same(*status, make_status(), "StatusMsg");

    hd = reader.next();
    ASSERT_NE(hd, nullptr);
    const InstrumentDefMsg* def = record_cast<InstrumentDefMsg>(*hd);
    ASSERT_NE(def, nullptr);
    expect_same(*def, make_definition(), "InstrumentDefMsg");

    EXPECT_EQ(reader.next(), nullptr);
    EXPECT_EQ(reader.record_count(), 5U);
  }
}

TEST(DbnRoundTrip, MetadataSurvivesIncludingMappings) {
  const std::vector<std::byte> bytes = every_schema().encode_compressed();
  DbnReader reader{bytes.data(), bytes.size()};
  const DbnMetadata& m = reader.metadata();

  EXPECT_EQ(m.version, kSupportedDbnVersion);
  EXPECT_EQ(m.dataset, "GLBX.MDP3");
  EXPECT_FALSE(m.schema.has_value());
  ASSERT_TRUE(m.end.has_value());
  EXPECT_EQ(*m.end, 1'785'974'400'000'000'000ULL);
  EXPECT_FALSE(m.limit.has_value());
  ASSERT_EQ(m.symbols.size(), 1U);
  EXPECT_EQ(m.symbols[0], "NQ.FUT");
  ASSERT_EQ(m.mappings.size(), 2U);
  EXPECT_EQ(m.mappings[0].raw_symbol, "NQU6");
  ASSERT_EQ(m.mappings[0].intervals.size(), 1U);
  EXPECT_EQ(m.mappings[0].intervals[0].symbol, "42004177");
  EXPECT_EQ(m.mappings[1].intervals[0].start_date, 20260805U);
}

TEST(DbnRoundTrip, SentinelEndAndZeroLimitDecodeAsAbsent) {
  DbnEncoder enc;
  enc.window(1'785'888'000'000'000'000ULL, kUndefTimestamp);
  const std::vector<std::byte> bytes = enc.encode();

  DbnReader reader{bytes.data(), bytes.size()};
  EXPECT_FALSE(reader.metadata().end.has_value());
  EXPECT_FALSE(reader.metadata().limit.has_value());
}

TEST(DbnRoundTrip, ARecordLongerThanItsStructIsAcceptedAndSkippedPast) {
  const MboMsg rec = make_mbo(7, 'A', 'B', 100 * kPriceScale, 3);
  std::vector<std::byte> padded(sizeof(MboMsg) + 8, std::byte{0});
  std::memcpy(padded.data(), &rec, sizeof(rec));
  padded[0] = static_cast<std::byte>((sizeof(MboMsg) + 8) / kLengthUnit);

  DbnEncoder enc;
  enc.add_raw(padded);
  enc.add(make_mbo(8, 'C', 'A', 200 * kPriceScale, 1));
  const std::vector<std::byte> bytes = enc.encode();

  DbnReader reader{bytes.data(), bytes.size()};
  const RecordHeader* hd = reader.next();
  ASSERT_NE(hd, nullptr);
  EXPECT_EQ(record_bytes(*hd), sizeof(MboMsg) + 8);
  const MboMsg* first = record_cast<MboMsg>(*hd);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->order_id, 7U);

  hd = reader.next();
  ASSERT_NE(hd, nullptr);
  const MboMsg* second = record_cast<MboMsg>(*hd);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->order_id, 8U);
  EXPECT_EQ(reader.next(), nullptr);
}

TEST(DbnRoundTrip, TsOutExtendedRecordsDecode) {
  DbnEncoder enc;
  enc.ts_out(true, 1'785'888'000'000'000'999ULL);
  enc.add(make_mbo(1, 'A', 'B', 100 * kPriceScale, 1));
  enc.add(make_mbo(2, 'C', 'A', 200 * kPriceScale, 2));
  const std::vector<std::byte> bytes = enc.encode_compressed();

  DbnReader reader{bytes.data(), bytes.size()};
  EXPECT_TRUE(reader.metadata().ts_out);

  const RecordHeader* hd = reader.next();
  ASSERT_NE(hd, nullptr);
  EXPECT_EQ(record_bytes(*hd), sizeof(MboMsg) + sizeof(std::uint64_t));
  const MboMsg* first = record_cast<MboMsg>(*hd);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->order_id, 1U);

  hd = reader.next();
  ASSERT_NE(hd, nullptr);
  const MboMsg* second = record_cast<MboMsg>(*hd);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->order_id, 2U);
  EXPECT_EQ(reader.next(), nullptr);
}

TEST(DbnRoundTrip, ManyRecordsCrossBufferBoundaries) {
  constexpr std::uint64_t kCount = 50'000;
  DbnEncoder enc;
  for (std::uint64_t i = 0; i < kCount; ++i) {
    enc.add(make_mbo(i, 'A', 'B', static_cast<std::int64_t>(i) * kPriceScale,
                     static_cast<std::uint32_t>(i % 97 + 1)));
  }
  const std::vector<std::byte> bytes = enc.encode_compressed();

  DbnReader reader{bytes.data(), bytes.size()};
  std::uint64_t seen = 0;
  while (const RecordHeader* hd = reader.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(rec->order_id, seen);
    ASSERT_EQ(rec->size, seen % 97 + 1);
    ++seen;
  }
  EXPECT_EQ(seen, kCount);
}

// Databento batch files arrive as several concatenated zstd frames, not one.
TEST(DbnRoundTrip, MultipleZstdFramesAreOneLogicalStream) {
  DbnEncoder enc;
  for (std::uint64_t i = 0; i < 40; ++i) {
    enc.add(make_mbo(i, 'A', 'B', static_cast<std::int64_t>(i) * kPriceScale, 1));
  }
  const std::vector<std::byte> plain = enc.encode();

  std::uint32_t meta_size = 0;
  std::memcpy(&meta_size, plain.data() + 4, sizeof(meta_size));
  const std::size_t split = 8 + meta_size + 20 * sizeof(MboMsg);

  const std::vector<std::byte> head{plain.begin(),
                                    plain.begin() + static_cast<std::ptrdiff_t>(split)};
  const std::vector<std::byte> tail{plain.begin() + static_cast<std::ptrdiff_t>(split),
                                    plain.end()};

  std::vector<std::byte> two_frames = DbnEncoder::compress(head);
  const std::vector<std::byte> second = DbnEncoder::compress(tail);
  two_frames.insert(two_frames.end(), second.begin(), second.end());

  DbnReader reader{two_frames.data(), two_frames.size()};
  std::uint64_t seen = 0;
  while (const RecordHeader* hd = reader.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(rec->order_id, seen);
    ++seen;
  }
  EXPECT_EQ(seen, 40U);
}

template <typename T>
T random_record(std::mt19937_64& rng) {
  std::array<std::byte, sizeof(T)> raw{};
  for (std::byte& b : raw) {
    b = static_cast<std::byte>(rng() & 0xFFU);
  }
  T rec{};
  std::memcpy(&rec, raw.data(), sizeof(T));
  rec.hd.length = kLengthUnits<T>;
  rec.hd.rtype = RecordTraits<T>::kRType;
  return rec;
}

template <typename T>
void add_random(DbnEncoder& enc, std::mt19937_64& rng,
                std::vector<std::vector<std::byte>>& expected) {
  const T rec = random_record<T>(rng);
  enc.add(rec);
  const auto* first = reinterpret_cast<const std::byte*>(&rec);
  expected.emplace_back(first, first + sizeof(T));
}

TEST(DbnRoundTrip, RandomlyValuedRecordsSurviveBitExact) {
  std::mt19937_64 rng{0xB00C5EEDULL};

  DbnEncoder enc;
  enc.schema(std::nullopt);
  std::vector<std::vector<std::byte>> expected;
  constexpr int kRounds = 200;
  for (int i = 0; i < kRounds; ++i) {
    add_random<MboMsg>(enc, rng, expected);
    add_random<TradeMsg>(enc, rng, expected);
    add_random<Mbp10Msg>(enc, rng, expected);
    add_random<StatusMsg>(enc, rng, expected);
    add_random<InstrumentDefMsg>(enc, rng, expected);
  }

  const std::vector<std::byte> bytes = enc.encode_compressed();
  DbnReader reader{bytes.data(), bytes.size()};

  std::size_t i = 0;
  while (const RecordHeader* hd = reader.next()) {
    ASSERT_LT(i, expected.size());
    ASSERT_EQ(record_bytes(*hd), expected[i].size()) << "record " << i;
    EXPECT_EQ(std::memcmp(hd, expected[i].data(), expected[i].size()), 0) << "record " << i;
    ++i;
  }
  EXPECT_EQ(i, expected.size());
}

std::vector<std::byte> raw_bytes(std::uint8_t length_units, std::uint8_t rtype, std::size_t total) {
  std::vector<std::byte> bytes(total, std::byte{0});
  bytes[0] = static_cast<std::byte>(length_units);
  bytes[1] = static_cast<std::byte>(rtype);
  return bytes;
}

TEST(DbnMalformed, EmptyInputIsRejected) {
  const std::vector<std::byte> empty;
  EXPECT_THROW(DbnReader(empty.data(), empty.size()), DbnError);
}

TEST(DbnMalformed, NullMemoryWithANonzeroSizeIsRejected) {
  EXPECT_THROW(DbnReader(nullptr, 100), DbnError);
}

TEST(DbnMalformed, AMetadataStringWithoutItsTerminatorIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode();
  // The 16-byte dataset field sits right after the 8-byte prelude.
  std::fill_n(bytes.begin() + 8, 16, static_cast<std::byte>('A'));
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, ANonzeroSchemaDefinitionLengthIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode();
  // schema_definition_length sits directly after the 100-byte fixed block.
  const std::uint32_t nonzero = 8;
  std::memcpy(bytes.data() + 8 + 100, &nonzero, sizeof(nonzero));
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, InputShorterThanTheMagicIsRejected) {
  const std::vector<std::byte> bytes(3, std::byte{0});
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, WrongMagicIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode();
  bytes[0] = static_cast<std::byte>('X');
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, LegacyDbzIsNamedRatherThanGuessedAt) {
  // A zstd skippable frame, which is what the retired DBZ container used.
  std::vector<std::byte> bytes(64, std::byte{0});
  const std::uint32_t skippable = 0x184D2A50U;
  std::memcpy(bytes.data(), &skippable, sizeof(skippable));
  try {
    DbnReader reader(bytes.data(), bytes.size());
    FAIL() << "expected a DbnError";
  } catch (const DbnError& e) {
    EXPECT_NE(std::string{e.what()}.find("DBZ"), std::string::npos) << e.what();
  }
}

TEST(DbnMalformed, UnsupportedVersionsAreRejected) {
  for (const std::uint8_t version :
       {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{4}, std::uint8_t{255}}) {
    std::vector<std::byte> bytes = every_schema().encode();
    bytes[3] = static_cast<std::byte>(version);
    EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError)
        << "version " << static_cast<int>(version) << " was accepted";
  }
}

TEST(DbnMalformed, MetadataFrameShorterThanTheFixedBlockIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode();
  const std::uint32_t too_small = 99;
  std::memcpy(bytes.data() + 4, &too_small, sizeof(too_small));
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, TruncatedMetadataIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode();
  bytes.resize(40);
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, AnOversizedSymbolCountIsRejectedRatherThanAllocated) {
  std::vector<std::byte> bytes = every_schema().encode();
  // The symbol count sits directly after the 100-byte fixed block and the
  // 4-byte schema_definition_length.
  const std::uint32_t absurd = 0xFFFFFFFFU;
  std::memcpy(bytes.data() + 8 + 100 + 4, &absurd, sizeof(absurd));
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, ASymbolWidthOtherThanTheVersionsIsRejected) {
  std::vector<std::byte> bytes = DbnEncoder{}.symbol_cstr_len(22).encode();
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, UnknownRtypeIsRejected) {
  DbnEncoder enc;
  enc.add_raw(raw_bytes(14, 0x99, sizeof(MboMsg)));
  const std::vector<std::byte> bytes = enc.encode();
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, RecordShorterThanTheHeaderIsRejected) {
  DbnEncoder enc;
  enc.add_raw(raw_bytes(2, kRTypeMbo, sizeof(MboMsg)));
  const std::vector<std::byte> bytes = enc.encode();
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, RecordLengthNotAMultipleOfEightIsRejected) {
  DbnEncoder enc;
  enc.add_raw(raw_bytes(15, kRTypeMbo, 60));
  const std::vector<std::byte> bytes = enc.encode();
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, RecordShorterThanItsOwnStructIsRejected) {
  DbnEncoder enc;
  enc.add_raw(raw_bytes(6, kRTypeMbo, 24));
  const std::vector<std::byte> bytes = enc.encode();
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, ATruncatedFinalRecordIsRejectedNotSilentlyDropped) {
  std::vector<std::byte> bytes = every_schema().encode();
  bytes.resize(bytes.size() - 8);
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, CorruptZstdPayloadIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode_compressed(/*checksum=*/true);
  ASSERT_GT(bytes.size(), 40U);
  bytes[bytes.size() / 2] ^= std::byte{0xFF};
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, GarbageBehindTheZstdMagicIsRejected) {
  std::vector<std::byte> bytes(256, std::byte{0xFF});
  const std::uint32_t magic = 0xFD2FB528U;
  std::memcpy(bytes.data(), &magic, sizeof(magic));
  EXPECT_THROW(drain(bytes), DbnError);
}

TEST(DbnMalformed, SingleBitFlipsAreEitherReadOrRefusedNeverAnythingElse) {
  const std::vector<std::byte> good = every_schema().encode();
  std::mt19937_64 rng{0x5EEDCAFEULL};

  for (int i = 0; i < 4000; ++i) {
    std::vector<std::byte> bytes = good;
    const std::size_t pos = static_cast<std::size_t>(rng()) % bytes.size();
    const auto bit = static_cast<unsigned>(rng() % 8);
    bytes[pos] ^= static_cast<std::byte>(1U << bit);
    try {
      drain(bytes);
    } catch (const DbnError&) {
    }
  }
}

TEST(DbnMalformed, AnAbsurdMetadataLengthIsRefusedNotAllocated) {
  std::vector<std::byte> bytes = every_schema().encode();
  const std::uint32_t absurd = 0xFFFFFFFFU;
  std::memcpy(bytes.data() + 4, &absurd, sizeof(absurd));
  EXPECT_THROW(DbnReader(bytes.data(), bytes.size()), DbnError);
}

TEST(DbnMalformed, AZstdStreamCutMidFrameIsRejected) {
  std::vector<std::byte> bytes = every_schema().encode_compressed();
  bytes.resize(bytes.size() - 12);
  EXPECT_THROW(drain(bytes), DbnError);
}

}  // namespace
}  // namespace bookreplay
