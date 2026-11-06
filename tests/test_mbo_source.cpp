#include "bookreplay/dbn.hpp"
#include "bookreplay/mbo_source.hpp"
#include "bookreplay/order.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "dbn_encoder.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path{BOOKREPLAY_TEST_DATA_DIR} / name;
}

TEST(MboSource, AFileOfMboRecordsIsReadToItsEndInOrder) {
  DbnMboSource source{fixture("test_data.mbo.v3.dbn.zst")};
  ASSERT_TRUE(source.metadata().schema);
  EXPECT_EQ(*source.metadata().schema, kSchemaMbo);

  std::size_t count = 0;
  std::uint64_t last = 0;
  while (const MboMsg* rec = source.next()) {
    EXPECT_EQ(rec->hd.rtype, kRTypeMbo);
    EXPECT_GE(rec->ts_recv, last);
    last = rec->ts_recv;
    ++count;
  }
  EXPECT_EQ(count, std::size_t{2});
  EXPECT_EQ(source.next(), nullptr);
}

TEST(MboSource, AFileOfAnotherSchemaIsRefusedBeforeARecordIsRead) {
  EXPECT_THROW((DbnMboSource{fixture("test_data.trades.v3.dbn.zst")}), ReplayError);
  EXPECT_THROW((DbnMboSource{fixture("test_data.status.v3.dbn.zst")}), ReplayError);
}

TEST(MboSource, ARecordThatIsNotMboInsideAStreamThatNamesNoSchemaIsRefusedWhenReached) {
  const std::vector<MboMsg> one = testing::clean_trade_event();
  testing::DbnEncoder encoder;
  encoder.schema(std::nullopt)
      .add(one.front())
      .add(testing::status(42004177, kStatusActionTrading, kTriStateYes));
  const std::vector<std::byte> bytes = encoder.encode();
  DbnMboSource source{bytes.data(), bytes.size()};

  EXPECT_NE(source.next(), nullptr);
  EXPECT_THROW((void)source.next(), ReplayError);
}

TEST(MboSource, ASpanIsReplayedAsHeld) {
  const std::vector<MboMsg> records = testing::clean_trade_event();
  MboSpanSource source{records};

  for (const MboMsg& expected : records) {
    const MboMsg* rec = source.next();
    ASSERT_NE(rec, nullptr);
    EXPECT_EQ(rec, &expected);
  }
  EXPECT_EQ(source.next(), nullptr);
  EXPECT_EQ(source.next(), nullptr);
}

}  // namespace
}  // namespace bookreplay
