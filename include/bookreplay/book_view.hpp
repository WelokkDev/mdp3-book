// Read-only copies of what a book holds, the same shape whichever book they
// came from, so a test or a comparison can ask two books the same question.
// The two that return vectors allocate; they are for audits and tests.

#ifndef BOOKREPLAY_BOOK_VIEW_HPP
#define BOOKREPLAY_BOOK_VIEW_HPP

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace bookreplay {

using RestingView = Book::Resting;

struct LevelView {
  std::int64_t price = 0;
  std::uint64_t total = 0;
  std::uint64_t count = 0;
};

[[nodiscard]] std::optional<RestingView> resting_view(const Book& book, std::uint32_t instrument_id,
                                                      std::uint64_t order_id);

[[nodiscard]] std::optional<LevelView> level_view(const Book& book, std::uint32_t instrument_id,
                                                  Side side, std::int64_t price);

/// Best first; empty for an instrument holding no book.
[[nodiscard]] std::vector<LevelView> ladder_view(const Book& book, std::uint32_t instrument_id,
                                                 Side side);

/// Oldest first; empty where no level exists.
[[nodiscard]] std::vector<std::uint64_t> queue_view(const Book& book, std::uint32_t instrument_id,
                                                    Side side, std::int64_t price);

}  // namespace bookreplay

#endif  // BOOKREPLAY_BOOK_VIEW_HPP
