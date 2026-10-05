// A second order-by-order book, built for replay speed and held to the
// reference `Book` record for record: the same queues, the same counters and
// the same modify rule, which both call rather than each restating.
//
// Two things differ where a caller can see them. It must be told each
// instrument's tick before that instrument's first order, and it refuses a
// price off the grid rather than place it. And its storage grows only in the
// counted events `Growth` lists, so once warm it replays without allocating.

#ifndef BOOKREPLAY_FAST_BOOK_HPP
#define BOOKREPLAY_FAST_BOOK_HPP

#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/depth.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bookreplay {

/// Thrown when a record asks for a price the fast book cannot place: one off
/// its instrument's tick grid, or on an instrument it was given no tick for.
/// Placing it anyway would mean guessing the grid, and a guessed grid is
/// exactly what the definition schema exists to replace.
class TickGridError : public BookreplayError {
 public:
  using BookreplayError::BookreplayError;
};

class FastBook {
 public:
  /// Every way the book's storage grows. Each is counted where it happens, and
  /// `apply` allocates nowhere else. A growth is one allocation of the book's
  /// own; a debug standard library may add bookkeeping allocations to it.
  struct Growth {
    std::uint64_t slab = 0;            ///< the order slab doubled
    std::uint64_t id_table = 0;        ///< the id table doubled and rehashed
    std::uint64_t pages = 0;           ///< a page of levels opened at prices not seen before
    std::uint64_t page_directory = 0;  ///< a list of pages outgrew its reserve
    std::uint64_t attribution = 0;     ///< an instrument's fill attribution outgrew its reserve

    [[nodiscard]] std::uint64_t total() const noexcept {
      return slab + id_table + pages + page_directory + attribution;
    }
  };

  FastBook();

  /// Moving leaves the source an empty book with no instrument registered.
  FastBook(FastBook&& other) noexcept;
  FastBook& operator=(FastBook&& other) noexcept;
  FastBook(const FastBook&) = delete;
  FastBook& operator=(const FastBook&) = delete;
  ~FastBook() = default;

  /// Throws TickGridError unless `tick_size` is positive, and if the
  /// instrument already has a different one. Registering an instrument after
  /// the replay began is allowed and allocates.
  void set_tick_size(std::uint32_t instrument_id, std::int64_t tick_size);

  /// A record refused with TickGridError leaves the book as it was, counters
  /// included. A failed allocation leaves it consistent but perhaps without
  /// the record's order.
  void apply(const MboMsg& rec);

  /// No counter is reset, and no instrument forgets its tick.
  void reset();

  /// Throws BookError unless every level's total and count are those of its
  /// queue, every queued order rests at that level and is reachable from its
  /// id, every slot is either queued exactly once or free, every page's
  /// occupancy agrees with its levels, and every side's touch is its best
  /// non-empty level.
  void verify() const;

  [[nodiscard]] std::uint64_t mutation_count() const noexcept { return mutations_; }

  [[nodiscard]] bool contains(std::uint32_t instrument_id, std::uint64_t order_id) const;

  [[nodiscard]] std::int64_t best_bid(std::uint32_t instrument_id) const;
  [[nodiscard]] std::int64_t best_ask(std::uint32_t instrument_id) const;

  [[nodiscard]] std::uint64_t unknown_modifies() const noexcept { return unknown_modifies_; }

  [[nodiscard]] std::uint64_t duplicate_adds() const noexcept { return duplicate_adds_; }

  [[nodiscard]] std::uint64_t queue_ahead(std::uint32_t instrument_id,
                                          std::uint64_t order_id) const;

  [[nodiscard]] std::size_t instrument_count() const noexcept { return holding_; }

  /// Instruments holding a book, in `Book::instruments()`'s sense.
  [[nodiscard]] std::vector<std::uint32_t> instruments() const;

  [[nodiscard]] std::size_t order_count() const noexcept;

  [[nodiscard]] const Growth& growth() const noexcept { return growth_; }

