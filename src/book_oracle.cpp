#include "bookreplay/book_oracle.hpp"

namespace bookreplay {

const char* oracle_difference_name(OracleDifferenceKind kind) noexcept {
  switch (kind) {
    case OracleDifferenceKind::kResting:
      return "resting";
    case OracleDifferenceKind::kOrder:
      return "order";
    case OracleDifferenceKind::kQueueAhead:
      return "queue_ahead";
    case OracleDifferenceKind::kLevelTotal:
      return "level_total";
    case OracleDifferenceKind::kLevelCount:
      return "level_count";
    case OracleDifferenceKind::kTouch:
      return "touch";
    case OracleDifferenceKind::kTopTen:
      return "top_ten";
    case OracleDifferenceKind::kInstruments:
      return "instruments";
    case OracleDifferenceKind::kLadder:
      return "ladder";
    case OracleDifferenceKind::kQueue:
      return "queue";
    case OracleDifferenceKind::kCounter:
      return "counter";
  }
  return "<unknown>";
}

}  // namespace bookreplay
