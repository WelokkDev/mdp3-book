// DBN v3 wire types for the GLBX.MDP3 schemas this project reads.
//
// The MBO layout was verified against a real 2026-08-05 GLBX.MDP3 MBO file
// (28,562,350 records), diffing every field against Databento's own decoder.
//
// The structs are implicit-lifetime types, so C++20 implicit object creation
// (P0593R6) makes casting a suitably aligned byte buffer to them well defined.

#ifndef BOOKREPLAY_DBN_HPP
#define BOOKREPLAY_DBN_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace bookreplay {

/// The root of every exception this library throws.
class BookreplayError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::int64_t kPriceScale = 1'000'000'000;
inline constexpr std::int64_t kUndefPrice = std::numeric_limits<std::int64_t>::max();
inline constexpr std::uint32_t kUndefOrderSize = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint64_t kUndefTimestamp = std::numeric_limits<std::uint64_t>::max();

/// `length` counts 4-byte units, so a record is `length * 4` bytes.
inline constexpr std::size_t kLengthUnit = 4;

inline constexpr std::uint8_t kRTypeTrade = 0x00;
inline constexpr std::uint8_t kRTypeMbp10 = 0x0A;
inline constexpr std::uint8_t kRTypeStatus = 0x12;
inline constexpr std::uint8_t kRTypeInstrumentDef = 0x13;
inline constexpr std::uint8_t kRTypeMbo = 0xA0;

inline constexpr std::uint8_t kSupportedDbnVersion = 3;

inline constexpr std::size_t kSymbolCstrLen = 71;
inline constexpr std::size_t kAssetCstrLen = 11;

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
  kTrade = 'T',  ///< READ-ONLY. Never mutates resting quantity.
  kFill = 'F',   ///< READ-ONLY. Never mutates resting quantity.
  kNone = 'N',   ///< READ-ONLY. Carries flags; post-renorm F_LAST carrier.
  kClear = 'R',  ///< drop all orders for that instrument_id
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

/// Leads every DBN record regardless of schema.
struct RecordHeader {
  std::uint8_t length;  ///< in 4-byte units
  std::uint8_t rtype;
  std::uint16_t publisher_id;
  std::uint32_t instrument_id;
  std::uint64_t ts_event;
};

static_assert(sizeof(RecordHeader) == 16);
static_assert(alignof(RecordHeader) == 8);
static_assert(offsetof(RecordHeader, length) == 0);
static_assert(offsetof(RecordHeader, rtype) == 1);
static_assert(offsetof(RecordHeader, publisher_id) == 2);
static_assert(offsetof(RecordHeader, instrument_id) == 4);
static_assert(offsetof(RecordHeader, ts_event) == 8);

