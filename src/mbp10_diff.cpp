#include "bookreplay/mbp10_diff.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace bookreplay {
namespace {

[[nodiscard]] std::array<std::int64_t, 6> values_of(const BidAskPair& level) noexcept {
  return {level.bid_px, level.bid_sz, level.bid_ct, level.ask_px, level.ask_sz, level.ask_ct};
}

constexpr std::array<const char*, 6> kFieldNames{"bid_px", "bid_sz", "bid_ct",
                                                 "ask_px", "ask_sz", "ask_ct"};

[[nodiscard]] std::uint32_t narrow(std::uint64_t value, const char* what) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw Mbp10DiffError(std::string{"a level's "} + what +
                         " is too wide for mbp-10: " + std::to_string(value));
  }
  return static_cast<std::uint32_t>(value);
}

}  // namespace

Depth10 top_ten(const Book& book, std::uint32_t instrument_id) {
  Depth10 depth = kPaddedDepth;
  if (const Book::Bids* bids = book.bids(instrument_id)) {
    std::size_t i = 0;
    for (auto it = bids->begin(); it != bids->end() && i < kDepthLevels; ++it, ++i) {
      depth[i].bid_px = it->first;
      depth[i].bid_sz = narrow(it->second.size, "total");
      depth[i].bid_ct = narrow(it->second.queue.size(), "order count");
    }
  }
  if (const Book::Asks* asks = book.asks(instrument_id)) {
    std::size_t i = 0;
    for (auto it = asks->begin(); it != asks->end() && i < kDepthLevels; ++it, ++i) {
      depth[i].ask_px = it->first;
      depth[i].ask_sz = narrow(it->second.size, "total");
      depth[i].ask_ct = narrow(it->second.queue.size(), "order count");
    }
  }
  return depth;
}

std::optional<DepthMismatch> first_mismatch(const Depth10& ours, const Depth10& theirs) noexcept {
  for (std::size_t level = 0; level < kDepthLevels; ++level) {
    const std::array<std::int64_t, 6> a = values_of(ours[level]);
    const std::array<std::int64_t, 6> b = values_of(theirs[level]);
    for (std::size_t field = 0; field < a.size(); ++field) {
      if (a[field] != b[field]) {
        return DepthMismatch{level, kFieldNames[field], a[field], b[field]};
      }
    }
  }
  return std::nullopt;
}

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
