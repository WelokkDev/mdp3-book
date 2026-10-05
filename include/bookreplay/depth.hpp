// A book's top ten levels per side in mbp-10's own layout, and the comparison
// of two such ladders.

#ifndef BOOKREPLAY_DEPTH_HPP
#define BOOKREPLAY_DEPTH_HPP

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace bookreplay {

/// Thrown when the book holds a value mbp-10 has no room for.
class DepthError : public BookreplayError {
 public:
  using BookreplayError::BookreplayError;
};

inline constexpr std::size_t kDepthLevels = 10;

using Depth10 = std::array<BidAskPair, kDepthLevels>;

/// mbp-10 pads a level that does not exist rather than shortening its array.
inline constexpr BidAskPair kPaddedLevel{kUndefPrice, kUndefPrice, 0, 0, 0, 0};

inline constexpr Depth10 kPaddedDepth = [] {
  Depth10 depth{};
  depth.fill(kPaddedLevel);
  return depth;
}();

namespace detail {

[[noreturn]] void throw_too_wide(const char* what, std::uint64_t value);

}  // namespace detail

/// Writes one side of one level. Throws DepthError if the total or the order
/// count is too wide for the wire's 32 bits. Inline because a top ten calls it
/// twenty times at every event boundary.
inline void set_level(Depth10& depth, Side side, std::size_t level, std::int64_t price,
                      std::uint64_t total, std::uint64_t count) {
  constexpr std::uint64_t kWidest = std::numeric_limits<std::uint32_t>::max();
  if (total > kWidest) {
    detail::throw_too_wide("total", total);
  }
  if (count > kWidest) {
    detail::throw_too_wide("order count", count);
  }
  BidAskPair& pair = depth[level];
  if (side == Side::kBid) {
    pair.bid_px = price;
    pair.bid_sz = static_cast<std::uint32_t>(total);
    pair.bid_ct = static_cast<std::uint32_t>(count);
  } else {
    pair.ask_px = price;
    pair.ask_sz = static_cast<std::uint32_t>(total);
    pair.ask_ct = static_cast<std::uint32_t>(count);
  }
}

/// The book's top ten levels per side in mbp-10's own layout: bids
/// descending, asks ascending, the rest padded.
[[nodiscard]] Depth10 top_ten(const Book& book, std::uint32_t instrument_id);

/// A book whose top ten can be taken. `Book` qualifies through the overload
/// above; `FastBook` and the tests' deliberately wrong books bring their own.
template <typename B>
concept DepthBook = requires(const B& b, std::uint32_t instrument_id) {
  { top_ten(b, instrument_id) } -> std::convertible_to<Depth10>;
};

/// The first of the sixty values two ladders disagree on.
struct DepthMismatch {
  std::size_t level = 0;   ///< 0-based
  const char* field = "";  ///< "bid_px", "ask_ct", and so on
  std::int64_t ours = 0;
  std::int64_t theirs = 0;
};

/// Walks level by level, bids before asks and price before size before count,
/// so what comes back is the highest disagreement in the ladder.
[[nodiscard]] std::optional<DepthMismatch> first_mismatch(const Depth10& ours,
                                                          const Depth10& theirs) noexcept;

[[nodiscard]] inline bool same_depth(const Depth10& ours, const Depth10& theirs) noexcept {
  return !first_mismatch(ours, theirs).has_value();
}

}  // namespace bookreplay

#endif  // BOOKREPLAY_DEPTH_HPP
