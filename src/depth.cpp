#include "bookreplay/depth.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace bookreplay {
namespace {

[[nodiscard]] std::array<std::int64_t, 6> values_of(const BidAskPair& level) noexcept {
  return {level.bid_px, level.bid_sz, level.bid_ct, level.ask_px, level.ask_sz, level.ask_ct};
}

constexpr std::array<const char*, 6> kFieldNames{"bid_px", "bid_sz", "bid_ct",
                                                 "ask_px", "ask_sz", "ask_ct"};

}  // namespace

namespace detail {

void throw_too_wide(const char* what, std::uint64_t value) {
  throw DepthError(std::string{"a level's "} + what +
                   " is too wide for mbp-10: " + std::to_string(value));
}

}  // namespace detail

Depth10 top_ten(const Book& book, std::uint32_t instrument_id) {
  Depth10 depth = kPaddedDepth;
  if (const Book::Bids* bids = book.bids(instrument_id)) {
    std::size_t i = 0;
    for (auto it = bids->begin(); it != bids->end() && i < kDepthLevels; ++it, ++i) {
      set_level(depth, Side::kBid, i, it->first, it->second.size, it->second.queue.size());
    }
  }
  if (const Book::Asks* asks = book.asks(instrument_id)) {
    std::size_t i = 0;
    for (auto it = asks->begin(); it != asks->end() && i < kDepthLevels; ++it, ++i) {
      set_level(depth, Side::kAsk, i, it->first, it->second.size, it->second.queue.size());
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

}  // namespace bookreplay
