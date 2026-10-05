#include "bookreplay/mbp10_diff.hpp"

namespace bookreplay {

const char* divergence_name(DivergenceKind kind) noexcept {
  switch (kind) {
    case DivergenceKind::kLevels:
      return "levels";
    case DivergenceKind::kMissingRecord:
      return "missing_record";
    case DivergenceKind::kUnclaimedRecord:
      return "unclaimed_record";
    case DivergenceKind::kTradeUnmatched:
      return "trade_unmatched";
    case DivergenceKind::kTradeLevels:
      return "trade_levels";
    case DivergenceKind::kSnapshotMissing:
      return "snapshot_missing";
    case DivergenceKind::kSnapshotLevels:
      return "snapshot_levels";
    case DivergenceKind::kSnapshotUnclaimed:
      return "snapshot_unclaimed";
  }
  return "<unknown>";
}

}  // namespace bookreplay
