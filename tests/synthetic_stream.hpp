// Made-up MBO streams, for exercising a book without the licensed corpus.
//
// `synthetic_stream` is long enough to run a book at volume. One instrument at
// NQ's 0.25 tick: a snapshot, then adds clustered near a touch that wanders
// inside a fixed band, cancels and modifies of resting orders, and now and
// then a trade that takes the front of one side. Shaped like NQ only loosely,
// and nothing about a real day can be read from its numbers.
//
// `rough_stream` is shaped like no market at all. It sends what the first one
// is too well behaved to send, for two books that must still come out alike.

#ifndef BOOKREPLAY_TESTS_SYNTHETIC_STREAM_HPP
#define BOOKREPLAY_TESTS_SYNTHETIC_STREAM_HPP

#include "bookreplay/dbn.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "toy_stream.hpp"

namespace bookreplay::testing {

/// The standard library's distributions differ between implementations, and
/// a test counting allocations must see one stream on every platform.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t n) { return next() % n; }

 private:
  std::uint64_t state_;
};

/// Resting orders never exceed `kSyntheticMaxResting`, and prices stay inside a
/// few hundred ticks, so once a book has held that many orders across that
/// band the rest of the stream asks it for no more room.
inline constexpr std::size_t kSyntheticMaxResting = 2000;

[[nodiscard]] inline std::vector<MboMsg> synthetic_stream(std::size_t events,
                                                          std::uint64_t seed = 1) {
  struct Resting {
    std::uint64_t id;
    Side side;
    std::int64_t tick;
    std::uint32_t size;
  };

  constexpr std::int64_t kCentre = 29000 * 4;
  constexpr std::int64_t kBand = 64;

  SplitMix64 rng{seed};
  StreamBuilder s;
  std::vector<Resting> resting;
  std::uint64_t next_id = 1;
  std::int64_t mid = kCentre;

  const auto price = [](std::int64_t tick) { return tick * kTick; };
  const auto random_side = [&rng] { return rng.below(2) == 0 ? Side::kBid : Side::kAsk; };
  const auto add = [&](Side side) {
    const auto away = static_cast<std::int64_t>(rng.below(4) == 0 ? rng.below(200) : rng.below(8));
    const std::int64_t tick = side == Side::kBid ? mid - 1 - away : mid + 1 + away;
    const auto size = static_cast<std::uint32_t>(1 + rng.below(10));
    s.add(next_id, side, price(tick), size);
    resting.push_back(Resting{next_id++, side, tick, size});
  };
  const auto take = [&](std::size_t i) {
    const Resting o = resting[i];
    resting[i] = resting.back();
    resting.pop_back();
    return o;
  };

  s.clear();
  for (int i = 0; i < 400; ++i) {
    add(i % 2 == 0 ? Side::kBid : Side::kAsk);
    s.snapshot();
  }
  s.last();

  for (std::size_t event = 0; event < events; ++event) {
    const std::uint64_t roll = rng.below(100);
    if (resting.empty() || (roll < 45 && resting.size() < kSyntheticMaxResting)) {
      add(random_side());
    } else if (roll < 80) {
      const Resting o = take(rng.below(resting.size()));
      s.cancel(o.id, o.side, price(o.tick), o.size);
    } else if (roll < 95) {
      Resting& o = resting[rng.below(resting.size())];
      if (rng.below(4) == 0 && o.size > 1) {
        --o.size;
      } else {
        const auto step = static_cast<std::int64_t>(1 + rng.below(3));
        o.tick += rng.below(2) == 0 ? step : -step;
      }
      s.modify(o.id, o.side, price(o.tick), o.size);
    } else {
      const Side side = random_side();
      std::size_t front = resting.size();
      for (std::size_t i = 0; i < resting.size(); ++i) {
        const Resting& o = resting[i];
        if (o.side == side &&
            (front == resting.size() ||
             (side == Side::kBid ? o.tick > resting[front].tick : o.tick < resting[front].tick))) {
          front = i;
        }
      }
      if (front == resting.size()) {
        add(side);
      } else {
        const Resting o = take(front);
        const Side aggressor = side == Side::kBid ? Side::kAsk : Side::kBid;
        s.trade(aggressor, price(o.tick), o.size).fill(o.id, o.side, price(o.tick), o.size);
        s.cancel(o.id, o.side, price(o.tick), o.size);
      }
    }
    s.last();
    if (rng.below(50) == 0) {
      mid = std::clamp(mid + (rng.below(2) == 0 ? 1 : -1), kCentre - kBand, kCentre + kBand);
    }
  }
  return s.records();
}

