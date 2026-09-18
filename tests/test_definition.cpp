#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/definition.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "dbn_encoder.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::kBaseTs;
using testing::kDecemberExpiry;
using testing::kSeptemberExpiry;
using testing::kSpreadTick;
using testing::kTick;
using testing::outright;
using testing::spread_leg;

/// Instrument ids from the Aug 2026 NQ.FUT definition pull.
constexpr std::uint32_t kNqu6 = 42004177;
constexpr std::uint32_t kNqz6 = 261401;
constexpr std::uint32_t kNqh7 = 42005205;
constexpr std::uint32_t kNqu6Nqz6 = 42013467;

constexpr std::uint64_t kDay = 86'400'000'000'000ULL;

std::filesystem::path fixture(const std::string& name) {
  return std::filesystem::path{BOOKREPLAY_TEST_DATA_DIR} / name;
}

/// Every spread in the pull lists its nearby leg first and on the ask side:
/// buying the calendar sells the nearby and buys the deferred.
InstrumentDefMsg nearby_leg() {
  return spread_leg(kNqu6Nqz6, "NQU6-NQZ6", 0, kNqu6, "NQU6", Side::kAsk);
}

InstrumentDefMsg deferred_leg() {
  return spread_leg(kNqu6Nqz6, "NQU6-NQZ6", 1, kNqz6, "NQZ6", Side::kBid);
}

void expect_nqu6_nqz6_legs(const InstrumentDefinition& spread) {
  EXPECT_TRUE(spread.has_all_legs());
  ASSERT_EQ(spread.legs.size(), 2U);
  EXPECT_EQ(spread.legs[0].index, 0U);
  EXPECT_EQ(spread.legs[0].instrument_id, kNqu6);
  EXPECT_EQ(spread.legs[0].side, Side::kAsk);
  EXPECT_EQ(spread.legs[0].raw_symbol, "NQU6");
  EXPECT_EQ(spread.legs[1].index, 1U);
  EXPECT_EQ(spread.legs[1].instrument_id, kNqz6);
  EXPECT_EQ(spread.legs[1].side, Side::kBid);
  EXPECT_EQ(spread.legs[1].raw_symbol, "NQZ6");
}

DbnMetadata window(std::optional<std::uint16_t> schema, std::uint64_t start,
                   std::optional<std::uint64_t> end, std::string dataset = "GLBX.MDP3") {
  DbnMetadata meta{};
  meta.dataset = std::move(dataset);
  meta.schema = schema;
  meta.start = start;
  meta.end = end;
  return meta;
}

