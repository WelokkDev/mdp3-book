#ifndef BOOKREPLAY_ORDER_HPP
#define BOOKREPLAY_ORDER_HPP

#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bookreplay {

/// Thrown on a request the model cannot represent or cannot honour; a rejected
/// order is never half-accepted.
class ReplayError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using OrderId = std::uint64_t;
using OcoGroup = std::uint32_t;

inline constexpr OcoGroup kNoOcoGroup = 0;
inline constexpr std::int64_t kNotStarted = std::numeric_limits<std::int64_t>::min();
inline constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::max();

enum class OrderType : std::uint8_t {
  kMarket,
  kLimit,
  kStop,
  kStopLimit,
};

enum class LatencyClass : std::uint8_t {
  kOrderEntry,
  kProtectionArm,
};

enum class OrderStatus : std::uint8_t {
  kPending,
  kLive,
  kElected,
  kFilled,
  kCancelled,
  kOcoCancelled,
};

enum class FillReason : std::uint8_t {
  kMarket,
  kLimitThrough,
  kStopElected,
  kStopLimitThrough,
};

class Latency {
 public:
  Latency() = delete;

  constexpr Latency(std::int64_t order_entry_ns, std::int64_t protection_arm_ns,
                    std::int64_t cancel_ns)
      : order_entry_ns_(order_entry_ns),
        protection_arm_ns_(protection_arm_ns),
        cancel_ns_(cancel_ns) {
    if (order_entry_ns < 0 || protection_arm_ns < 0 || cancel_ns < 0) {
      throw ReplayError("latency parameters must be non-negative");
    }
  }

  [[nodiscard]] constexpr std::int64_t order_entry_ns() const noexcept { return order_entry_ns_; }

  [[nodiscard]] constexpr std::int64_t protection_arm_ns() const noexcept {
    return protection_arm_ns_;
  }

  [[nodiscard]] constexpr std::int64_t cancel_ns() const noexcept { return cancel_ns_; }

  [[nodiscard]] constexpr std::int64_t for_class(LatencyClass c) const noexcept {
    return c == LatencyClass::kProtectionArm ? protection_arm_ns_ : order_entry_ns_;
  }

 private:
  std::int64_t order_entry_ns_;
  std::int64_t protection_arm_ns_;
  std::int64_t cancel_ns_;
};

/// The venue's tick size is InstrumentDefMsg::min_price_increment on the
/// `definition` schema.
class TickScale {
 public:
  TickScale() = delete;

  constexpr explicit TickScale(std::int64_t tick_size) : tick_size_(tick_size) {
    if (tick_size <= 0) {
      throw ReplayError("tick size must be positive");
    }
  }

  [[nodiscard]] constexpr std::int64_t tick_size() const noexcept { return tick_size_; }

  [[nodiscard]] constexpr std::int64_t max_ticks() const noexcept {
    return (kUndefPrice - 1) / tick_size_;
  }

  [[nodiscard]] constexpr bool representable(std::int64_t ticks) const noexcept {
    return ticks <= max_ticks() && ticks >= -max_ticks();
  }

  [[nodiscard]] constexpr std::int64_t to_price(std::int64_t ticks) const {
    if (!representable(ticks)) {
      throw ReplayError("tick level does not fit a wire price");
    }
    return ticks * tick_size_;
  }

  // Integer division truncates toward zero.
  [[nodiscard]] constexpr std::int64_t to_ticks(std::int64_t price) const noexcept {
    const std::int64_t q = price / tick_size_;
    return (price % tick_size_ < 0) ? q - 1 : q;
  }

  [[nodiscard]] constexpr bool on_tick(std::int64_t price) const noexcept {
    return price % tick_size_ == 0;
  }

 private:
  std::int64_t tick_size_;
};

/// `live_from_ns` is the instant the caller decided, not the instant the order
/// reaches the venue: the engine adds the latency `latency` names.
struct Order {
  OrderId id = 0;
  OrderType type = OrderType::kMarket;
  Side side = Side::kNone;
  std::uint32_t qty = 0;
  std::int64_t trigger_ticks = 0;  ///< kStop and kStopLimit only
  std::int64_t limit_ticks = 0;    ///< kLimit and kStopLimit only
  std::int64_t live_from_ns = 0;
  OcoGroup oco_group = kNoOcoGroup;
  LatencyClass latency = LatencyClass::kOrderEntry;
};

struct Fill {
  std::uint64_t seq = 0;  ///< strictly increasing across a run
  OrderId order_id = 0;
  OcoGroup oco_group = kNoOcoGroup;
  std::int64_t ts_ns = 0;  ///< the print's instant on the replay clock
  std::int64_t ts_event = 0;
  std::int64_t price = 0;  ///< 1e-9 fixed point
  std::int64_t price_ticks = 0;
  std::uint32_t qty = 0;
  std::uint32_t remaining = 0;   ///< of that order, after this fill
  std::uint32_t print_size = 0;  ///< the electing or filling print's size
  std::uint32_t sequence = 0;    ///< the print's DBN sequence
  Side side = Side::kNone;       ///< ours
  Side aggressor = Side::kNone;  ///< kNone is an auction or implied print
  FillReason reason = FillReason::kMarket;
};

struct OrderView {
  OrderStatus status = OrderStatus::kPending;
  OrderType type = OrderType::kMarket;
  Side side = Side::kNone;
  OcoGroup oco_group = kNoOcoGroup;
  std::uint32_t qty = 0;
  std::uint32_t remaining = 0;
  std::uint32_t filled = 0;
  std::uint32_t oco_reduced = 0;
  std::int64_t effective_live_ns = 0;
  std::int64_t effective_cancel_ns = kNever;
  std::int64_t elected_ns = kNever;
  std::int64_t trigger_price = 0;
  std::int64_t limit_price = 0;
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  if (s == Side::kBid) {
    return Side::kAsk;
  }
  if (s == Side::kAsk) {
    return Side::kBid;
  }
  return Side::kNone;
}

[[nodiscard]] constexpr bool uses_trigger(OrderType t) noexcept {
  return t == OrderType::kStop || t == OrderType::kStopLimit;
}

[[nodiscard]] constexpr bool uses_limit(OrderType t) noexcept {
  return t == OrderType::kLimit || t == OrderType::kStopLimit;
}

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
  return s == OrderStatus::kFilled || s == OrderStatus::kCancelled ||
         s == OrderStatus::kOcoCancelled;
}

/// Throws ReplayError naming the offending field.
void validate(const Order& order, const TickScale& scale);

[[nodiscard]] const char* order_type_name(OrderType t) noexcept;
[[nodiscard]] const char* order_status_name(OrderStatus s) noexcept;
[[nodiscard]] const char* fill_reason_name(FillReason r) noexcept;
[[nodiscard]] const char* latency_class_name(LatencyClass c) noexcept;

}  // namespace bookreplay

#endif  // BOOKREPLAY_ORDER_HPP
