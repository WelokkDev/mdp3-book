#ifndef BOOKREPLAY_NAKED_WINDOW_HPP
#define BOOKREPLAY_NAKED_WINDOW_HPP

#include "bookreplay/order.hpp"
#include "bookreplay/trade_source.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bookreplay {

/// The gap between an entry filling and its protective legs reaching the venue.
struct NakedWindowQuery {
  std::int64_t entry_fill_ts = 0;
  std::int64_t window_ns = 0;  ///< protection_arm_ns; the window is half-open
  std::int64_t entry_price = 0;
  std::int64_t stop_price = 0;
  Side position_side = Side::kNone;  ///< kBid is long
};

struct NakedWindowResult {
  std::uint64_t ticks = 0;
  std::uint64_t no_aggressor_ticks = 0;
  std::uint64_t volume = 0;
  bool stop_reached = false;
  std::int64_t first_reach_ts = kNever;  ///< kNever unless stop_reached
  std::int64_t worst_price = 0;          ///< the entry price when nothing adverse printed
  std::int64_t mae_ticks = 0;            ///< max adverse excursion, floored at zero
  std::int64_t window_end_ns = 0;
};

class NakedWindowAccumulator {
 public:
  NakedWindowAccumulator(TickScale scale, const NakedWindowQuery& query)
      : scale_(scale), query_(query) {
    if (query.window_ns < 0) {
      throw ReplayError("naked window length must be non-negative");
    }
    if (query.position_side != Side::kBid && query.position_side != Side::kAsk) {
      throw ReplayError("naked window position side must be bid or ask");
    }
    if (query.stop_price == 0) {
      throw ReplayError("naked window stop price is unset");
    }
    const bool wrong_side = query.position_side == Side::kBid
                                ? query.stop_price > query.entry_price
                                : query.stop_price < query.entry_price;
    if (wrong_side) {
      throw ReplayError("a protective stop must sit on the adverse side of the entry price");
    }
    result_.worst_price = query.entry_price;
    result_.window_end_ns = query.entry_fill_ts + query.window_ns;
  }

  void observe(const Tick& tick) {
    if (tick.ts < query_.entry_fill_ts || closed(tick.ts)) {
      return;
    }
    ++result_.ticks;
    if (tick.aggressor == Side::kNone) {
      ++result_.no_aggressor_ticks;
    }
    result_.volume += tick.size;

    const bool is_long = query_.position_side == Side::kBid;
    result_.worst_price = is_long ? std::min(result_.worst_price, tick.price)
                                  : std::max(result_.worst_price, tick.price);

    const bool reached =
        is_long ? tick.price <= query_.stop_price : tick.price >= query_.stop_price;
    if (reached && !result_.stop_reached) {
      result_.stop_reached = true;
      result_.first_reach_ts = tick.ts;
    }

    const std::int64_t adverse = is_long ? query_.entry_price - result_.worst_price
                                         : result_.worst_price - query_.entry_price;
    result_.mae_ticks = adverse > 0 ? scale_.to_ticks(adverse) : 0;
  }

  [[nodiscard]] bool closed(std::int64_t ts) const noexcept { return ts >= result_.window_end_ns; }

  [[nodiscard]] const NakedWindowResult& result() const noexcept { return result_; }

 private:
  TickScale scale_;
  NakedWindowQuery query_;
  NakedWindowResult result_{};
};

/// Queries must be sorted by `entry_fill_ts`; windows may overlap.
class NakedWindowScan {
 public:
  explicit NakedWindowScan(TickScale scale, std::size_t max_concurrent = 64)
      : scale_(scale), max_concurrent_(max_concurrent) {}

  [[nodiscard]] std::vector<NakedWindowResult> run(TradeSource& source,
                                                   std::span<const NakedWindowQuery> queries) {
    std::vector<NakedWindowAccumulator> accumulators;
    accumulators.reserve(queries.size());
    std::int64_t last_start = kNotStarted;
    for (const NakedWindowQuery& q : queries) {
      if (!accumulators.empty() && q.entry_fill_ts < last_start) {
        throw ReplayError("naked window queries must be sorted by entry_fill_ts");
      }
      last_start = q.entry_fill_ts;
      accumulators.emplace_back(scale_, q);
    }

    std::vector<std::size_t> active;
    std::size_t next = 0;
    bool instrument_bound = false;
    std::uint32_t instrument = 0;
    while (const Tick* tick = source.next()) {
      if (!instrument_bound) {
        instrument_bound = true;
        instrument = tick->instrument_id;
      } else if (tick->instrument_id != instrument) {
        throw ReplayError(
            "a print from a second instrument reached the scan; filter the source to one "
            "instrument_id");
      }
      active.erase(std::remove_if(active.begin(), active.end(),
                                  [&](std::size_t i) { return accumulators[i].closed(tick->ts); }),
                   active.end());
      while (next < queries.size() && queries[next].entry_fill_ts <= tick->ts) {
        const std::size_t opened = next++;
        if (accumulators[opened].closed(tick->ts)) {
          continue;
        }
        active.push_back(opened);
        if (active.size() > max_concurrent_) {
          throw ReplayError("more naked windows are open at once than max_concurrent allows");
        }
      }
      for (const std::size_t i : active) {
        accumulators[i].observe(*tick);
      }
    }

    std::vector<NakedWindowResult> out;
    out.reserve(accumulators.size());
    for (const NakedWindowAccumulator& a : accumulators) {
      out.push_back(a.result());
    }
    return out;
  }

 private:
  TickScale scale_;
  std::size_t max_concurrent_;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_NAKED_WINDOW_HPP
