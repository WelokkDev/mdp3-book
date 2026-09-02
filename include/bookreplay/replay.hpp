#ifndef BOOKREPLAY_REPLAY_HPP
#define BOOKREPLAY_REPLAY_HPP

#include "bookreplay/order.hpp"
#include "bookreplay/trade_source.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bookreplay {

enum class NoAggressorPolicy : std::uint8_t {
  kNeitherSide,
  kBothSides,
};

enum class FillSizePolicy : std::uint8_t {
  kPrintCapped,
  kFullRemaining,
};

struct ReplayConfig {
  Latency latency;
  TickScale scale;
  NoAggressorPolicy no_aggressor = NoAggressorPolicy::kNeitherSide;
  FillSizePolicy fill_size = FillSizePolicy::kPrintCapped;
  std::size_t reserve_live_orders = 64;
  /// A hint, not a cap: growth past it is counted in ReplayStats::reallocations.
  std::size_t reserve_fills_per_advance = 256;
  bool retain_ticks = false;
};

struct ReplayStats {
  std::uint64_t ticks = 0;
  std::uint64_t no_aggressor_ticks = 0;
  /// Prints through a resting order's price that kNeitherSide refused.
  std::uint64_t no_aggressor_passive_skips = 0;
  /// Contracts filled a tick off the print because the other side aggressed it.
  std::uint64_t tick_charged_qty = 0;
  std::uint64_t elections = 0;
  /// Stops whose trigger was already through the last print when they armed.
  std::uint64_t stop_entry_rejects = 0;
  std::uint64_t fills = 0;
  std::uint64_t partial_fills = 0;
  std::uint64_t oco_reductions = 0;
  std::uint64_t oco_cancels = 0;
  std::uint64_t cancels_applied = 0;
  /// Orders whose effective live instant was already behind the cursor at
  /// submit; accepted, not clamped.
  std::uint64_t late_arm_orders = 0;
  std::uint64_t late_arm_ns_total = 0;
  /// Growth of the per-advance fill and tick buffers.
  std::uint64_t reallocations = 0;
};

class Replay {
 public:
  Replay(std::unique_ptr<TradeSource> source, ReplayConfig config);

  ~Replay();
  Replay(Replay&&) noexcept;
  Replay& operator=(Replay&&) noexcept;
  Replay(const Replay&) = delete;
  Replay& operator=(const Replay&) = delete;

  /// Goes live at `order.live_from_ns` plus the latency its LatencyClass names.
  /// Throws ReplayError on a duplicate id or a malformed order. An effective
  /// live instant already behind the cursor is accepted and counted.
  void submit(const Order& order);

  /// Requested as of the current cursor; effective at cursor + cancel_ns.
  void cancel(OrderId id);

  /// Requested at `request_ts_ns`, which may not precede the cursor.
  void cancel_at(OrderId id, std::int64_t request_ts_ns);

  /// Every fill in (previous cursor, ts_ns], in order. Monotonic: an earlier
  /// timestamp throws ReplayError, the same timestamp is an empty no-op. The
  /// span is valid only until the next call.
  [[nodiscard]] std::span<const Fill> advance_to(std::int64_t ts_ns);

  /// The prints the last advance_to consumed, in stream order; empty unless
  /// ReplayConfig::retain_ticks is set. Valid only until the next call.
  [[nodiscard]] std::span<const Tick> last_ticks() const noexcept;

  [[nodiscard]] std::int64_t now_ns() const noexcept;
  [[nodiscard]] bool exhausted() const noexcept;

  /// Throws ReplayError on an id that was never submitted.
  [[nodiscard]] OrderView order(OrderId id) const;

  [[nodiscard]] const ReplayConfig& config() const noexcept;
  [[nodiscard]] const ReplayStats& stats() const noexcept;
  [[nodiscard]] const TradeSourceStats& source_stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_REPLAY_HPP