struct MboMsg {
  RecordHeader hd;
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

static_assert(sizeof(MboMsg) == 56);
static_assert(offsetof(MboMsg, hd) == 0);
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

struct BidAskPair {
  std::int64_t bid_px;
  std::int64_t ask_px;
  std::uint32_t bid_sz;
  std::uint32_t ask_sz;
  std::uint32_t bid_ct;
  std::uint32_t ask_ct;
};

static_assert(sizeof(BidAskPair) == 32);
static_assert(offsetof(BidAskPair, bid_px) == 0);
static_assert(offsetof(BidAskPair, ask_px) == 8);
static_assert(offsetof(BidAskPair, bid_sz) == 16);
static_assert(offsetof(BidAskPair, ask_sz) == 20);
static_assert(offsetof(BidAskPair, bid_ct) == 24);
static_assert(offsetof(BidAskPair, ask_ct) == 28);

/// The `trades` schema. DBN calls it MBP-0: a trade print with no book.
struct TradeMsg {
  RecordHeader hd;
  std::int64_t price;
  std::uint32_t size;
  char action;
  char side;
  std::uint8_t flags;
  std::uint8_t depth;
  std::uint64_t ts_recv;
  std::int32_t ts_in_delta;
  std::uint32_t sequence;
};

static_assert(sizeof(TradeMsg) == 48);
static_assert(offsetof(TradeMsg, price) == 16);
static_assert(offsetof(TradeMsg, size) == 24);
static_assert(offsetof(TradeMsg, action) == 28);
static_assert(offsetof(TradeMsg, side) == 29);
static_assert(offsetof(TradeMsg, flags) == 30);
static_assert(offsetof(TradeMsg, depth) == 31);
static_assert(offsetof(TradeMsg, ts_recv) == 32);
static_assert(offsetof(TradeMsg, ts_in_delta) == 40);
static_assert(offsetof(TradeMsg, sequence) == 44);

/// The `mbp-10` schema. The field order differs from MboMsg: action and side
/// precede flags here.
struct Mbp10Msg {
  RecordHeader hd;
  std::int64_t price;
  std::uint32_t size;
  char action;
  char side;
  std::uint8_t flags;
  std::uint8_t depth;
  std::uint64_t ts_recv;
  std::int32_t ts_in_delta;
  std::uint32_t sequence;
  std::array<BidAskPair, 10> levels;
};

static_assert(sizeof(Mbp10Msg) == 368);
static_assert(offsetof(Mbp10Msg, price) == 16);
static_assert(offsetof(Mbp10Msg, size) == 24);
static_assert(offsetof(Mbp10Msg, action) == 28);
static_assert(offsetof(Mbp10Msg, side) == 29);
static_assert(offsetof(Mbp10Msg, flags) == 30);
static_assert(offsetof(Mbp10Msg, depth) == 31);
static_assert(offsetof(Mbp10Msg, ts_recv) == 32);
static_assert(offsetof(Mbp10Msg, ts_in_delta) == 40);
static_assert(offsetof(Mbp10Msg, sequence) == 44);
static_assert(offsetof(Mbp10Msg, levels) == 48);

/// The `status` schema.
struct StatusMsg {
  RecordHeader hd;
  std::uint64_t ts_recv;
  std::uint16_t action;
  std::uint16_t reason;
  std::uint16_t trading_event;
  char is_trading;  ///< 'Y', 'N', or '~' for not applicable
  char is_quoting;
  char is_short_sell_restricted;
  std::array<std::byte, 7> reserved;
};

static_assert(sizeof(StatusMsg) == 40);
static_assert(offsetof(StatusMsg, ts_recv) == 16);
static_assert(offsetof(StatusMsg, action) == 24);
static_assert(offsetof(StatusMsg, reason) == 26);
static_assert(offsetof(StatusMsg, trading_event) == 28);
static_assert(offsetof(StatusMsg, is_trading) == 30);
static_assert(offsetof(StatusMsg, is_quoting) == 31);
static_assert(offsetof(StatusMsg, is_short_sell_restricted) == 32);

inline constexpr std::uint16_t kStatusActionPreOpen = 1;
inline constexpr std::uint16_t kStatusActionPreCross = 2;
inline constexpr std::uint16_t kStatusActionQuoting = 3;
inline constexpr std::uint16_t kStatusActionCross = 4;
inline constexpr std::uint16_t kStatusActionRotation = 5;
inline constexpr std::uint16_t kStatusActionNewPriceIndication = 6;
inline constexpr std::uint16_t kStatusActionTrading = 7;
inline constexpr std::uint16_t kStatusActionHalt = 8;
inline constexpr std::uint16_t kStatusActionPause = 9;
inline constexpr std::uint16_t kStatusActionSuspend = 10;
inline constexpr std::uint16_t kStatusActionPreClose = 11;
inline constexpr std::uint16_t kStatusActionClose = 12;
inline constexpr std::uint16_t kStatusActionPostClose = 13;
inline constexpr std::uint16_t kStatusActionNotAvailableForTrading = 15;

inline constexpr char kTriStateYes = 'Y';
inline constexpr char kTriStateNo = 'N';
inline constexpr char kTriStateNotAvailable = '~';

/// The `definition` schema. DBN v3 reshaped this record: `asset` widened from
/// 7 to 11 bytes, `raw_instrument_id` from 32 to 64 bits, the `leg_*` fields
/// arrived, and `trading_reference_price`, `settl_price_type` and
/// `md_security_trading_status` were removed.
struct InstrumentDefMsg {
  RecordHeader hd;
  std::uint64_t ts_recv;
  std::int64_t min_price_increment;
  std::int64_t display_factor;
  std::uint64_t expiration;
  std::uint64_t activation;
  std::int64_t high_limit_price;
  std::int64_t low_limit_price;
  std::int64_t max_price_variation;
  std::int64_t unit_of_measure_qty;
  std::int64_t min_price_increment_amount;
  std::int64_t price_ratio;
  std::int64_t strike_price;
  std::uint64_t raw_instrument_id;
  std::int64_t leg_price;
  std::int64_t leg_delta;
  std::int32_t inst_attrib_value;
  std::uint32_t underlying_id;
  std::int32_t market_depth_implied;
  std::int32_t market_depth;
  std::uint32_t market_segment_id;
  std::uint32_t max_trade_vol;
  std::int32_t min_lot_size;
  std::int32_t min_lot_size_block;
  std::int32_t min_lot_size_round_lot;
  std::uint32_t min_trade_vol;
  std::int32_t contract_multiplier;
  std::int32_t decay_quantity;
  std::int32_t original_contract_size;
  std::uint32_t leg_instrument_id;
  std::int32_t leg_ratio_price_numerator;
  std::int32_t leg_ratio_price_denominator;
  std::int32_t leg_ratio_qty_numerator;
  std::int32_t leg_ratio_qty_denominator;
  std::uint32_t leg_underlying_id;
  std::int16_t appl_id;
  std::uint16_t maturity_year;
  std::uint16_t decay_start_date;
  std::uint16_t channel_id;
  std::uint16_t leg_count;
  std::uint16_t leg_index;
  std::array<char, 4> currency;
  std::array<char, 4> settl_currency;
  std::array<char, 6> secsubtype;
  std::array<char, kSymbolCstrLen> raw_symbol;
  std::array<char, 21> group;
  std::array<char, 5> exchange;
  std::array<char, kAssetCstrLen> asset;
  std::array<char, 7> cfi;
  std::array<char, 7> security_type;
  std::array<char, 31> unit_of_measure;
  std::array<char, 21> underlying;
  std::array<char, 4> strike_price_currency;
  std::array<char, kSymbolCstrLen> leg_raw_symbol;
  char instrument_class;
  char match_algorithm;
  std::uint8_t main_fraction;
  std::uint8_t price_display_format;
  std::uint8_t sub_fraction;
  std::uint8_t underlying_product;
  char security_update_action;
  std::uint8_t maturity_month;
  std::uint8_t maturity_day;
  std::uint8_t maturity_week;
  char user_defined_instrument;
  std::int8_t contract_multiplier_unit;
  std::int8_t flow_schedule_type;
  std::uint8_t tick_rule;
  char leg_instrument_class;
  char leg_side;
  std::array<std::byte, 17> reserved;
};

static_assert(sizeof(InstrumentDefMsg) == 520);
static_assert(offsetof(InstrumentDefMsg, ts_recv) == 16);
static_assert(offsetof(InstrumentDefMsg, min_price_increment) == 24);
static_assert(offsetof(InstrumentDefMsg, expiration) == 40);
static_assert(offsetof(InstrumentDefMsg, raw_instrument_id) == 112);
static_assert(offsetof(InstrumentDefMsg, leg_price) == 120);
static_assert(offsetof(InstrumentDefMsg, inst_attrib_value) == 136);
static_assert(offsetof(InstrumentDefMsg, appl_id) == 212);
static_assert(offsetof(InstrumentDefMsg, currency) == 224);
static_assert(offsetof(InstrumentDefMsg, raw_symbol) == 238);
static_assert(offsetof(InstrumentDefMsg, asset) == 335);
static_assert(offsetof(InstrumentDefMsg, leg_raw_symbol) == 416);
static_assert(offsetof(InstrumentDefMsg, instrument_class) == 487);
static_assert(offsetof(InstrumentDefMsg, leg_side) == 502);
static_assert(offsetof(InstrumentDefMsg, reserved) == 503);

/// The largest record layout this decoder knows, `ts_out` extension included.
/// Not a ceiling: a record may declare a longer length, and the declared
/// length is what advances the stream.
inline constexpr std::size_t kMaxKnownRecordLen = sizeof(InstrumentDefMsg) + sizeof(std::uint64_t);

template <typename T>
inline constexpr bool kIsDbnRecord =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && alignof(T) == 8;

static_assert(kIsDbnRecord<RecordHeader>);
static_assert(kIsDbnRecord<MboMsg>);
static_assert(kIsDbnRecord<TradeMsg>);
static_assert(kIsDbnRecord<Mbp10Msg>);
static_assert(kIsDbnRecord<StatusMsg>);
static_assert(kIsDbnRecord<InstrumentDefMsg>);
static_assert(kIsDbnRecord<BidAskPair>);

template <typename T>
inline constexpr std::uint8_t kLengthUnits = static_cast<std::uint8_t>(sizeof(T) / kLengthUnit);

static_assert(kLengthUnits<MboMsg> == 14);
static_assert(kLengthUnits<TradeMsg> == 12);
static_assert(kLengthUnits<Mbp10Msg> == 92);
static_assert(kLengthUnits<StatusMsg> == 10);
static_assert(kLengthUnits<InstrumentDefMsg> == 130);

inline constexpr std::uint8_t kMboLengthUnits = kLengthUnits<MboMsg>;

/// The declared size of a known rtype, or 0 if the rtype is not one we read.
[[nodiscard]] constexpr std::size_t record_size_for_rtype(std::uint8_t rtype) noexcept {
  switch (rtype) {
    case kRTypeMbo:
      return sizeof(MboMsg);
    case kRTypeTrade:
      return sizeof(TradeMsg);
    case kRTypeMbp10:
      return sizeof(Mbp10Msg);
    case kRTypeStatus:
      return sizeof(StatusMsg);
    case kRTypeInstrumentDef:
      return sizeof(InstrumentDefMsg);
    default:
      return 0;
  }
}

template <typename T>
struct RecordTraits;

template <>
struct RecordTraits<MboMsg> {
  static constexpr std::uint8_t kRType = kRTypeMbo;
};

template <>
struct RecordTraits<TradeMsg> {
  static constexpr std::uint8_t kRType = kRTypeTrade;
};

template <>
struct RecordTraits<Mbp10Msg> {
  static constexpr std::uint8_t kRType = kRTypeMbp10;
};

template <>
struct RecordTraits<StatusMsg> {
  static constexpr std::uint8_t kRType = kRTypeStatus;
};

template <>
struct RecordTraits<InstrumentDefMsg> {
  static constexpr std::uint8_t kRType = kRTypeInstrumentDef;
};

[[nodiscard]] constexpr std::size_t record_bytes(const RecordHeader& hd) noexcept {
  return static_cast<std::size_t>(hd.length) * kLengthUnit;
}

/// Typed view of a record, or nullptr if it is not one. `hd` must be
/// 8-aligned with the whole record present behind it.
template <typename T>
[[nodiscard]] const T* record_cast(const RecordHeader& hd) noexcept {
  static_assert(kIsDbnRecord<T>);
  if (hd.rtype != RecordTraits<T>::kRType || record_bytes(hd) < sizeof(T)) {
    return nullptr;
  }
  return reinterpret_cast<const T*>(&hd);
}

[[nodiscard]] constexpr Action action_of(const MboMsg& r) noexcept {
  return static_cast<Action>(r.action);
}

[[nodiscard]] constexpr Side side_of(const MboMsg& r) noexcept {
  return static_cast<Side>(r.side);
}

[[nodiscard]] constexpr bool has_flag(const MboMsg& r, std::uint8_t flag) noexcept {
  return (r.flags & flag) != 0;
}

/// Post-renormalization the F_LAST flag may arrive on a standalone
/// `action='N'` record: on 2026-08-05, 482,044 of 26,878,300 boundaries did.
[[nodiscard]] constexpr bool is_event_boundary(const MboMsg& r) noexcept {
  return has_flag(r, kFlagLast);
}

[[nodiscard]] constexpr bool is_undef_price(std::int64_t price) noexcept {
  return price == kUndefPrice;
}

[[nodiscard]] bool is_supported_dbn_version(std::uint8_t version) noexcept;
[[nodiscard]] const char* action_name(Action a) noexcept;
[[nodiscard]] const char* side_name(Side s) noexcept;

}  // namespace bookreplay

#endif  // BOOKREPLAY_DBN_HPP
