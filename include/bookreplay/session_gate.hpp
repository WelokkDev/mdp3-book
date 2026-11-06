#ifndef BOOKREPLAY_SESSION_GATE_HPP
#define BOOKREPLAY_SESSION_GATE_HPP

#include "bookreplay/dbn.hpp"
#include "bookreplay/order.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bookreplay {

/// One instrument's stretches of continuous trading on the venue's clock, read
/// from the status schema, for something that has to know which events an
/// order could have traded in.
///
/// A status record and an event can share a `ts_event`, and which goes first
/// depends on which way the status points. The record that opens trading
/// shares its instant with the opening uncross, which is not continuous
/// trading, so it applies before that instant's events and they are the
/// uncross. The record that halts trading shares its instant with the event
/// that tripped the halt, which was matched like any other, so it applies
/// after that instant's events. Applying a halt first would void a resting
/// order just ahead of the prints most likely to have filled it.
class SessionGate {
 public:
  struct Change {
    std::int64_t ts_event = 0;
    bool trading = false;
  };

  /// `statuses` in any order; only `instrument_id`'s are kept. `is_trading`
  /// decides, as in `session_state_of`. `trading_at_start` is the state before
  /// the first of them.
  SessionGate(std::span<const StatusMsg> statuses, std::uint32_t instrument_id,
              bool trading_at_start);

  /// The next change that takes effect before an event at `ts_event`, which is
  /// then the state; empty once none is left before it. Every change flips
  /// the state: a status record that repeats it is not one.
  [[nodiscard]] std::optional<Change> take_change_before(std::int64_t ts_event);

  [[nodiscard]] bool trading() const noexcept { return trading_; }

  /// Whether an event at `ts_event` is continuous trading, once every change
  /// before it has been taken.
  [[nodiscard]] bool continuous_at(std::int64_t ts_event) const noexcept {
    return trading_ && ts_event > since_;
  }

  /// Status records kept for the instrument, whether or not they changed its
  /// state. 0 means the stream never described it.
  [[nodiscard]] std::size_t records() const noexcept { return records_; }

 private:
  std::vector<Change> changes_;
  std::size_t next_ = 0;
  std::size_t records_ = 0;
  std::int64_t since_ = kNotStarted;
  bool trading_ = false;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_SESSION_GATE_HPP