TEST(SpreadLegs, TwoLegRecordsUnderOneIdBuildOneSpreadInLegIndexOrder) {
  InstrumentCatalog catalog;
  catalog.apply(nearby_leg());
  catalog.apply(deferred_leg());

  EXPECT_EQ(catalog.instrument_count(), 1U);
  const InstrumentDefinition& spread = catalog.at(kNqu6Nqz6);
  EXPECT_EQ(spread.raw_symbol, "NQU6-NQZ6");
  expect_nqu6_nqz6_legs(spread);
  EXPECT_EQ(catalog.stats().leg_records, 2U);
  EXPECT_EQ(catalog.stats().replacements, 0U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(SpreadLegs, LegsArrivingDeferredFirstBuildTheSameSpread) {
  InstrumentCatalog catalog;
  catalog.apply(deferred_leg());
  EXPECT_FALSE(catalog.at(kNqu6Nqz6).has_all_legs());
  catalog.apply(nearby_leg());

  expect_nqu6_nqz6_legs(catalog.at(kNqu6Nqz6));
  EXPECT_EQ(catalog.stats().replacements, 0U);
}

TEST(SpreadLegs, ALegFromALaterSendNeverCompletesAnEarlierUnfinishedOne) {
  struct Later {
    const char* what;
    std::uint64_t sent_after;
    std::uint64_t received_after;
  };

  for (const Later later : {Later{"sent later", 1, 0}, Later{"received later", 0, 1}}) {
    SCOPED_TRACE(later.what);
    InstrumentCatalog catalog;
    catalog.apply(nearby_leg());

    InstrumentDefMsg deferred = deferred_leg();
    deferred.hd.ts_event += later.sent_after;
    deferred.ts_recv += later.received_after;
    catalog.apply(deferred);
    EXPECT_FALSE(catalog.at(kNqu6Nqz6).has_all_legs());

    InstrumentDefMsg nearby = nearby_leg();
    nearby.hd.ts_event = deferred.hd.ts_event;
    nearby.ts_recv = deferred.ts_recv;
    catalog.apply(nearby);
    expect_nqu6_nqz6_legs(catalog.at(kNqu6Nqz6));
    EXPECT_EQ(catalog.stats().replacements, 1U);
  }
}

TEST(SpreadLegs, TheSameLegTwiceBeginsANewDefinition) {
  InstrumentCatalog catalog;
  catalog.apply(nearby_leg());
  catalog.apply(nearby_leg());

  const InstrumentDefinition& spread = catalog.at(kNqu6Nqz6);
  EXPECT_FALSE(spread.has_all_legs());
  ASSERT_EQ(spread.legs.size(), 1U);
  EXPECT_EQ(catalog.stats().replacements, 1U);
}

TEST(SpreadLegs, ALegDeclaringAnotherLegCountBeginsANewDefinition) {
  InstrumentDefMsg of_three = deferred_leg();
  of_three.leg_count = 3;

  InstrumentCatalog catalog;
  catalog.apply(nearby_leg());
  catalog.apply(of_three);

  const InstrumentDefinition& spread = catalog.at(kNqu6Nqz6);
  EXPECT_EQ(spread.leg_count, 3U);
  ASSERT_EQ(spread.legs.size(), 1U);
  EXPECT_EQ(spread.legs[0].index, 1U);
  EXPECT_EQ(catalog.stats().replacements, 1U);
}

TEST(SpreadLegs, ALegIndexAtOrBeyondTheLegCountThrows) {
  InstrumentDefMsg past_the_end = nearby_leg();
  past_the_end.leg_index = 2;
  InstrumentDefMsg last = nearby_leg();
  last.leg_index = 1;

  InstrumentCatalog catalog;
  EXPECT_THROW(catalog.apply(past_the_end), DefinitionError);
  EXPECT_EQ(catalog.instrument_count(), 0U);
  EXPECT_EQ(catalog.stats().records, 0U);
  EXPECT_NO_THROW(catalog.apply(last));
}

TEST(SpreadLegs, ALeglessRecordLoadsWhateverItsLegFieldsHold) {
  InstrumentDefMsg rec = outright(kNqu6, "NQU6");
  rec.leg_index = 9;
  rec.leg_side = '\0';

  InstrumentCatalog catalog;
  EXPECT_NO_THROW(catalog.apply(rec));
  EXPECT_TRUE(catalog.at(kNqu6).legs.empty());
  EXPECT_EQ(catalog.stats().leg_records, 0U);
}

TEST(SpreadLegs, ALegSideByteOutsideTheDocumentedSetThrows) {
  InstrumentDefMsg rec = nearby_leg();
  rec.leg_side = 'Z';

  InstrumentCatalog catalog;
  EXPECT_THROW(catalog.apply(rec), DefinitionError);
  EXPECT_EQ(catalog.instrument_count(), 0U);
  EXPECT_EQ(catalog.stats().records, 0U);
}

TEST(DefinedTick, EachInstrumentScalesByItsOwnIncrement) {
  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6"));
  catalog.apply(nearby_leg());
  catalog.apply(deferred_leg());

  const InstrumentDefinition& nqu6 = catalog.at(kNqu6);
  const InstrumentDefinition& spread = catalog.at(kNqu6Nqz6);
  EXPECT_FALSE(nqu6.is_spread());
  EXPECT_TRUE(spread.is_spread());
  EXPECT_EQ(nqu6.tick_scale().tick_size(), kTick);
  EXPECT_EQ(spread.tick_scale().tick_size(), kSpreadTick);
}

TEST(DefinedTick, AnIncrementThatIsUndefinedOrNotPositiveLoadsButRefusesToScale) {
  for (const std::int64_t increment : {kUndefPrice, std::int64_t{0}, std::int64_t{-1}}) {
    SCOPED_TRACE(increment);
    InstrumentCatalog catalog;
    catalog.apply(outright(kNqu6, "NQU6", increment));

    const InstrumentDefinition& nqu6 = catalog.at(kNqu6);
    EXPECT_FALSE(nqu6.has_tick_size());
    try {
      (void)nqu6.tick_scale();
      ADD_FAILURE() << "expected a DefinitionError";
    } catch (const DefinitionError& e) {
      const std::string msg = e.what();
      EXPECT_NE(msg.find(std::to_string(kNqu6)), std::string::npos) << msg;
      EXPECT_NE(msg.find("NQU6"), std::string::npos) << msg;
    }
  }
}

TEST(DefinedTick, AnIncrementChangedWithinTheStreamLeavesNoTickSize) {
  InstrumentDefMsg next_day = outright(kNqu6, "NQU6", 2 * kTick);
  next_day.ts_recv += kDay;

  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6"));
  catalog.apply(next_day);
  EXPECT_FALSE(catalog.at(kNqu6).has_tick_size());
  EXPECT_THROW((void)catalog.at(kNqu6).tick_scale(), DefinitionError);

  next_day.ts_recv += kDay;
  catalog.apply(next_day);
  EXPECT_FALSE(catalog.at(kNqu6).has_tick_size());
}

TEST(OutrightOrSpread, CountsInstrumentsNotRecords) {
  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6"));
  catalog.apply(outright(kNqz6, "NQZ6", kTick, kDecemberExpiry));
  catalog.apply(nearby_leg());
  catalog.apply(deferred_leg());

  EXPECT_EQ(catalog.instrument_count(), 3U);
  EXPECT_EQ(catalog.outright_count(), 2U);
  EXPECT_EQ(catalog.spread_count(), 1U);
  EXPECT_EQ(catalog.instruments(), (std::vector<std::uint32_t>{kNqz6, kNqu6, kNqu6Nqz6}));
  EXPECT_EQ(catalog.stats().records, 4U);
  EXPECT_EQ(catalog.stats().leg_records, 2U);
  EXPECT_EQ(catalog.stats().class_leg_disagreements, 0U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(OutrightOrSpread, FollowsTheClassWhereItDisagreesWithTheLegsAndCountsIt) {
  InstrumentDefMsg legless_spread = outright(kNqu6Nqz6, "NQU6-NQZ6", kSpreadTick);
  legless_spread.instrument_class = static_cast<char>(InstrumentClass::kFutureSpread);
  InstrumentDefMsg legged_future = spread_leg(kNqh7, "NQH7", 0, kNqu6, "NQU6", Side::kAsk, kTick);
  legged_future.instrument_class = static_cast<char>(InstrumentClass::kFuture);

  InstrumentCatalog catalog;
  catalog.apply(legless_spread);
  catalog.apply(legged_future);

  EXPECT_TRUE(catalog.at(kNqu6Nqz6).is_spread());
  EXPECT_FALSE(catalog.at(kNqh7).is_spread());
  EXPECT_EQ(catalog.at(kNqh7).legs.size(), 1U);
  EXPECT_EQ(catalog.spread_count(), 1U);
  EXPECT_EQ(catalog.stats().class_leg_disagreements, 2U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(OutrightOrSpread, AClassByteOutsideTheDocumentedSetThrows) {
  InstrumentDefMsg rec = outright(kNqu6, "NQU6");
  rec.instrument_class = 'Z';

  InstrumentCatalog catalog;
  EXPECT_THROW(catalog.apply(rec), DefinitionError);
  EXPECT_EQ(catalog.instrument_count(), 0U);
  EXPECT_EQ(catalog.stats().records, 0U);
}

TEST(Projection, EveryStoredFieldReadsBackFromItsRecord) {
  const InstrumentDefMsg rec = outright(kNqz6, "NQZ6", kTick, kDecemberExpiry);
  InstrumentCatalog catalog;
  catalog.apply(rec);

  const InstrumentDefinition& nqz6 = catalog.at(kNqz6);
  EXPECT_EQ(nqz6.instrument_id, kNqz6);
  EXPECT_EQ(nqz6.raw_symbol, "NQZ6");
  EXPECT_EQ(nqz6.asset, "NQ");
  EXPECT_EQ(nqz6.exchange, "XCME");
  EXPECT_EQ(nqz6.instrument_class, InstrumentClass::kFuture);
  EXPECT_EQ(nqz6.match_algorithm, 'F');
  EXPECT_EQ(nqz6.min_price_increment, kTick);
  EXPECT_EQ(nqz6.expiration, kDecemberExpiry);
  EXPECT_EQ(nqz6.activation, rec.activation);
  EXPECT_EQ(nqz6.ts_event, rec.hd.ts_event);
  EXPECT_EQ(nqz6.ts_recv, rec.ts_recv);
  EXPECT_EQ(nqz6.leg_count, 0U);
  EXPECT_TRUE(nqz6.legs.empty());
}

TEST(UpdateAction, ARepeatedAddReplacesTheInstrument) {
  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6", kTick, kSeptemberExpiry));
  catalog.apply(outright(kNqu6, "NQU6", kTick, kDecemberExpiry));

  EXPECT_EQ(catalog.instrument_count(), 1U);
  EXPECT_EQ(catalog.at(kNqu6).expiration, kDecemberExpiry);
  EXPECT_TRUE(catalog.at(kNqu6).has_tick_size());
  EXPECT_EQ(catalog.stats().adds, 2U);
  EXPECT_EQ(catalog.stats().replacements, 1U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(UpdateAction, OneOutsideTheDocumentedSetThrows) {
  InstrumentDefMsg rec = outright(kNqu6, "NQU6");
  rec.security_update_action = '~';

  InstrumentCatalog catalog;
  EXPECT_THROW(catalog.apply(rec), DefinitionError);
  EXPECT_EQ(catalog.instrument_count(), 0U);
  EXPECT_EQ(catalog.stats().records, 0U);
}

TEST(UpdateAction, AModifyReplacesASpreadAndItsLegsThoughThePullSendsNone) {
  InstrumentCatalog catalog;
  catalog.apply(nearby_leg());
  catalog.apply(deferred_leg());

  InstrumentDefMsg nearby = nearby_leg();
  nearby.security_update_action = kSecurityUpdateModify;
  nearby.expiration = kDecemberExpiry;
  catalog.apply(nearby);

  const InstrumentDefinition& half = catalog.at(kNqu6Nqz6);
  EXPECT_EQ(half.expiration, kDecemberExpiry);
  EXPECT_FALSE(half.has_all_legs());
  ASSERT_EQ(half.legs.size(), 1U);
  EXPECT_EQ(half.legs[0].instrument_id, kNqu6);

  InstrumentDefMsg deferred = deferred_leg();
  deferred.security_update_action = kSecurityUpdateModify;
  deferred.expiration = kDecemberExpiry;
  catalog.apply(deferred);

  expect_nqu6_nqz6_legs(catalog.at(kNqu6Nqz6));
  EXPECT_EQ(catalog.stats().modifies, 2U);
  EXPECT_EQ(catalog.stats().replacements, 1U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(UpdateAction, ADeleteErasesTheInstrumentThoughThePullSendsNone) {
  InstrumentDefMsg deletion = outright(kNqu6, "NQU6");
  deletion.security_update_action = kSecurityUpdateDelete;

  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6"));
  catalog.apply(outright(kNqz6, "NQZ6", kTick, kDecemberExpiry));
  catalog.apply(deletion);

  EXPECT_EQ(catalog.find(kNqu6), nullptr);
  EXPECT_NE(catalog.find(kNqz6), nullptr);
  EXPECT_EQ(catalog.instrument_count(), 1U);
  EXPECT_EQ(catalog.stats().deletions, 1U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(UpdateAction, AnIdRelistedAfterADeleteStartsCleanOfTheOldIncrement) {
  InstrumentDefMsg deletion = outright(kNqu6, "NQU6", kSpreadTick);
  deletion.security_update_action = kSecurityUpdateDelete;

  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6", kSpreadTick));
  catalog.apply(deletion);
  catalog.apply(outright(kNqu6, "NQU6", kTick));

  const InstrumentDefinition& nqu6 = catalog.at(kNqu6);
  EXPECT_FALSE(nqu6.increment_changed);
  EXPECT_EQ(nqu6.tick_scale().tick_size(), kTick);
  EXPECT_EQ(catalog.stats().deletions, 1U);
  EXPECT_EQ(catalog.stats().replacements, 0U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(UpdateAction, ARefusedRecordLeavesTheHeldDefinitionUntouched) {
  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6", kTick, kSeptemberExpiry));
  catalog.apply(nearby_leg());

  InstrumentDefMsg bad_class = outright(kNqu6, "NQU6", 2 * kTick, kDecemberExpiry);
  bad_class.instrument_class = 'Z';
  InstrumentDefMsg bad_action = outright(kNqu6, "NQU6", 2 * kTick, kDecemberExpiry);
  bad_action.security_update_action = '~';
  InstrumentDefMsg bad_leg_index = deferred_leg();
  bad_leg_index.leg_index = 2;
  InstrumentDefMsg bad_leg_side = deferred_leg();
  bad_leg_side.leg_side = 'Z';

  for (const InstrumentDefMsg& rec : {bad_class, bad_action, bad_leg_index, bad_leg_side}) {
    EXPECT_THROW(catalog.apply(rec), DefinitionError);
  }

  const InstrumentDefinition& nqu6 = catalog.at(kNqu6);
  EXPECT_EQ(nqu6.expiration, kSeptemberExpiry);
  EXPECT_EQ(nqu6.min_price_increment, kTick);
  EXPECT_TRUE(nqu6.has_tick_size());

  const InstrumentDefinition& spread = catalog.at(kNqu6Nqz6);
  ASSERT_EQ(spread.legs.size(), 1U);
  EXPECT_EQ(spread.legs[0].instrument_id, kNqu6);

  EXPECT_EQ(catalog.instrument_count(), 2U);
  EXPECT_EQ(catalog.stats().records, 2U);
  EXPECT_EQ(catalog.stats().adds, 2U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(Lookup, AtThrowsNamingAnUnknownIdAndFindReturnsNull) {
  InstrumentCatalog catalog;
  catalog.apply(outright(kNqu6, "NQU6"));

  EXPECT_EQ(catalog.find(999), nullptr);
  try {
    (void)catalog.at(999);
    FAIL() << "expected a DefinitionError";
  } catch (const DefinitionError& e) {
    EXPECT_NE(std::string{e.what()}.find("999"), std::string::npos) << e.what();
  }
}

TEST(Pairing, AcceptsTheDataWindowOrOneOpeningEarlier) {
  const DbnMetadata mbo = window(kSchemaMbo, kBaseTs, kBaseTs + kDay);

  EXPECT_NO_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs, kBaseTs + kDay), mbo));
  EXPECT_NO_THROW(
      require_definitions_for(window(kSchemaDefinition, kBaseTs - kDay, kBaseTs + kDay), mbo));
  EXPECT_NO_THROW(require_definitions_for(window(std::nullopt, kBaseTs, kBaseTs + kDay), mbo));
  EXPECT_NO_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs, std::nullopt),
                                          window(kSchemaMbo, kBaseTs, std::nullopt)));
}

TEST(Pairing, RefusesAnotherDatasetOrSchemaOrARecordLimit) {
  const DbnMetadata mbo = window(kSchemaMbo, kBaseTs, kBaseTs + kDay);
  DbnMetadata capped = window(kSchemaDefinition, kBaseTs, kBaseTs + kDay);
  capped.limit = 20;

  EXPECT_THROW(
      require_definitions_for(window(kSchemaDefinition, kBaseTs, kBaseTs + kDay, "XNAS.ITCH"), mbo),
      DefinitionError);
  EXPECT_THROW(require_definitions_for(window(kSchemaMbo, kBaseTs, kBaseTs + kDay), mbo),
               DefinitionError);
  EXPECT_THROW(require_definitions_for(capped, mbo), DefinitionError);
}

TEST(Pairing, RefusesAWindowOpeningLaterOrClosingEarlier) {
  const DbnMetadata mbo = window(kSchemaMbo, kBaseTs, kBaseTs + kDay);

  EXPECT_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs + 1, kBaseTs + kDay), mbo),
               DefinitionError);
  EXPECT_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs, kBaseTs + kDay - 1), mbo),
               DefinitionError);
}

TEST(Pairing, RefusesAWindowClosingLaterSinceTheLastDefinitionWins) {
  const DbnMetadata mbo = window(kSchemaMbo, kBaseTs, kBaseTs + kDay);

  EXPECT_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs, kBaseTs + 2 * kDay), mbo),
               DefinitionError);
  EXPECT_THROW(require_definitions_for(window(kSchemaDefinition, kBaseTs, std::nullopt), mbo),
               DefinitionError);
}

TEST(Loader, TheCommittedEquityDefinitionsLoadWithNoTickSizeAndNoExpiry) {
  const InstrumentCatalog catalog = read_definitions(fixture("test_data.definition.v3.dbn.zst"));

  EXPECT_EQ(catalog.instruments(), (std::vector<std::uint32_t>{6819, 6830}));
  for (const std::uint32_t id : catalog.instruments()) {
    SCOPED_TRACE(id);
    const InstrumentDefinition& msft = catalog.at(id);
    EXPECT_EQ(msft.raw_symbol, "MSFT");
    EXPECT_EQ(msft.exchange, "XNAS");
    EXPECT_EQ(msft.instrument_class, InstrumentClass::kStock);
    EXPECT_FALSE(msft.is_spread());
    EXPECT_FALSE(msft.has_tick_size());
    EXPECT_THROW((void)msft.tick_scale(), DefinitionError);
    EXPECT_EQ(msft.expiration, kUndefTimestamp);
  }
  EXPECT_EQ(catalog.stats().adds, 2U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(Loader, AnEncodedNqStreamLoadsThroughTheReader) {
  const std::vector<std::byte> bytes = testing::DbnEncoder{}
                                           .schema(kSchemaDefinition)
                                           .add(outright(kNqu6, "NQU6"))
                                           .add(outright(kNqz6, "NQZ6", kTick, kDecemberExpiry))
                                           .add(nearby_leg())
                                           .add(deferred_leg())
                                           .encode_compressed();

  const InstrumentCatalog catalog = read_definitions(bytes.data(), bytes.size());
  EXPECT_EQ(catalog.instrument_count(), 3U);
  EXPECT_EQ(catalog.outright_count(), 2U);
  EXPECT_EQ(catalog.spread_count(), 1U);
  EXPECT_EQ(catalog.at(kNqu6).tick_scale().tick_size(), kTick);
  EXPECT_EQ(catalog.at(kNqz6).tick_scale().tick_size(), kTick);
  EXPECT_EQ(catalog.at(kNqu6Nqz6).tick_scale().tick_size(), kSpreadTick);
  expect_nqu6_nqz6_legs(catalog.at(kNqu6Nqz6));
  EXPECT_EQ(catalog.stats().records, 4U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

TEST(Loader, AFileOfAnotherSchemaIsRefusedAtOpenNamingIt) {
  try {
    (void)read_definitions(fixture("test_data.mbo.v3.dbn.zst"));
    FAIL() << "expected a DefinitionError";
  } catch (const DefinitionError& e) {
    EXPECT_NE(std::string{e.what()}.find("schema 0"), std::string::npos) << e.what();
  }
}

TEST(Loader, AMixedSchemaStreamLoadsItsDefinitionsAndCountsTheRest) {
  testing::StreamBuilder mbo;
  mbo.add(1, Side::kBid, testing::px(29000), 1);
  const std::vector<std::byte> bytes = testing::DbnEncoder{}
                                           .schema(std::nullopt)
                                           .add(mbo.records().front())
                                           .add(outright(kNqu6, "NQU6"))
                                           .encode();

  const InstrumentCatalog catalog = read_definitions(bytes.data(), bytes.size());
  EXPECT_EQ(catalog.instruments(), (std::vector<std::uint32_t>{kNqu6}));
  EXPECT_EQ(catalog.stats().records, 2U);
  EXPECT_EQ(catalog.stats().skipped_other_schema, 1U);
  EXPECT_TRUE(catalog.stats().reconciles());
}

}  // namespace
}  // namespace bookreplay
