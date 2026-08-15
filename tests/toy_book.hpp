#ifndef BOOKREPLAY_TESTS_TOY_BOOK_HPP
#define BOOKREPLAY_TESTS_TOY_BOOK_HPP

#include "bookreplay/dbn.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

namespace bookreplay::testing {

class ToyBookBase {
 public:
  using Key = std::pair<std::uint32_t, std::uint64_t>;

  struct Order {
    Side side = Side::kNone;
    std::int64_t price = 0;
    std::uint64_t size = 0;
  };

  [[nodiscard]] std::uint64_t mutation_count() const noexcept { return mutations_; }

  [[nodiscard]] bool contains(std::uint32_t instrument_id, std::uint64_t order_id) const {
    return orders_.find(Key{instrument_id, order_id}) != orders_.end();
  }

  [[nodiscard]] std::int64_t best_bid(std::uint32_t instrument_id) const {
    const auto it = bids_.find(instrument_id);
    if (it == bids_.end() || it->second.empty()) {
      return kUndefPrice;
    }
    return it->second.rbegin()->first;
  }

  [[nodiscard]] std::int64_t best_ask(std::uint32_t instrument_id) const {
    const auto it = asks_.find(instrument_id);
    if (it == asks_.end() || it->second.empty()) {
      return kUndefPrice;
    }
    return it->second.begin()->first;
  }

 protected:
  using Levels = std::map<std::int64_t, std::uint64_t>;

  void insert_order(const MboMsg& rec) {
    const Key key{rec.hd.instrument_id, rec.order_id};
    const Side side = side_of(rec);
    orders_[key] = Order{side, rec.price, rec.size};
    level_for(side, rec.hd.instrument_id)[rec.price] += rec.size;
  }

  void erase_order(const MboMsg& rec) {
    const auto it = orders_.find(Key{rec.hd.instrument_id, rec.order_id});
    if (it == orders_.end()) {
      return;
    }
    remove_size(it->second.side, rec.hd.instrument_id, it->second.price, it->second.size);
    orders_.erase(it);
  }

  void clear_instrument(std::uint32_t instrument_id) {
    for (auto it = orders_.begin(); it != orders_.end();) {
      it = (it->first.first == instrument_id) ? orders_.erase(it) : std::next(it);
    }
    bids_.erase(instrument_id);
    asks_.erase(instrument_id);
  }

  void bump() noexcept { ++mutations_; }

 private:
  Levels& level_for(Side side, std::uint32_t instrument_id) {
    return (side == Side::kBid) ? bids_[instrument_id] : asks_[instrument_id];
  }

  void remove_size(Side side, std::uint32_t instrument_id, std::int64_t price, std::uint64_t size) {
    Levels& levels = level_for(side, instrument_id);
    const auto it = levels.find(price);
    if (it == levels.end()) {
      return;
    }
    it->second = (it->second > size) ? (it->second - size) : 0;
    if (it->second == 0) {
      levels.erase(it);
    }
  }

  std::map<Key, Order> orders_;
  std::map<std::uint32_t, Levels> bids_;
  std::map<std::uint32_t, Levels> asks_;
  std::uint64_t mutations_ = 0;
};

class ToyBook : public ToyBookBase {
 public:
  void apply(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
        bump();
        insert_order(rec);
        return;
      case Action::kCancel:
        bump();
        erase_order(rec);
        return;
      case Action::kModify:
        bump();
        erase_order(rec);
        insert_order(rec);
        return;
      case Action::kClear:
        bump();
        clear_instrument(rec.hd.instrument_id);
        return;
      case Action::kTrade:
      case Action::kFill:
      case Action::kNone:
        return;
    }
  }
};

class FillAsDeltaBook : public ToyBookBase {
 public:
  void apply(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
        bump();
        insert_order(rec);
        return;
      case Action::kCancel:
        bump();
        erase_order(rec);
        return;
      case Action::kModify:
        bump();
        erase_order(rec);
        insert_order(rec);
        return;
      case Action::kClear:
        bump();
        clear_instrument(rec.hd.instrument_id);
        return;
      case Action::kFill:
        bump();
        insert_order(rec);
        return;
      case Action::kTrade:
      case Action::kNone:
        return;
    }
  }
};

class FillAsDeleteBook : public ToyBookBase {
 public:
  void apply(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
        bump();
        insert_order(rec);
        return;
      case Action::kCancel:
        bump();
        erase_order(rec);
        return;
      case Action::kModify:
        bump();
        erase_order(rec);
        insert_order(rec);
        return;
      case Action::kClear:
        bump();
        clear_instrument(rec.hd.instrument_id);
        return;
      case Action::kFill:
        erase_order(rec);
        return;
      case Action::kTrade:
      case Action::kNone:
        return;
    }
  }
};

class CompensatingCountBook : public ToyBookBase {
 public:
  void apply(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
        bump();
        insert_order(rec);
        return;
      case Action::kCancel:
        if (contains(rec.hd.instrument_id, rec.order_id)) {
          bump();
        }
        erase_order(rec);
        return;
      case Action::kModify:
        bump();
        erase_order(rec);
        bump();
        insert_order(rec);
        return;
      case Action::kClear:
        bump();
        clear_instrument(rec.hd.instrument_id);
        return;
      case Action::kTrade:
      case Action::kFill:
      case Action::kNone:
        return;
    }
  }
};

class TradeMutatesBook : public ToyBookBase {
 public:
  void apply(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
        bump();
        insert_order(rec);
        return;
      case Action::kCancel:
        bump();
        erase_order(rec);
        return;
      case Action::kModify:
        bump();
        erase_order(rec);
        insert_order(rec);
        return;
      case Action::kClear:
        bump();
        clear_instrument(rec.hd.instrument_id);
        return;
      case Action::kTrade:
        bump();
        erase_order(rec);
        return;
      case Action::kFill:
      case Action::kNone:
        return;
    }
  }
};

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_TOY_BOOK_HPP
