// The wire layout is the one assumption everything else rests on. It was
// verified empirically against a real GLBX.MDP3 file (8/8 records identical to
// Databento's own decoder); these tests keep it verified.

#include "bookreplay/dbn.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

namespace bookreplay {
namespace {

static_assert(std::endian::native == std::endian::little,
              "DBN is little-endian on the wire; a big-endian host needs byte swaps that "
              "this decoder does not implement");

template <typename T, std::size_t N>
void put(std::array<std::uint8_t, N>& buf, std::size_t offset, T value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(buf.data() + offset, &value, sizeof(T));
}

template <std::size_t N>
void put_str(std::array<std::uint8_t, N>& buf, std::size_t offset, std::string_view value) {
  std::memcpy(buf.data() + offset, value.data(), value.size());
}

TEST(DbnLayout, FieldsDecodeFromRawBytesAtTheDocumentedOffsets) {
  std::array<std::uint8_t, sizeof(MboMsg)> raw{};
  put<std::uint8_t>(raw, 0, kMboLengthUnits);
  put<std::uint8_t>(raw, 1, kRTypeMbo);
  put<std::uint16_t>(raw, 2, 1);
  put<std::uint32_t>(raw, 4, 42004177);
  put<std::uint64_t>(raw, 8, 1785672006253206641ULL);
  put<std::uint64_t>(raw, 16, 987654321ULL);
  put<std::int64_t>(raw, 24, 29000LL * kPriceScale);
  put<std::uint32_t>(raw, 32, 17);
  put<std::uint8_t>(raw, 36, kFlagLast | kFlagTob);
  put<std::uint8_t>(raw, 37, 3);
  put<char>(raw, 38, 'A');
  put<char>(raw, 39, 'B');
  put<std::uint64_t>(raw, 40, 1785888000000000000ULL);
  put<std::int32_t>(raw, 48, -250);
  put<std::uint32_t>(raw, 52, 4242);

  MboMsg rec{};
  std::memcpy(&rec, raw.data(), sizeof(rec));

  EXPECT_EQ(rec.hd.length, kMboLengthUnits);
  EXPECT_EQ(rec.hd.rtype, kRTypeMbo);
  EXPECT_EQ(rec.hd.publisher_id, std::uint16_t{1});
  EXPECT_EQ(rec.hd.instrument_id, std::uint32_t{42004177});
  EXPECT_EQ(rec.hd.ts_event, 1785672006253206641ULL);
  EXPECT_EQ(rec.order_id, 987654321ULL);
  EXPECT_EQ(rec.price, 29000LL * kPriceScale);
  EXPECT_EQ(rec.size, std::uint32_t{17});
  EXPECT_EQ(rec.channel_id, std::uint8_t{3});
  EXPECT_EQ(rec.action, 'A');
  EXPECT_EQ(rec.side, 'B');
  EXPECT_EQ(rec.ts_recv, 1785888000000000000ULL);
  EXPECT_EQ(rec.ts_in_delta, -250);
  EXPECT_EQ(rec.sequence, std::uint32_t{4242});

  EXPECT_TRUE(has_flag(rec, kFlagLast));
  EXPECT_TRUE(has_flag(rec, kFlagTob));
  EXPECT_FALSE(has_flag(rec, kFlagSnapshot));
  EXPECT_TRUE(is_event_boundary(rec));
  EXPECT_EQ(action_of(rec), Action::kAdd);
  EXPECT_EQ(side_of(rec), Side::kBid);
}

// The leg fields are what the instrument catalog keys on, and every one of
// them is zero in the committed fixture, so nothing else in the suite would
// notice them trading places. Built from bytes rather than from the struct,
// with no two values alike.
TEST(DbnLayout, LegFieldsDecodeFromRawBytesAtTheDocumentedOffsets) {
  std::array<std::uint8_t, sizeof(InstrumentDefMsg)> raw{};
  put<std::uint8_t>(raw, 0, kLengthUnits<InstrumentDefMsg>);
  put<std::uint8_t>(raw, 1, kRTypeInstrumentDef);
  put<std::uint32_t>(raw, 4, 42037493);
  put<std::int64_t>(raw, 24, 50'000'000);
  put<std::int64_t>(raw, 120, 7LL * kPriceScale);
  put<std::uint32_t>(raw, 188, 261401);
  put<std::uint32_t>(raw, 208, 42004177);
  put<std::uint16_t>(raw, 220, 2);
  put<std::uint16_t>(raw, 222, 1);
  put_str(raw, 238, "NQZ6-NQM7");
  put_str(raw, 416, "NQM7");
  put<char>(raw, 487, 'S');
  put<char>(raw, 501, 'F');
  put<char>(raw, 502, 'B');

  InstrumentDefMsg rec{};
  std::memcpy(&rec, raw.data(), sizeof(rec));

  EXPECT_EQ(rec.hd.rtype, kRTypeInstrumentDef);
  EXPECT_EQ(rec.hd.instrument_id, std::uint32_t{42037493});
  EXPECT_EQ(rec.min_price_increment, 50'000'000);

  EXPECT_EQ(rec.leg_price, 7LL * kPriceScale);
  EXPECT_EQ(rec.leg_instrument_id, std::uint32_t{261401});
  EXPECT_EQ(rec.leg_underlying_id, std::uint32_t{42004177});
  EXPECT_EQ(rec.leg_count, std::uint16_t{2});
  EXPECT_EQ(rec.leg_index, std::uint16_t{1});
  EXPECT_EQ(rec.leg_instrument_class, 'F');
  EXPECT_EQ(rec.leg_side, 'B');
  EXPECT_EQ(cstr_view(rec.leg_raw_symbol), "NQM7");

  EXPECT_EQ(cstr_view(rec.raw_symbol), "NQZ6-NQM7");
  EXPECT_EQ(instrument_class_of(rec), InstrumentClass::kFutureSpread);
  EXPECT_TRUE(is_spread(instrument_class_of(rec)));
}

// Record #1 of glbx-mdp3-20260805.mbo.dbn.zst, field for field.
TEST(DbnLayout, RealSnapshotClearRecordIsAcceptedNotValidatedAsAQuantity) {
  MboMsg rec{};
  rec.hd.length = kMboLengthUnits;
  rec.hd.rtype = kRTypeMbo;
  rec.hd.instrument_id = 261401;
  rec.hd.ts_event = 1785672006253206641ULL;
  rec.order_id = 0;
  rec.price = kUndefPrice;
  rec.size = 0;
  rec.flags = 40;
  rec.action = 'R';
  rec.side = 'N';
  rec.ts_recv = 1785888000000000000ULL;

  EXPECT_EQ(action_of(rec), Action::kClear);
  EXPECT_EQ(side_of(rec), Side::kNone);
  EXPECT_TRUE(is_undef_price(rec.price));
  EXPECT_TRUE(mutates_book(action_of(rec)));

  // flags == 40 decomposes to exactly these two and nothing else.
  EXPECT_TRUE(has_flag(rec, kFlagSnapshot));
  EXPECT_TRUE(has_flag(rec, kFlagBadTsRecv));
  EXPECT_FALSE(has_flag(rec, kFlagLast));
  EXPECT_FALSE(has_flag(rec, kFlagTob));
  EXPECT_FALSE(has_flag(rec, kFlagMbp));
  EXPECT_FALSE(has_flag(rec, kFlagMaybeBadBook));

  // The snapshot replays orders with their original entry time.
  EXPECT_LT(rec.hd.ts_event, rec.ts_recv);
}

TEST(DbnLayout, UndefPriceWouldReadAsTheBestPossibleAskIfUnguarded) {
  EXPECT_GT(kUndefPrice, 1'000'000LL * kPriceScale);
  EXPECT_TRUE(is_undef_price(kUndefPrice));
  EXPECT_FALSE(is_undef_price(0));
  EXPECT_FALSE(is_undef_price(-42));  // negative prices are structurally legal
}

TEST(ActionClassification, OnlyAddCancelModifyClearMutateTheBook) {
  EXPECT_TRUE(mutates_book(Action::kAdd));
  EXPECT_TRUE(mutates_book(Action::kCancel));
  EXPECT_TRUE(mutates_book(Action::kModify));
  EXPECT_TRUE(mutates_book(Action::kClear));

  // The three that every instinct says should mutate, and must not.
  EXPECT_FALSE(mutates_book(Action::kTrade));
  EXPECT_FALSE(mutates_book(Action::kFill));
  EXPECT_FALSE(mutates_book(Action::kNone));
}

TEST(ActionClassification, UnknownActionCharsAreRejected) {
  EXPECT_TRUE(is_known_action('A'));
  EXPECT_TRUE(is_known_action('R'));
  EXPECT_FALSE(is_known_action('X'));
  EXPECT_FALSE(is_known_action('\0'));
  EXPECT_FALSE(is_known_action('a'));

  EXPECT_TRUE(is_known_side('B'));
  EXPECT_FALSE(is_known_side('Z'));
}

TEST(InstrumentClassification, OnlyMixedFutureAndOptionSpreadsAreSpreads) {
  EXPECT_TRUE(is_spread(InstrumentClass::kMixedSpread));
  EXPECT_TRUE(is_spread(InstrumentClass::kFutureSpread));
  EXPECT_TRUE(is_spread(InstrumentClass::kOptionSpread));

  EXPECT_FALSE(is_spread(InstrumentClass::kBond));
  EXPECT_FALSE(is_spread(InstrumentClass::kCall));
  EXPECT_FALSE(is_spread(InstrumentClass::kFuture));
  EXPECT_FALSE(is_spread(InstrumentClass::kIndex));
  EXPECT_FALSE(is_spread(InstrumentClass::kStock));
  EXPECT_FALSE(is_spread(InstrumentClass::kPut));
  EXPECT_FALSE(is_spread(InstrumentClass::kFxSpot));
  EXPECT_FALSE(is_spread(InstrumentClass::kCommoditySpot));
}

TEST(InstrumentClassification, OnlyTheDocumentedClassBytesAreKnown) {
  for (const char c : {'B', 'C', 'F', 'I', 'K', 'M', 'P', 'S', 'T', 'X', 'Y'}) {
    EXPECT_TRUE(is_known_instrument_class(c)) << c;
  }
  for (const char c : {'\0', 'A', 'N', 'Z', 'f', '~'}) {
    EXPECT_FALSE(is_known_instrument_class(c)) << static_cast<int>(c);
  }
}

TEST(FixedWidthString, ViewsUpToTheFirstNulAndNeverPastTheField) {
  constexpr std::array<char, 8> padded{'N', 'Q', 'U', '6', '\0', 'Z', 'Z', 'Z'};
  constexpr std::array<char, 4> full{'N', 'Q', 'U', '6'};
  constexpr std::array<char, 4> empty{};

  EXPECT_EQ(cstr_view(padded), "NQU6");
  EXPECT_EQ(cstr_view(full), "NQU6");
  EXPECT_TRUE(cstr_view(empty).empty());
}

TEST(DbnVersionGate, AcceptsOnlyTheTestedVersion) {
  EXPECT_TRUE(is_supported_dbn_version(3));
  EXPECT_FALSE(is_supported_dbn_version(2));
  EXPECT_FALSE(is_supported_dbn_version(4));
}

TEST(Naming, EveryActionAndSideHasAName) {
  EXPECT_STREQ(action_name(Action::kAdd), "Add");
  EXPECT_STREQ(action_name(Action::kCancel), "Cancel");
  EXPECT_STREQ(action_name(Action::kModify), "Modify");
  EXPECT_STREQ(action_name(Action::kTrade), "Trade");
  EXPECT_STREQ(action_name(Action::kFill), "Fill");
  EXPECT_STREQ(action_name(Action::kNone), "None");
  EXPECT_STREQ(action_name(Action::kClear), "Clear");

  EXPECT_STREQ(side_name(Side::kBid), "Bid");
  EXPECT_STREQ(side_name(Side::kAsk), "Ask");
  EXPECT_STREQ(side_name(Side::kNone), "None");
}

TEST(Naming, EveryInstrumentClassHasAName) {
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kBond), "Bond");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kCall), "Call");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kFuture), "Future");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kIndex), "Index");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kStock), "Stock");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kMixedSpread), "MixedSpread");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kPut), "Put");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kFutureSpread), "FutureSpread");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kOptionSpread), "OptionSpread");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kFxSpot), "FxSpot");
  EXPECT_STREQ(instrument_class_name(InstrumentClass::kCommoditySpot), "CommoditySpot");
}

}  // namespace
}  // namespace bookreplay
