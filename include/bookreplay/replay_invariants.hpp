#ifndef BOOKREPLAY_REPLAY_INVARIANTS_HPP
#define BOOKREPLAY_REPLAY_INVARIANTS_HPP

#include "bookreplay/order.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bookreplay {

inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline void fnv_mix(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    h ^= (value >> (i * 8)) & 0xFFULL;
    h *= kFnvPrime;
  }
}

/// Fill carries padding, which is excluded, so the digest is comparable across
/// compilers.
[[nodiscard]] inline std::uint64_t fill_digest(std::span<const Fill> fills,
                                               std::uint64_t seed = kFnvOffset) noexcept {
  std::uint64_t h = seed;
  for (const Fill& f : fills) {
    fnv_mix(h, f.seq);
    fnv_mix(h, f.order_id);
    fnv_mix(h, f.oco_group);
    fnv_mix(h, static_cast<std::uint64_t>(f.ts_ns));
    fnv_mix(h, static_cast<std::uint64_t>(f.ts_event));
    fnv_mix(h, static_cast<std::uint64_t>(f.price));
    fnv_mix(h, static_cast<std::uint64_t>(f.price_ticks));
    fnv_mix(h, f.qty);
    fnv_mix(h, f.remaining);
    fnv_mix(h, f.print_size);
    fnv_mix(h, f.sequence);
    fnv_mix(h, static_cast<std::uint64_t>(static_cast<unsigned char>(f.side)));
    fnv_mix(h, static_cast<std::uint64_t>(static_cast<unsigned char>(f.aggressor)));
    fnv_mix(h, static_cast<std::uint64_t>(f.reason));
  }
  return h;
}

template <typename D>
concept ReplayLike = requires(D& d, const Order& o, OrderId id, std::int64_t ts) {
  { d.submit(o) };
  { d.cancel_at(id, ts) };
  { d.advance_to(ts) } -> std::convertible_to<std::span<const Fill>>;
  { d.order(id) } -> std::convertible_to<OrderView>;
};

enum class ReplayInvariant : std::uint8_t {
  kMonotoneFills = 1,
  kQuantityConservation = 2,
  kGroupLinkage = 3,
  kGroupTermination = 4,
  kCausality = 5,
  kPassiveAttribution = 6,
};

struct ReplayViolation {
  ReplayInvariant invariant{};
  const char* what = "";
  std::uint64_t fill_seq = 0;
  OrderId order_id = 0;
  OcoGroup oco_group = kNoOcoGroup;
  std::int64_t ts_ns = 0;
};

struct ReplayInvariantReport {
  std::uint64_t advances = 0;
  std::uint64_t fills = 0;
  std::uint64_t orders = 0;
  std::uint64_t monotone_violations = 0;
  std::uint64_t conservation_violations = 0;
  std::uint64_t linkage_violations = 0;
  std::uint64_t termination_violations = 0;
  std::uint64_t causality_violations = 0;
  std::uint64_t attribution_violations = 0;
  std::uint64_t digest = kFnvOffset;
  std::vector<ReplayViolation> violations;

  /// The first violation's fill and the fills preceding it, in order.
  std::vector<Fill> first_violation_context;

  [[nodiscard]] bool ok() const noexcept {
    return monotone_violations == 0 && conservation_violations == 0 && linkage_violations == 0 &&
           termination_violations == 0 && causality_violations == 0 && attribution_violations == 0;
  }
};

/// Effective instants are recomputed from the caller's own Latency, never read
/// back from the driver.
template <ReplayLike D>
class ReplayHarness {
 public:
  static constexpr std::size_t kContextDepth = 8;

  struct Options {
    std::size_t max_violations = 16;
  };

  ReplayHarness(D& driver, Latency latency, Options opts = {})
      : driver_(&driver), latency_(latency), opts_(opts) {}

  void submit(const Order& order) {
    Tracked t;
    t.order = order;
    t.effective_live_ns = order.live_from_ns + latency_.for_class(order.latency);
    tracked_.push_back(t);
    ++report_.orders;
    driver_->submit(order);
  }

  void cancel_at(OrderId id, std::int64_t request_ts_ns) {
    if (Tracked* t = find(id)) {
      t->effective_cancel_ns =
          std::min(t->effective_cancel_ns, request_ts_ns + latency_.cancel_ns());
    }
    driver_->cancel_at(id, request_ts_ns);
  }

  std::span<const Fill> advance_to(std::int64_t ts_ns) {
    const std::span<const Fill> fills = driver_->advance_to(ts_ns);
    ++report_.advances;
    for (const Fill& f : fills) {
      check_fill(f, ts_ns);
    }
    report_.digest = fill_digest(fills, report_.digest);
    check_state();
    return fills;
  }

  [[nodiscard]] const ReplayInvariantReport& report() const noexcept { return report_; }

  [[nodiscard]] bool ok() const noexcept { return report_.ok(); }

 private:
  struct Tracked {
    Order order{};
    std::int64_t effective_live_ns = 0;
    std::int64_t effective_cancel_ns = kNever;
    std::uint32_t filled = 0;
  };

  [[nodiscard]] Tracked* find(OrderId id) {
    for (Tracked& t : tracked_) {
      if (t.order.id == id) {
        return &t;
      }
    }
    return nullptr;
  }

