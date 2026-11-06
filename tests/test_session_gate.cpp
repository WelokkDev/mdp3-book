#include "bookreplay/dbn.hpp"
#include "bookreplay/order.hpp"
#include "bookreplay/session_gate.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::status;

constexpr std::uint32_t kIid = 42004177;
constexpr std::uint32_t kOtherIid = 42019315;
constexpr std::uint64_t kOpen = 1'000;
constexpr std::uint64_t kHalt = 2'000;
constexpr std::uint64_t kResume = 3'000;

StatusMsg trading(std::uint64_t ts_event, std::uint32_t instrument_id = kIid) {
  return status(instrument_id, kStatusActionTrading, kTriStateYes, kTriStateYes, ts_event);
}

/// The venue reports a halt as a pre-open that is not trading.
StatusMsg halted(std::uint64_t ts_event, std::uint32_t instrument_id = kIid) {
  return status(instrument_id, kStatusActionPreOpen, kTriStateNo, kTriStateYes, ts_event);
}

TEST(SessionGate, TheStatusThatOpensTradingAppliesBeforeItsOwnInstantsEvents) {
  const std::vector<StatusMsg> statuses{trading(kOpen)};
  SessionGate gate{statuses, kIid, false};

  EXPECT_FALSE(gate.take_change_before(kOpen - 1));
  EXPECT_FALSE(gate.trading());

  const std::optional<SessionGate::Change> change = gate.take_change_before(kOpen);
  ASSERT_TRUE(change);
  EXPECT_TRUE(change->trading);
  EXPECT_EQ(change->ts_event, std::int64_t{kOpen});
  EXPECT_TRUE(gate.trading());
  // The event at that instant is the uncross the status admitted.
  EXPECT_FALSE(gate.continuous_at(kOpen));
  EXPECT_TRUE(gate.continuous_at(kOpen + 1));
}

TEST(SessionGate, TheStatusThatClosesTradingAppliesAfterItsOwnInstantsEvents) {
  const std::vector<StatusMsg> statuses{halted(kHalt)};
  SessionGate gate{statuses, kIid, true};

  EXPECT_FALSE(gate.take_change_before(kHalt));
  EXPECT_TRUE(gate.continuous_at(kHalt));

  const std::optional<SessionGate::Change> change = gate.take_change_before(kHalt + 1);
  ASSERT_TRUE(change);
  EXPECT_FALSE(change->trading);
  EXPECT_FALSE(gate.trading());
  EXPECT_FALSE(gate.continuous_at(kHalt + 1));
}

TEST(SessionGate, ChangesComeOneAtATimeInVenueOrderWhateverOrderTheyWereGivenIn) {
  const std::vector<StatusMsg> statuses{trading(kResume), halted(kHalt), trading(kOpen)};
  SessionGate gate{statuses, kIid, false};

  std::vector<std::int64_t> seen;
  while (const std::optional<SessionGate::Change> change = gate.take_change_before(kResume + 1)) {
    seen.push_back(change->ts_event);
  }

  EXPECT_EQ(seen, (std::vector<std::int64_t>{kOpen, kHalt, kResume}));
  EXPECT_FALSE(gate.continuous_at(kResume));
  EXPECT_TRUE(gate.continuous_at(kResume + 1));
}

TEST(SessionGate, ARecordThatRestatesTheStateIsNoChange) {
  // The open arrives as two records, and a new session's first is a pre-open
  // following a close: neither second record changes whether it is trading.
  const std::vector<StatusMsg> statuses{
      trading(kOpen),
      status(kIid, kStatusActionNewPriceIndication, kTriStateYes, kTriStateYes, kOpen),
      status(kIid, kStatusActionClose, kTriStateNo, kTriStateNo, kHalt), halted(kHalt + 5)};
  SessionGate gate{statuses, kIid, false};

  ASSERT_TRUE(gate.take_change_before(kOpen));
  EXPECT_FALSE(gate.take_change_before(kHalt));
  ASSERT_TRUE(gate.take_change_before(kResume));
  EXPECT_FALSE(gate.take_change_before(kResume));
  EXPECT_EQ(gate.records(), std::size_t{4});
}

TEST(SessionGate, AnotherInstrumentsRecordsAreNotThisOnes) {
  const std::vector<StatusMsg> statuses{trading(kOpen, kOtherIid)};
  SessionGate gate{statuses, kIid, false};

  EXPECT_FALSE(gate.take_change_before(kResume));
  EXPECT_FALSE(gate.trading());
  EXPECT_EQ(gate.records(), std::size_t{0});
}

TEST(SessionGate, WithNoRecordItStaysAsItStarted) {
  SessionGate gate{{}, kIid, true};

  EXPECT_FALSE(gate.take_change_before(kNever));
  EXPECT_TRUE(gate.continuous_at(0));
}

}  // namespace
}  // namespace bookreplay