/// The records `synthetic_stream` never sends: several instruments, prices
/// thousands of ticks apart, an add for an id still resting, a modify or a
/// cancel for one never added, fills that an M follows inside the same event
/// (sometimes twice for one order), and a clear in mid-stream. An instrument
/// holding no orders always opens with a modify. The first stream stays as it
/// is because the allocation test needs a book that stops growing.
[[nodiscard]] inline std::vector<MboMsg> rough_stream(std::size_t events,
                                                      std::span<const std::uint32_t> instruments,
                                                      std::uint64_t seed = 1) {
  struct Resting {
    std::uint64_t id;
    Side side;
    std::int64_t tick;
    std::uint32_t size;
  };

  struct Market {
    std::int64_t mid;
    std::vector<Resting> resting;
  };

  constexpr std::int64_t kCentre = 29000 * 4;
  constexpr std::int64_t kBand = 2500;
  constexpr std::size_t kMaxResting = 600;

  SplitMix64 rng{seed};
  StreamBuilder s;
  std::vector<Market> markets(instruments.size(), Market{kCentre, {}});
  std::uint64_t next_id = 1;

  const auto price = [](std::int64_t tick) { return tick * kTick; };
  const auto lots = [&rng] { return static_cast<std::uint32_t>(1 + rng.below(10)); };
  // Draws are kept out of argument lists: the order a compiler evaluates
  // arguments in is its own business, and the stream must not depend on it.
  const auto placed = [&](const Market& m, std::uint64_t id) {
    const Side side = rng.below(2) == 0 ? Side::kBid : Side::kAsk;
    const std::uint64_t reach = rng.below(16) == 0 ? 3000 : (rng.below(4) == 0 ? 200 : 8);
    const auto away = static_cast<std::int64_t>(1 + rng.below(reach));
    const std::uint32_t size = lots();
    return Resting{id, side, side == Side::kBid ? m.mid - away : m.mid + away, size};
  };

  for (std::size_t event = 0; event < events; ++event) {
    const std::size_t which = rng.below(markets.size());
    Market& m = markets[which];
    s.instrument(instruments[which]);
    const auto pick = [&]() -> Resting& { return m.resting[rng.below(m.resting.size())]; };
    const auto drop = [&m](Resting& o) {
      o = m.resting.back();
      m.resting.pop_back();
    };

    const std::uint64_t roll = rng.below(100);
    if (m.resting.empty() || roll < 3) {
      const Resting o = placed(m, next_id++);
      s.modify(o.id, o.side, price(o.tick), o.size);
      m.resting.push_back(o);
    } else if (roll < 40 && m.resting.size() < kMaxResting) {
      const Resting o = placed(m, next_id++);
      s.add(o.id, o.side, price(o.tick), o.size);
      m.resting.push_back(o);
    } else if (roll < 42) {
      Resting& o = pick();
      o = placed(m, o.id);
      s.add(o.id, o.side, price(o.tick), o.size);
    } else if (roll < 62) {
      Resting& o = pick();
      s.cancel(o.id, o.side, price(o.tick), o.size);
      drop(o);
    } else if (roll < 63) {
      s.cancel(next_id++, Side::kBid, price(m.mid), 1);
    } else if (roll < 78) {
      Resting& o = pick();
      const std::uint64_t change = rng.below(3);
      if (change == 0 && o.size > 1) {
        --o.size;
      } else if (change == 1) {
        o.size += lots();
      } else {
        const auto step = static_cast<std::int64_t>(1 + rng.below(3));
        o.tick += rng.below(2) == 0 ? step : -step;
      }
      s.modify(o.id, o.side, price(o.tick), o.size);
    } else if (roll < 99 || rng.below(40) != 0) {
      Resting& o = pick();
      const Side aggressor = o.side == Side::kBid ? Side::kAsk : Side::kBid;
      const auto fill = [&](std::uint32_t size) {
        s.trade(aggressor, price(o.tick), size).fill(o.id, o.side, price(o.tick), size);
      };
      if (rng.below(3) == 0) {
        fill(o.size);
        s.cancel(o.id, o.side, price(o.tick), o.size);
        drop(o);
      } else {
        // Each round is a partial fill and its remainder, or a displayed
        // tranche taken whole and refreshed.
        do {
          const bool partial = o.size > 1 && rng.below(2) == 0;
          const std::uint32_t taken =
              partial ? static_cast<std::uint32_t>(1 + rng.below(o.size - 1)) : o.size;
          fill(taken);
          o.size = partial ? o.size - taken : lots();
          s.modify(o.id, o.side, price(o.tick), o.size);
        } while (rng.below(3) == 0);
      }
    } else {
      s.clear();
      m.resting.clear();
    }
    s.last();

    if (rng.below(8) == 0) {
      const auto step = static_cast<std::int64_t>(rng.below(65)) - 32;
      m.mid = std::clamp(m.mid + step, kCentre - kBand, kCentre + kBand);
    }
  }
  return s.records();
}

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_SYNTHETIC_STREAM_HPP