  friend Depth10 top_ten(const FastBook& book, std::uint32_t instrument_id);
  friend std::optional<RestingView> resting_view(const FastBook& book, std::uint32_t instrument_id,
                                                 std::uint64_t order_id);
  friend std::optional<LevelView> level_view(const FastBook& book, std::uint32_t instrument_id,
                                             Side side, std::int64_t price);
  friend std::vector<LevelView> ladder_view(const FastBook& book, std::uint32_t instrument_id,
                                            Side side);
  friend std::vector<std::uint64_t> queue_view(const FastBook& book, std::uint32_t instrument_id,
                                               Side side, std::int64_t price);

 private:
  /// Defined only by the tests, which use it to corrupt a link and watch
  /// `verify()` find it.
  friend struct FastBookProbe;

  static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();
  static constexpr std::int64_t kNoKey = std::numeric_limits<std::int64_t>::max();

  // Levels are addressed by tick, but a venue lets an order rest far from the
  // market, so one array over an instrument's whole price range would be
  // almost entirely empty. A page is allocated only where an order has rested:
  // the levels around the touch share one or two, and a stray far order costs
  // one more. A page is 1,024 ticks so that its bitmap is sixteen words, which
  // one summary word covers, and its levels are 24 KB.
  static constexpr int kPageBits = 10;
  static constexpr std::uint32_t kPageTicks = 1U << kPageBits;
  static constexpr std::uint32_t kPageWords = kPageTicks / 64;

  /// One resting order, or a free slot. Addressed by index, so the slab can
  /// grow without invalidating a link.
  struct Node {
    std::uint64_t order_id = 0;
    std::int64_t price = 0;
    std::uint64_t filled = 0;
    std::uint32_t size = 0;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;  ///< the free list's link while free
    std::uint32_t page = kNil;  ///< kNil exactly while free
    std::uint32_t offset = 0;
    Side side = Side::kNone;
  };

  // The reference keeps a std::deque here, which allocates when a level opens
  // (a 4 KB block on libc++) and frees when it closes. A list threaded through
  // the slab opens and closes a level without allocating and unlinks an order
  // without searching for it, and an empty level is just one whose head is
  // kNil.
  struct Level {
    std::uint64_t total = 0;
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
    std::uint32_t count = 0;
  };

  struct Page {
    std::int64_t first_key = 0;
    std::uint32_t instrument = 0;
    Side side = Side::kNone;
    /// Where this page sits in its ladder's directory.
    std::uint32_t position = 0;
    std::uint32_t occupied = 0;
    /// Bit w is set while `words[w]` is non-zero, so the next occupied level
    /// is two bit scans away however sparse the page.
    std::uint64_t summary = 0;
    std::array<std::uint64_t, kPageWords> words{};
    std::array<Level, kPageTicks> levels{};
  };

  /// One side of one instrument. Keys run best first on both sides: an ask's
  /// key is its tick index, a bid's that index complemented, so the touch is
  /// always the lowest occupied key and walking away from it is walking up.
  struct Ladder {
    struct Entry {
      std::int64_t number = 0;
      std::uint32_t page = kNil;
    };

    /// Ascending by page number.
    std::vector<Entry> directory;
    std::int64_t best = kNoKey;
    std::uint32_t best_page = kNil;
    /// The page the last placement landed on, tried before the directory is
    /// searched.
    std::int64_t recent_number = kNoKey;
    std::uint32_t recent_page = kNil;
  };

  struct Instrument {
    std::uint32_t id = 0;
    std::int64_t tick = 0;
    bool holds_book = false;
    Ladder bids;
    Ladder asks;
    /// What `forget_fills` must reset at this instrument's next boundary. An
    /// id may appear twice, and may name an order cancelled since.
    std::vector<std::uint64_t> filled_this_event;
  };

  // One table for the whole book, keyed by instrument and order id together,
  // as `contains` takes them: an id used on two instruments is two orders.
  class IdTable {
   public:
    struct Entry {
      std::uint64_t order_id = 0;  ///< 0, never a placeable id, marks an empty bucket
      std::uint32_t instrument = 0;
      std::uint32_t slot = kNil;
    };

    IdTable() = default;
    explicit IdTable(std::size_t capacity);
    IdTable(IdTable&& other) noexcept;
    IdTable& operator=(IdTable&& other) noexcept;
    IdTable(const IdTable&) = delete;
    IdTable& operator=(const IdTable&) = delete;
    ~IdTable() = default;

