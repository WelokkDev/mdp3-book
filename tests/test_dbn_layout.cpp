// The wire layout is the one assumption everything else rests on. It was
// verified empirically against a real GLBX.MDP3 file (8/8 records identical to
// Databento's own decoder); these tests keep it verified.

#include "bookreplay/dbn.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <gtest/gtest.h>

namespace bookreplay {
namespace {

static_assert(std::endian::native == std::endian::little,
              "DBN is little-endian on the wire; a big-endian host needs byte swaps that "
              "this decoder does not implement");

template <typename T>
void put(std::array<std::uint8_t, sizeof(MboMsg)>& buf, std::size_t offset, T value) {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(buf.data() + offset, &value, sizeof(T));
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

}  // namespace
}  // namespace bookreplay
