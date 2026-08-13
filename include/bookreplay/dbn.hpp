// DBN wire types for the GLBX.MDP3 `mbo` schema.
//
// The layout below is not taken on faith. It was verified against a real
// 2026-08-05 GLBX.MDP3 MBO file (28,562,350 records) by parsing the head of
// the stream with this exact field order and diffing every field against
// Databento's own decoder: 8/8 records identical. The static_asserts here are
// that check frozen into the build.

#ifndef BOOKREPLAY_DBN_HPP
#define BOOKREPLAY_DBN_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace bookreplay {

inline constexpr std::int64_t kPriceScale = 1'000'000'000;
inline constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();
inline constexpr std::uint8_t kRTypeMbo = 0xA0;

/// `length` is expressed in 4-byte units: 56 / 4 == 14.
inline constexpr std::uint8_t kMboLengthUnits = 14;

/// The only DBN version this decoder has been tested against.
inline constexpr std::uint8_t kSupportedDbnVersion = 3;
inline constexpr std::uint8_t kFlagLast = 128;
inline constexpr std::uint8_t kFlagTob = 64;
inline constexpr std::uint8_t kFlagSnapshot = 32;
inline constexpr std::uint8_t kFlagMbp = 16;
inline constexpr std::uint8_t kFlagBadTsRecv = 8;
inline constexpr std::uint8_t kFlagMaybeBadBook = 4;
inline constexpr std::uint8_t kFlagPublisherSpecific = 2;

enum class Action : char {
  kAdd = 'A',     ///< insert at the TAIL of (side, price)
  kCancel = 'C',  ///< erase the order; erase the level if it empties
  kModify = 'M',
  kTrade = 'T',   ///< READ-ONLY. Never mutates resting quantity.
  kFill = 'F',    ///< READ-ONLY. Never mutates resting quantity.
  kNone = 'N',    ///< READ-ONLY. Carries flags; post-renorm F_LAST carrier.
  kClear = 'R',   ///< drop all orders for that instrument_id
};

enum class Side : char {
  kBid = 'B',
  kAsk = 'A',
  kNone = 'N',
};

[[nodiscard]] constexpr bool mutates_book(Action a) noexcept {
  return a == Action::kAdd || a == Action::kCancel || a == Action::kModify || a == Action::kClear;
}

[[nodiscard]] constexpr bool is_known_action(char c) noexcept {
  return c == 'A' || c == 'C' || c == 'M' || c == 'T' || c == 'F' || c == 'N' || c == 'R';
}

[[nodiscard]] constexpr bool is_known_side(char c) noexcept {
  return c == 'B' || c == 'A' || c == 'N';
}

/// Byte-for-byte mirror of DBN's MboMsg.
struct MboMsg {
  // RecordHeader
  std::uint8_t length;  ///< in 4-byte units; 14 for MboMsg
  std::uint8_t rtype;   ///< kRTypeMbo
  std::uint16_t publisher_id;
  std::uint32_t instrument_id;
  std::uint64_t ts_event;

  // Body
  std::uint64_t order_id;
  std::int64_t price;
  std::uint32_t size;
  std::uint8_t flags;
  std::uint8_t channel_id;
  char action;
  char side;
  std::uint64_t ts_recv;
  std::int32_t ts_in_delta;
  std::uint32_t sequence;
};

static_assert(sizeof(MboMsg) == 56, "MboMsg must be exactly 56 bytes");
static_assert(alignof(MboMsg) == 8, "MboMsg must be 8-aligned");
static_assert(std::is_standard_layout_v<MboMsg>, "MboMsg must be standard layout");
static_assert(std::is_trivially_copyable_v<MboMsg>, "MboMsg must be trivially copyable");

static_assert(offsetof(MboMsg, length) == 0);
static_assert(offsetof(MboMsg, rtype) == 1);
static_assert(offsetof(MboMsg, publisher_id) == 2);
static_assert(offsetof(MboMsg, instrument_id) == 4);
static_assert(offsetof(MboMsg, ts_event) == 8);
static_assert(offsetof(MboMsg, order_id) == 16);
static_assert(offsetof(MboMsg, price) == 24);
static_assert(offsetof(MboMsg, size) == 32);
static_assert(offsetof(MboMsg, flags) == 36);
static_assert(offsetof(MboMsg, channel_id) == 37);
static_assert(offsetof(MboMsg, action) == 38);
static_assert(offsetof(MboMsg, side) == 39);
static_assert(offsetof(MboMsg, ts_recv) == 40);
static_assert(offsetof(MboMsg, ts_in_delta) == 48);
static_assert(offsetof(MboMsg, sequence) == 52);

[[nodiscard]] constexpr Action action_of(const MboMsg& r) noexcept {
  return static_cast<Action>(r.action);
}

[[nodiscard]] constexpr Side side_of(const MboMsg& r) noexcept {
  return static_cast<Side>(r.side);
}

[[nodiscard]] constexpr bool has_flag(const MboMsg& r, std::uint8_t flag) noexcept {
  return (r.flags & flag) != 0;
}

/// The event boundary. Post-renormalization this may arrive on a standalone
/// `action='N'` record — but measured on 2026-08-05, only 482,044 of the
/// 26,878,300 boundaries did (1.8%); the other 98.2% still ride on an ordinary
/// A/C/M record. So the test is the FLAG, never the action.
[[nodiscard]] constexpr bool is_event_boundary(const MboMsg& r) noexcept {
  return has_flag(r, kFlagLast);
}

[[nodiscard]] constexpr bool maybe_bad_book(const MboMsg& r) noexcept {
  return has_flag(r, kFlagMaybeBadBook);
}

[[nodiscard]] constexpr bool is_undef_price(std::int64_t price) noexcept {
  return price == kUndefPrice;
}

[[nodiscard]] bool is_supported_dbn_version(std::uint8_t version) noexcept;
[[nodiscard]] const char* action_name(Action a) noexcept;
[[nodiscard]] const char* side_name(Side s) noexcept;

}  // namespace bookreplay

#endif  // BOOKREPLAY_DBN_HPP