  void check_fill(const Fill& f, std::int64_t target) {
    ++report_.fills;
    push_context(f);

    if (f.seq <= last_seq_ || f.ts_ns < last_ts_ || f.ts_ns > target) {
      ++report_.monotone_violations;
      add(ReplayInvariant::kMonotoneFills, "fill is out of order or past the requested instant", f);
    }
    last_seq_ = f.seq;
    last_ts_ = std::max(last_ts_, f.ts_ns);

    Tracked* t = find(f.order_id);
    if (t == nullptr) {
      ++report_.conservation_violations;
      add(ReplayInvariant::kQuantityConservation, "fill names an order that was never submitted",
          f);
      return;
    }

    if (f.ts_ns < t->effective_live_ns || f.ts_ns >= t->effective_cancel_ns) {
      ++report_.causality_violations;
      add(ReplayInvariant::kCausality, "fill landed outside the order's live window", f);
    }

    if (f.reason == FillReason::kLimitThrough || f.reason == FillReason::kStopLimitThrough) {
      if (f.aggressor != opposite(f.side)) {
        ++report_.attribution_violations;
        add(ReplayInvariant::kPassiveAttribution, "passive fill names no opposing aggressor", f);
      }
    }

    t->filled += f.qty;
    if (t->filled > t->order.qty || f.qty == 0) {
      ++report_.conservation_violations;
      add(ReplayInvariant::kQuantityConservation, "order filled past its submitted quantity", f);
    }
  }

  void check_state() {
    for (const Tracked& t : tracked_) {
      const OrderView v = driver_->order(t.order.id);
      if (v.filled + v.oco_reduced + v.remaining != t.order.qty) {
        ++report_.conservation_violations;
        add_order(ReplayInvariant::kQuantityConservation,
                  "filled + oco_reduced + remaining does not equal the submitted quantity", t);
      }
    }
    check_groups();
  }

  void check_groups() {
    for (const Tracked& seed : tracked_) {
      const OcoGroup group = seed.order.oco_group;
      if (group == kNoOcoGroup || !first_of_group(group, seed.order.id)) {
        continue;
      }

      bool have_reference = false;
      std::uint32_t reference = 0;
      bool any_filled = false;
      bool any_live = false;
      bool cancelled_with_remainder = false;

      for (const Tracked& t : tracked_) {
        if (t.order.oco_group != group) {
          continue;
        }
        const OrderView v = driver_->order(t.order.id);
        if (v.status == OrderStatus::kFilled) {
          any_filled = true;
        }
        if (v.status == OrderStatus::kOcoCancelled && v.remaining > 0) {
          cancelled_with_remainder = true;
        }
        if (is_terminal(v.status)) {
          continue;
        }
        any_live = true;
        const std::uint32_t consumed = t.order.qty - v.remaining;
        if (!have_reference) {
          have_reference = true;
          reference = consumed;
        } else if (consumed != reference) {
          ++report_.linkage_violations;
          add_order(ReplayInvariant::kGroupLinkage,
                    "live members of one OCO group disagree on how much has been consumed", t);
        }
      }

      if (any_filled && any_live) {
        ++report_.termination_violations;
        add_order(ReplayInvariant::kGroupTermination,
                  "a member filled its whole quantity but a sibling is still live", seed);
      }
      if (cancelled_with_remainder && !any_filled) {
        ++report_.termination_violations;
        add_order(
            ReplayInvariant::kGroupTermination,
            "a sibling was OCO-cancelled with quantity remaining although no member filled out",
            seed);
      }
    }
  }

  [[nodiscard]] bool first_of_group(OcoGroup group, OrderId id) const {
    for (const Tracked& t : tracked_) {
      if (t.order.oco_group == group) {
        return t.order.id == id;
      }
    }
    return false;
  }

  [[nodiscard]] ReplayViolation make(ReplayInvariant inv, const char* what) const {
    ReplayViolation v;
    v.invariant = inv;
    v.what = what;
    return v;
  }

  void add(ReplayInvariant inv, const char* what, const Fill& f) {
    ReplayViolation v = make(inv, what);
    v.fill_seq = f.seq;
    v.order_id = f.order_id;
    v.oco_group = f.oco_group;
    v.ts_ns = f.ts_ns;
    store(v);
  }

  void add_order(ReplayInvariant inv, const char* what, const Tracked& t) {
    ReplayViolation v = make(inv, what);
    v.order_id = t.order.id;
    v.oco_group = t.order.oco_group;
    store(v);
  }

  void store(const ReplayViolation& v) {
    if (report_.violations.empty()) {
      capture_context();
    }
    if (report_.violations.size() < opts_.max_violations) {
      report_.violations.push_back(v);
    }
  }

  void push_context(const Fill& f) {
    ring_[ring_next_] = f;
    ring_next_ = (ring_next_ + 1) % kContextDepth;
    if (ring_filled_ < kContextDepth) {
      ++ring_filled_;
    }
  }

  void capture_context() {
    report_.first_violation_context.reserve(ring_filled_);
    const std::size_t start = (ring_next_ + kContextDepth - ring_filled_) % kContextDepth;
    for (std::size_t i = 0; i < ring_filled_; ++i) {
      report_.first_violation_context.push_back(ring_[(start + i) % kContextDepth]);
    }
  }

  D* driver_;
  Latency latency_;
  Options opts_;
  ReplayInvariantReport report_{};
  std::vector<Tracked> tracked_;

  std::uint64_t last_seq_ = 0;
  std::int64_t last_ts_ = kNotStarted;

  std::array<Fill, kContextDepth> ring_{};
  std::size_t ring_next_ = 0;
  std::size_t ring_filled_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_REPLAY_INVARIANTS_HPP