    [[nodiscard]] std::uint32_t find(std::uint32_t instrument, std::uint64_t order_id) const;

    /// Grows the table if one more id would crowd it, so that `place` cannot
    /// fail. Returns whether it grew.
    bool make_room();

    /// The id must be absent and `make_room` called first.
    void place(std::uint32_t instrument, std::uint64_t order_id, std::uint32_t slot);

    void erase(std::uint32_t instrument, std::uint64_t order_id);

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] const std::vector<Entry>& buckets() const noexcept { return buckets_; }

   private:
    [[nodiscard]] std::size_t home(std::uint32_t instrument, std::uint64_t order_id) const;
    void rehash(std::size_t capacity);

    std::vector<Entry> buckets_;
    std::size_t mask_ = 0;
    int shift_ = 0;
    std::size_t size_ = 0;
  };

  [[nodiscard]] std::uint32_t find_instrument(std::uint32_t instrument_id) const;
  void index_instrument(std::uint32_t instrument_id, std::uint32_t instrument);
  [[nodiscard]] std::uint32_t require_instrument(std::uint32_t instrument_id);
  [[nodiscard]] Ladder& ladder(std::uint32_t instrument, Side side);
  [[nodiscard]] const Ladder& ladder(std::uint32_t instrument, Side side) const;
  [[nodiscard]] static bool on_grid(const Instrument& ins, std::int64_t price);
  [[nodiscard]] static std::int64_t key_of(const Instrument& ins, Side side, std::int64_t price);
  [[nodiscard]] static std::int64_t placed_key(const Instrument& ins, Side side,
                                               std::int64_t price);
  [[nodiscard]] static std::size_t directory_position(const Ladder& lad, std::int64_t number);
  [[nodiscard]] static std::int64_t price_of(const Instrument& ins, Side side, std::int64_t key);
  [[nodiscard]] static std::uint32_t first_occupied(const Page& page, std::uint32_t from);
  [[nodiscard]] std::uint32_t page_for(std::uint32_t instrument, Side side, std::int64_t key);
  [[nodiscard]] const Level* level_at(std::uint32_t instrument_id, Side side,
                                      std::int64_t price) const;
  void seek_best(Ladder& lad, std::size_t position, std::uint32_t offset);

  void take(FastBook& other) noexcept;
  [[nodiscard]] std::uint32_t allocate(std::uint64_t order_id, Side side, std::int64_t price,
                                       std::uint32_t size);
  void release(std::uint32_t slot);
  void link(std::uint32_t slot, std::uint32_t page, std::uint32_t offset);
  void unlink(std::uint32_t slot);
  void opened(std::uint32_t page, std::uint32_t offset);
  void closed(std::uint32_t page, std::uint32_t offset);

  void insert(std::uint32_t instrument, std::uint64_t order_id, Side side, std::int64_t key,
              std::int64_t price, std::uint32_t size);
  void erase(std::uint32_t instrument, std::uint32_t slot);
  void hold(Instrument& ins);
  void add(const MboMsg& rec);
  void cancel(const MboMsg& rec);
  void modify(const MboMsg& rec);
  void fill(const MboMsg& rec);
  void clear(std::uint32_t instrument);
  void forget_fills(std::uint32_t instrument_id);

  template <typename Visit>
  void for_each_level(std::uint32_t instrument, Side side, Visit visit) const;

  std::vector<Instrument> instruments_;
  /// Open addressing from instrument id to index into `instruments_`, kept at
  /// most a quarter full: every record asks it, and a probe that hits first
  /// time is the only lookup that never mispredicts.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> instrument_index_;

  std::vector<Node> nodes_;
  std::uint32_t free_ = kNil;
  IdTable ids_;
  std::vector<std::unique_ptr<Page>> pages_;

  std::size_t holding_ = 0;
  std::uint64_t mutations_ = 0;
  std::uint64_t unknown_modifies_ = 0;
  std::uint64_t duplicate_adds_ = 0;
  Growth growth_{};
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_FAST_BOOK_HPP
