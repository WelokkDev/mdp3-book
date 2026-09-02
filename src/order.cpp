#include "bookreplay/order.hpp"

#include <string>

namespace bookreplay {

namespace {

void require_price(const TickScale& scale, std::int64_t ticks, const char* what) {
  if (ticks == 0) {
    throw ReplayError(std::string("order ") + what + " price is unset");
  }
  if (!scale.representable(ticks)) {
    throw ReplayError(std::string("order ") + what + " does not fit a wire price");
  }
}

}  // namespace

void validate(const Order& order, const TickScale& scale) {
  if (order.qty == 0) {
    throw ReplayError("order qty must be at least 1");
  }
  if (order.side != Side::kBid && order.side != Side::kAsk) {
    throw ReplayError("order side must be bid or ask");
  }
  if (order.live_from_ns < 0) {
    throw ReplayError("order live_from_ns must be a non-negative unix nanosecond instant");
  }
  if (uses_trigger(order.type)) {
    require_price(scale, order.trigger_ticks, "trigger");
  } else if (order.trigger_ticks != 0) {
    throw ReplayError(std::string("a ") + order_type_name(order.type) +
                      " order must leave trigger_ticks at 0");
  }
  if (uses_limit(order.type)) {
    require_price(scale, order.limit_ticks, "limit");
  } else if (order.limit_ticks != 0) {
    throw ReplayError(std::string("a ") + order_type_name(order.type) +
                      " order must leave limit_ticks at 0");
  }
  if (order.type == OrderType::kStopLimit) {
    const bool inverted = order.side == Side::kBid ? order.limit_ticks < order.trigger_ticks
                                                   : order.limit_ticks > order.trigger_ticks;
    if (inverted) {
      throw ReplayError("stop-limit cap is on the wrong side of its trigger");
    }
  }
}

const char* order_type_name(OrderType t) noexcept {
  switch (t) {
    case OrderType::kMarket:
      return "market";
    case OrderType::kLimit:
      return "limit";
    case OrderType::kStop:
      return "stop";
    case OrderType::kStopLimit:
      return "stop_limit";
  }
  return "?";
}

const char* order_status_name(OrderStatus s) noexcept {
  switch (s) {
    case OrderStatus::kPending:
      return "pending";
    case OrderStatus::kLive:
      return "live";
    case OrderStatus::kElected:
      return "elected";
    case OrderStatus::kFilled:
      return "filled";
    case OrderStatus::kCancelled:
      return "cancelled";
    case OrderStatus::kOcoCancelled:
      return "oco_cancelled";
    case OrderStatus::kRejected:
      return "rejected";
  }
  return "?";
}

const char* fill_reason_name(FillReason r) noexcept {
  switch (r) {
    case FillReason::kMarket:
      return "market";
    case FillReason::kLimitThrough:
      return "limit_through";
    case FillReason::kStopElected:
      return "stop_elected";
    case FillReason::kStopLimitThrough:
      return "stop_limit_through";
  }
  return "?";
}

const char* latency_class_name(LatencyClass c) noexcept {
  switch (c) {
    case LatencyClass::kOrderEntry:
      return "order_entry";
    case LatencyClass::kProtectionArm:
      return "protection_arm";
  }
  return "?";
}

}  // namespace bookreplay
