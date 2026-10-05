#include "bookreplay/book_view.hpp"

namespace bookreplay {
namespace {

template <typename LevelMap>
void append_levels(const LevelMap* levels, std::vector<LevelView>& out) {
  if (levels == nullptr) {
    return;
  }
  out.reserve(levels->size());
  for (const auto& [price, lvl] : *levels) {
    out.push_back(LevelView{price, lvl.size, lvl.queue.size()});
  }
}

}  // namespace

std::optional<RestingView> resting_view(const Book& book, std::uint32_t instrument_id,
                                        std::uint64_t order_id) {
  const Book::Resting* o = book.order(instrument_id, order_id);
  if (o == nullptr) {
    return std::nullopt;
  }
  return *o;
}

std::optional<LevelView> level_view(const Book& book, std::uint32_t instrument_id, Side side,
                                    std::int64_t price) {
  const Book::Level* lvl = book.level(instrument_id, side, price);
  if (lvl == nullptr) {
    return std::nullopt;
  }
  return LevelView{price, lvl->size, lvl->queue.size()};
}

std::vector<LevelView> ladder_view(const Book& book, std::uint32_t instrument_id, Side side) {
  std::vector<LevelView> out;
  if (side == Side::kBid) {
    append_levels(book.bids(instrument_id), out);
  } else if (side == Side::kAsk) {
    append_levels(book.asks(instrument_id), out);
  }
  return out;
}

std::vector<std::uint64_t> queue_view(const Book& book, std::uint32_t instrument_id, Side side,
                                      std::int64_t price) {
  const Book::Level* lvl = book.level(instrument_id, side, price);
  if (lvl == nullptr) {
    return {};
  }
  return {lvl->queue.begin(), lvl->queue.end()};
}

}  // namespace bookreplay
