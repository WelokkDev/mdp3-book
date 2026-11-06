#include "bookreplay/session_gate.hpp"

#include <algorithm>

namespace bookreplay {

SessionGate::SessionGate(std::span<const StatusMsg> statuses, std::uint32_t instrument_id,
                         bool trading_at_start)
    : trading_(trading_at_start) {
  std::vector<Change> described;
  for (const StatusMsg& status : statuses) {
    if (status.hd.instrument_id == instrument_id) {
      described.push_back(
          Change{static_cast<std::int64_t>(status.hd.ts_event), status.is_trading == kTriStateYes});
    }
  }
  records_ = described.size();
  std::stable_sort(described.begin(), described.end(),
                   [](const Change& a, const Change& b) { return a.ts_event < b.ts_event; });
  bool state = trading_at_start;
  for (const Change& change : described) {
    if (change.trading != state) {
      changes_.push_back(change);
      state = change.trading;
    }
  }
}

std::optional<SessionGate::Change> SessionGate::take_change_before(std::int64_t ts_event) {
  if (next_ == changes_.size()) {
    return std::nullopt;
  }
  const Change change = changes_[next_];
  if (change.ts_event > ts_event || (change.ts_event == ts_event && !change.trading)) {
    return std::nullopt;
  }
  ++next_;
  trading_ = change.trading;
  if (change.trading) {
    since_ = change.ts_event;
  }
  return change;
}

}  // namespace bookreplay
