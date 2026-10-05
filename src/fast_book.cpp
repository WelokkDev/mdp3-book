#include "bookreplay/fast_book.hpp"

#include "bookreplay/book.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <initializer_list>
#include <string>

namespace bookreplay {
namespace {

// Starting sizes, not limits. What outgrows one during a replay grows, and
// `Growth` counts it; the instrument index grows only when an instrument is
// registered.
constexpr std::size_t kInitialSlots = 1024;
constexpr std::size_t kInitialBuckets = 2048;
constexpr std::size_t kInitialInstrumentBuckets = 16;
constexpr std::size_t kReservedPages = 64;
constexpr std::size_t kReservedDirectory = 16;
constexpr std::size_t kReservedAttribution = 64;

// 2^64 over the golden ratio. Multiplying by it spreads nearby integers across
// the upper half of the product.
constexpr std::uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;
// The first multiplier of splitmix64's finalizer: a second odd constant, so an
// order id is not mixed by the one that offsets it by its instrument.
constexpr std::uint64_t kSplitMix = 0xBF58476D1CE4E5B9ULL;

[[nodiscard]] std::uint64_t bit(std::uint32_t index) noexcept {
  return std::uint64_t{1} << (index & 63);
}

[[nodiscard]] std::size_t instrument_home(std::uint32_t instrument_id, std::size_t mask) noexcept {
  return static_cast<std::size_t>((std::uint64_t{instrument_id} * kGoldenRatio) >> 32) & mask;
}

}  // namespace

FastBook::IdTable::IdTable(std::size_t capacity) {
  rehash(capacity);
}

FastBook::IdTable::IdTable(IdTable&& other) noexcept {
  *this = std::move(other);
}

FastBook::IdTable& FastBook::IdTable::operator=(IdTable&& other) noexcept {
  buckets_ = std::exchange(other.buckets_, {});
  mask_ = std::exchange(other.mask_, 0);
  shift_ = std::exchange(other.shift_, 0);
  size_ = std::exchange(other.size_, 0);
  return *this;
}

// Ids arrive nearly in sequence. Taken straight modulo the table they would
// fill runs of adjacent buckets, which linear probing then has to walk, so
// they are mixed first.
std::size_t FastBook::IdTable::home(std::uint32_t instrument, std::uint64_t order_id) const {
  const std::uint64_t mixed = (order_id + std::uint64_t{instrument} * kGoldenRatio) * kSplitMix;
  return static_cast<std::size_t>(mixed >> shift_);
}

std::uint32_t FastBook::IdTable::find(std::uint32_t instrument, std::uint64_t order_id) const {
  if (order_id == 0 || size_ == 0) {
    return kNil;
  }
  for (std::size_t i = home(instrument, order_id);; i = (i + 1) & mask_) {
    const Entry& e = buckets_[i];
    if (e.order_id == 0) {
      return kNil;
    }
    if (e.order_id == order_id && e.instrument == instrument) {
      return e.slot;
    }
  }
}

bool FastBook::IdTable::make_room() {
  if ((size_ + 1) * 2 <= buckets_.size()) {
    return false;
  }
  rehash(buckets_.empty() ? kInitialBuckets : buckets_.size() * 2);
  return true;
}

void FastBook::IdTable::place(std::uint32_t instrument, std::uint64_t order_id,
                              std::uint32_t slot) {
  std::size_t i = home(instrument, order_id);
  while (buckets_[i].order_id != 0) {
    i = (i + 1) & mask_;
  }
  buckets_[i] = Entry{order_id, instrument, slot};
  ++size_;
}

// Backward-shift deletion: every entry after the hole that could sit in it
// moves up, so no tombstone is ever left for a later probe to step over.
void FastBook::IdTable::erase(std::uint32_t instrument, std::uint64_t order_id) {
  if (size_ == 0) {
    return;
  }
  std::size_t hole = home(instrument, order_id);
  while (buckets_[hole].order_id != order_id || buckets_[hole].instrument != instrument) {
    if (buckets_[hole].order_id == 0) {
      return;
    }
    hole = (hole + 1) & mask_;
  }
  for (std::size_t j = (hole + 1) & mask_; buckets_[j].order_id != 0; j = (j + 1) & mask_) {
    const std::size_t want = home(buckets_[j].instrument, buckets_[j].order_id);
    if (((j - want) & mask_) >= ((j - hole) & mask_)) {
      buckets_[hole] = buckets_[j];
      hole = j;
    }
  }
  buckets_[hole] = Entry{};
  --size_;
}

void FastBook::IdTable::rehash(std::size_t capacity) {
  std::vector<Entry> old = std::exchange(buckets_, std::vector<Entry>(capacity));
  mask_ = capacity - 1;
  shift_ = 64 - std::countr_zero(capacity);
  for (const Entry& e : old) {
    if (e.order_id == 0) {
      continue;
    }
    std::size_t i = home(e.instrument, e.order_id);
    while (buckets_[i].order_id != 0) {
      i = (i + 1) & mask_;
    }
    buckets_[i] = e;
  }
}

FastBook::FastBook() : ids_(kInitialBuckets) {
  nodes_.reserve(kInitialSlots);
  pages_.reserve(kReservedPages);
}

FastBook::FastBook(FastBook&& other) noexcept {
  take(other);
}

FastBook& FastBook::operator=(FastBook&& other) noexcept {
  if (this != &other) {
    take(other);
  }
  return *this;
}

void FastBook::take(FastBook& other) noexcept {
  instruments_ = std::exchange(other.instruments_, {});
  instrument_index_ = std::exchange(other.instrument_index_, {});
  nodes_ = std::exchange(other.nodes_, {});
  free_ = std::exchange(other.free_, kNil);
  ids_ = std::move(other.ids_);
  pages_ = std::exchange(other.pages_, {});
  holding_ = std::exchange(other.holding_, 0);
  mutations_ = std::exchange(other.mutations_, 0);
  unknown_modifies_ = std::exchange(other.unknown_modifies_, 0);
  duplicate_adds_ = std::exchange(other.duplicate_adds_, 0);
  growth_ = std::exchange(other.growth_, Growth{});
}

void FastBook::set_tick_size(std::uint32_t instrument_id, std::int64_t tick_size) {
  if (tick_size <= 0) {
    throw TickGridError("instrument " + std::to_string(instrument_id) +
                        " needs a positive tick size, not " + std::to_string(tick_size));
  }
  const std::uint32_t known = find_instrument(instrument_id);
  if (known != kNil) {
    if (instruments_[known].tick != tick_size) {
      throw TickGridError("instrument " + std::to_string(instrument_id) +
                          " already has tick size " + std::to_string(instruments_[known].tick));
    }
    return;
  }
  Instrument ins;
  ins.id = instrument_id;
  ins.tick = tick_size;
  ins.bids.directory.reserve(kReservedDirectory);
  ins.asks.directory.reserve(kReservedDirectory);
  ins.filled_this_event.reserve(kReservedAttribution);
  instruments_.push_back(std::move(ins));
  if (instruments_.size() * 4 > instrument_index_.size()) {
    instrument_index_.assign(std::max(kInitialInstrumentBuckets, instrument_index_.size() * 2),
                             {0, kNil});
    for (std::uint32_t i = 0; i < instruments_.size(); ++i) {
      index_instrument(instruments_[i].id, i);
    }
  } else {
    index_instrument(instrument_id, static_cast<std::uint32_t>(instruments_.size() - 1));
  }
}

void FastBook::index_instrument(std::uint32_t instrument_id, std::uint32_t instrument) {
  const std::size_t mask = instrument_index_.size() - 1;
  std::size_t i = instrument_home(instrument_id, mask);
  while (instrument_index_[i].second != kNil) {
    i = (i + 1) & mask;
  }
  instrument_index_[i] = {instrument_id, instrument};
}

std::uint32_t FastBook::find_instrument(std::uint32_t instrument_id) const {
  if (instrument_index_.empty()) {
    return kNil;
  }
  const std::size_t mask = instrument_index_.size() - 1;
  for (std::size_t i = instrument_home(instrument_id, mask);; i = (i + 1) & mask) {
    const auto& [id, instrument] = instrument_index_[i];
    if (instrument == kNil || id == instrument_id) {
      return instrument;
    }
  }
}

std::uint32_t FastBook::require_instrument(std::uint32_t instrument_id) {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil) {
    throw TickGridError("instrument " + std::to_string(instrument_id) +
                        " has no tick size, so none of its prices can be placed");
  }
  return instrument;
}

FastBook::Ladder& FastBook::ladder(std::uint32_t instrument, Side side) {
  Instrument& ins = instruments_[instrument];
  return side == Side::kBid ? ins.bids : ins.asks;
}

const FastBook::Ladder& FastBook::ladder(std::uint32_t instrument, Side side) const {
  const Instrument& ins = instruments_[instrument];
  return side == Side::kBid ? ins.bids : ins.asks;
}

// The two extremes are refused along with everything off the grid: one is the
// venue's "no price", and either would complement or index to the sentinel
// that marks an empty side.
bool FastBook::on_grid(const Instrument& ins, std::int64_t price) {
  return price != kUndefPrice && price != std::numeric_limits<std::int64_t>::min() &&
         price % ins.tick == 0;
}

std::int64_t FastBook::key_of(const Instrument& ins, Side side, std::int64_t price) {
  const std::int64_t tick = price / ins.tick;
  return side == Side::kBid ? ~tick : tick;
}

std::int64_t FastBook::placed_key(const Instrument& ins, Side side, std::int64_t price) {
  if (!on_grid(ins, price)) {
    throw TickGridError("price " + std::to_string(price) + " is not on instrument " +
                        std::to_string(ins.id) + "'s tick of " + std::to_string(ins.tick));
  }
  return key_of(ins, side, price);
}

std::int64_t FastBook::price_of(const Instrument& ins, Side side, std::int64_t key) {
  return (side == Side::kBid ? ~key : key) * ins.tick;
}

std::uint32_t FastBook::first_occupied(const Page& page, std::uint32_t from) {
  if (from >= kPageTicks) {
    return kPageTicks;
  }
  std::uint32_t word = from >> 6;
  const std::uint64_t here = page.words[word] & (~std::uint64_t{0} << (from & 63));
  if (here != 0) {
    return (word << 6) + static_cast<std::uint32_t>(std::countr_zero(here));
  }
  const std::uint64_t later = page.summary & (~std::uint64_t{0} << (word + 1));
  if (later == 0) {
    return kPageTicks;
  }
  word = static_cast<std::uint32_t>(std::countr_zero(later));
  return (word << 6) + static_cast<std::uint32_t>(std::countr_zero(page.words[word]));
}

std::size_t FastBook::directory_position(const Ladder& lad, std::int64_t number) {
  const auto it =
      std::lower_bound(lad.directory.begin(), lad.directory.end(), number,
                       [](const Ladder::Entry& e, std::int64_t n) { return e.number < n; });
  return static_cast<std::size_t>(it - lad.directory.begin());
}

std::uint32_t FastBook::page_for(std::uint32_t instrument, Side side, std::int64_t key) {
  Ladder& lad = ladder(instrument, side);
  const std::int64_t number = key >> kPageBits;
  if (number == lad.recent_number) {
    return lad.recent_page;
  }
  const std::size_t position = directory_position(lad, number);
  std::uint32_t page = kNil;
  if (position < lad.directory.size() && lad.directory[position].number == number) {
    page = lad.directory[position].page;
  } else {
    if (pages_.size() == pages_.capacity()) {
      ++growth_.page_directory;
    }
    if (lad.directory.size() == lad.directory.capacity()) {
      ++growth_.page_directory;
    }
    auto opened_page = std::make_unique<Page>();
    opened_page->first_key = number * std::int64_t{kPageTicks};
    opened_page->instrument = instrument;
    opened_page->side = side;
    page = static_cast<std::uint32_t>(pages_.size());
    pages_.push_back(std::move(opened_page));
    lad.directory.insert(lad.directory.begin() + static_cast<std::ptrdiff_t>(position),
                         Ladder::Entry{number, page});
    for (std::size_t i = position; i < lad.directory.size(); ++i) {
      pages_[lad.directory[i].page]->position = static_cast<std::uint32_t>(i);
    }
    ++growth_.pages;
  }
  lad.recent_number = number;
  lad.recent_page = page;
  return page;
}

const FastBook::Level* FastBook::level_at(std::uint32_t instrument_id, Side side,
                                          std::int64_t price) const {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil || (side != Side::kBid && side != Side::kAsk)) {
    return nullptr;
  }
  const Instrument& ins = instruments_[instrument];
  if (!on_grid(ins, price)) {
    return nullptr;
  }
  const std::int64_t key = key_of(ins, side, price);
  const Ladder& lad = ladder(instrument, side);
  const std::int64_t number = key >> kPageBits;
  const std::size_t position = directory_position(lad, number);
  if (position == lad.directory.size() || lad.directory[position].number != number) {
    return nullptr;
  }
  const Level& lvl = pages_[lad.directory[position].page]
                         ->levels[static_cast<std::uint32_t>(key & (kPageTicks - 1))];
  return lvl.count == 0 ? nullptr : &lvl;
}

// Where the touch goes when its level closes: the next occupied level on this
// page, or on the first later page that has one. An empty page costs a look at
// its summary, not a scan of its levels.
void FastBook::seek_best(Ladder& lad, std::size_t position, std::uint32_t offset) {
  for (; position < lad.directory.size(); ++position, offset = 0) {
    const std::uint32_t page = lad.directory[position].page;
    const std::uint32_t found = first_occupied(*pages_[page], offset);
    if (found < kPageTicks) {
      lad.best = pages_[page]->first_key + found;
      lad.best_page = page;
      return;
    }
  }
  lad.best = kNoKey;
  lad.best_page = kNil;
}

std::uint32_t FastBook::allocate(std::uint64_t order_id, Side side, std::int64_t price,
                                 std::uint32_t size) {
  std::uint32_t slot = free_;
  if (slot != kNil) {
    free_ = nodes_[slot].next;
  } else {
    if (nodes_.size() == nodes_.capacity()) {
      if (nodes_.size() >= kNil / 2) {
        throw BookError("the order slab cannot grow further");
      }
      nodes_.reserve(std::max(nodes_.capacity() * 2, kInitialSlots));
      ++growth_.slab;
    }
    slot = static_cast<std::uint32_t>(nodes_.size());
    nodes_.emplace_back();
  }
  Node& n = nodes_[slot];
  n.order_id = order_id;
  n.price = price;
  n.filled = 0;
  n.size = size;
  n.side = side;
  return slot;
}

void FastBook::release(std::uint32_t slot) {
  Node& n = nodes_[slot];
  n.page = kNil;
  n.prev = kNil;
  n.next = free_;
  free_ = slot;
}

void FastBook::link(std::uint32_t slot, std::uint32_t page, std::uint32_t offset) {
  Level& lvl = pages_[page]->levels[offset];
  Node& n = nodes_[slot];
  n.page = page;
  n.offset = offset;
  n.prev = lvl.tail;
  n.next = kNil;
  if (lvl.tail == kNil) {
    lvl.head = slot;
  } else {
    nodes_[lvl.tail].next = slot;
  }
  lvl.tail = slot;
  lvl.total += n.size;
  if (lvl.count++ == 0) {
    opened(page, offset);
  }
}

void FastBook::unlink(std::uint32_t slot) {
  const Node& n = nodes_[slot];
  Level& lvl = pages_[n.page]->levels[n.offset];
  if (n.prev == kNil) {
    lvl.head = n.next;
  } else {
    nodes_[n.prev].next = n.next;
  }
  if (n.next == kNil) {
    lvl.tail = n.prev;
  } else {
    nodes_[n.next].prev = n.prev;
  }
  lvl.total -= n.size;
  if (--lvl.count == 0) {
    closed(n.page, n.offset);
  }
}

void FastBook::opened(std::uint32_t page, std::uint32_t offset) {
  Page& p = *pages_[page];
  const std::uint32_t word = offset >> 6;
  p.words[word] |= bit(offset);
  p.summary |= bit(word);
  ++p.occupied;
  Ladder& lad = ladder(p.instrument, p.side);
  const std::int64_t key = p.first_key + offset;
  if (key < lad.best) {
    lad.best = key;
    lad.best_page = page;
  }
}

void FastBook::closed(std::uint32_t page, std::uint32_t offset) {
  Page& p = *pages_[page];
  const std::uint32_t word = offset >> 6;
  p.words[word] &= ~bit(offset);
  if (p.words[word] == 0) {
    p.summary &= ~bit(word);
  }
  --p.occupied;
  Ladder& lad = ladder(p.instrument, p.side);
  if (p.first_key + offset == lad.best) {
    seek_best(lad, p.position, offset + 1);
  }
}

void FastBook::insert(std::uint32_t instrument, std::uint64_t order_id, Side side, std::int64_t key,
                      std::int64_t price, std::uint32_t size) {
  const std::uint32_t page = page_for(instrument, side, key);
  if (ids_.make_room()) {
    ++growth_.id_table;
  }
  const std::uint32_t slot = allocate(order_id, side, price, size);
  link(slot, page, static_cast<std::uint32_t>(key - pages_[page]->first_key));
  ids_.place(instrument, order_id, slot);
}

void FastBook::erase(std::uint32_t instrument, std::uint32_t slot) {
  unlink(slot);
  ids_.erase(instrument, nodes_[slot].order_id);
  release(slot);
}

void FastBook::hold(Instrument& ins) {
  if (!ins.holds_book) {
    ins.holds_book = true;
    ++holding_;
  }
}

void FastBook::add(const MboMsg& rec) {
  const Side side = side_of(rec);
  if (!placeable(side, rec.order_id)) {
    return;
  }
  const std::uint32_t instrument = require_instrument(rec.hd.instrument_id);
  Instrument& ins = instruments_[instrument];
  const std::int64_t key = placed_key(ins, side, rec.price);
  hold(ins);
  const std::uint32_t stale = ids_.find(instrument, rec.order_id);
  if (stale != kNil) {
    ++duplicate_adds_;
    erase(instrument, stale);
  }
  insert(instrument, rec.order_id, side, key, rec.price, rec.size);
}

void FastBook::cancel(const MboMsg& rec) {
  const std::uint32_t instrument = find_instrument(rec.hd.instrument_id);
  if (instrument == kNil) {
    return;
  }
  const std::uint32_t slot = ids_.find(instrument, rec.order_id);
  if (slot != kNil) {
    erase(instrument, slot);
  }
}

// A re-queue keeps the order's slot and its id's bucket; only the links that
// tie it to a level move.
void FastBook::modify(const MboMsg& rec) {
  const Side side = side_of(rec);
  if (!placeable(side, rec.order_id)) {
    return;
  }
  const std::uint32_t instrument = require_instrument(rec.hd.instrument_id);
  Instrument& ins = instruments_[instrument];
  const std::int64_t key = placed_key(ins, side, rec.price);
  hold(ins);
  const std::uint32_t slot = ids_.find(instrument, rec.order_id);
  if (slot == kNil) {
    ++unknown_modifies_;
    insert(instrument, rec.order_id, side, key, rec.price, rec.size);
    return;
  }

  Node& n = nodes_[slot];
  if (!keeps_priority(Book::Resting{n.side, n.price, n.size, n.filled}, rec)) {
    const std::uint32_t page = page_for(instrument, side, key);
    unlink(slot);
    n.side = side;
    n.price = rec.price;
    n.size = rec.size;
    n.filled = 0;
    link(slot, page, static_cast<std::uint32_t>(key - pages_[page]->first_key));
    return;
  }

  pages_[n.page]->levels[n.offset].total -= n.size - rec.size;
  n.size = rec.size;
  n.filled = 0;
}

void FastBook::fill(const MboMsg& rec) {
  const std::uint32_t instrument = find_instrument(rec.hd.instrument_id);
  if (instrument == kNil) {
    return;
  }
  const std::uint32_t slot = ids_.find(instrument, rec.order_id);
  if (slot == kNil) {
    return;
  }
  Node& n = nodes_[slot];
  if (n.filled == 0) {
    std::vector<std::uint64_t>& filled = instruments_[instrument].filled_this_event;
    if (filled.size() == filled.capacity()) {
      ++growth_.attribution;
    }
    filled.push_back(rec.order_id);
  }
  n.filled += rec.size;
}

void FastBook::forget_fills(std::uint32_t instrument_id) {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil) {
    return;
  }
  std::vector<std::uint64_t>& filled = instruments_[instrument].filled_this_event;
  for (const std::uint64_t order_id : filled) {
    const std::uint32_t slot = ids_.find(instrument, order_id);
    if (slot != kNil) {
      nodes_[slot].filled = 0;
    }
  }
  filled.clear();
}

void FastBook::clear(std::uint32_t instrument) {
  Instrument& ins = instruments_[instrument];
  for (Ladder* lad : {&ins.bids, &ins.asks}) {
    for (const Ladder::Entry& entry : lad->directory) {
      Page& page = *pages_[entry.page];
      for (std::uint32_t at = first_occupied(page, 0); at < kPageTicks;
           at = first_occupied(page, at + 1)) {
        Level& lvl = page.levels[at];
        for (std::uint32_t slot = lvl.head; slot != kNil;) {
          const std::uint32_t next = nodes_[slot].next;
          ids_.erase(instrument, nodes_[slot].order_id);
          release(slot);
          slot = next;
        }
        lvl = Level{};
      }
      page.words.fill(0);
      page.summary = 0;
      page.occupied = 0;
    }
    lad->best = kNoKey;
    lad->best_page = kNil;
  }
  ins.filled_this_event.clear();
  if (ins.holds_book) {
    ins.holds_book = false;
    --holding_;
  }
}

void FastBook::apply(const MboMsg& rec) {
  switch (action_of(rec)) {
    case Action::kAdd:
      add(rec);
      ++mutations_;
      break;
    case Action::kCancel:
      cancel(rec);
      ++mutations_;
      break;
    case Action::kModify:
      modify(rec);
      ++mutations_;
      break;
    case Action::kClear: {
      const std::uint32_t instrument = find_instrument(rec.hd.instrument_id);
      if (instrument != kNil) {
        clear(instrument);
      }
      ++mutations_;
      break;
    }
    case Action::kFill:
      fill(rec);
      break;
    case Action::kTrade:
    case Action::kNone:
      break;
  }
  if (is_event_boundary(rec)) {
    forget_fills(rec.hd.instrument_id);
  }
}

void FastBook::reset() {
  for (std::uint32_t instrument = 0; instrument < instruments_.size(); ++instrument) {
    clear(instrument);
  }
}

// Each word's set bits are peeled lowest first, so the next level is one
// clear-lowest-bit away rather than a fresh masked scan from the last one:
// a top ten at every event boundary walks twenty levels, and a scan per level
// made that walk a chain of dependent loads.
template <typename Visit>
void FastBook::for_each_level(std::uint32_t instrument, Side side, Visit visit) const {
  const Ladder& lad = ladder(instrument, side);
  if (lad.best == kNoKey) {
    return;
  }
  const Page& first = *pages_[lad.best_page];
  auto from = static_cast<std::uint32_t>(lad.best - first.first_key);
  for (std::size_t position = first.position; position < lad.directory.size();
       ++position, from = 0) {
    const Page& page = *pages_[lad.directory[position].page];
    const std::uint32_t first_word = from >> 6;
    for (std::uint64_t words = page.summary & (~std::uint64_t{0} << first_word); words != 0;
         words &= words - 1) {
      const auto word = static_cast<std::uint32_t>(std::countr_zero(words));
      std::uint64_t bits = page.words[word];
      if (word == first_word) {
        bits &= ~std::uint64_t{0} << (from & 63);
      }
      for (; bits != 0; bits &= bits - 1) {
        const std::uint32_t at = (word << 6) + static_cast<std::uint32_t>(std::countr_zero(bits));
        if (!visit(page.first_key + at, page.levels[at])) {
          return;
        }
      }
    }
  }
}

void FastBook::verify() const {
  std::vector<std::uint8_t> seen(nodes_.size(), 0);
  std::size_t queued = 0;
  std::size_t holding = 0;
  for (std::uint32_t instrument = 0; instrument < instruments_.size(); ++instrument) {
    const Instrument& ins = instruments_[instrument];
    if (ins.holds_book) {
      ++holding;
    } else if (!ins.filled_this_event.empty()) {
      throw BookError("an instrument holding no book still carries fill attribution");
    }
    for (const Side side : {Side::kBid, Side::kAsk}) {
      const Ladder& lad = ladder(instrument, side);
      std::int64_t best = kNoKey;
      std::uint32_t best_page = kNil;
      for (std::size_t position = 0; position < lad.directory.size(); ++position) {
        const Ladder::Entry& entry = lad.directory[position];
        if (position > 0 && lad.directory[position - 1].number >= entry.number) {
          throw BookError("a side's pages are out of order");
        }
        const Page& page = *pages_[entry.page];
        if (page.instrument != instrument || page.side != side || page.position != position ||
            page.first_key != entry.number * std::int64_t{kPageTicks}) {
          throw BookError("a page disagrees with its directory entry");
        }
        std::uint32_t occupied = 0;
        for (std::uint32_t word = 0; word < kPageWords; ++word) {
          if (((page.summary & bit(word)) != 0) != (page.words[word] != 0)) {
            throw BookError("a page's summary disagrees with its bitmap");
          }
        }
        for (std::uint32_t at = 0; at < kPageTicks; ++at) {
          const Level& lvl = page.levels[at];
          if (((page.words[at >> 6] & bit(at)) != 0) != (lvl.count != 0)) {
            throw BookError("a page's bitmap disagrees with its levels");
          }
          if (lvl.count == 0) {
            if (lvl.head != kNil || lvl.tail != kNil || lvl.total != 0) {
              throw BookError("an empty level still holds a queue or a total");
            }
            continue;
          }
          ++occupied;
          const std::int64_t key = page.first_key + at;
          if (best == kNoKey) {
            best = key;
            best_page = entry.page;
          }
          std::uint64_t total = 0;
          std::uint32_t count = 0;
          std::uint32_t prev = kNil;
          for (std::uint32_t slot = lvl.head; slot != kNil; slot = nodes_[slot].next) {
            if (slot >= nodes_.size()) {
              throw BookError("a queue links past the end of the slab");
            }
            if (seen[slot] != 0) {
              throw BookError("a slot is queued twice, or a queue loops");
            }
            seen[slot] = 1;
            const Node& n = nodes_[slot];
            if (n.prev != prev) {
              throw BookError("a queue's back link does not name the order in front");
            }
            if (n.page != entry.page || n.offset != at || n.side != side ||
                n.price != price_of(ins, side, key)) {
              throw BookError("a queued order does not rest at that level");
            }
            if (ids_.find(instrument, n.order_id) != slot) {
              throw BookError("a queued order is not the one its id finds");
            }
            total += n.size;
            ++count;
            prev = slot;
          }
          if (lvl.tail != prev) {
            throw BookError("a level's tail is not its last order");
          }
          if (total != lvl.total || count != lvl.count) {
            throw BookError("a level's total or count disagrees with its queue");
          }
          queued += count;
        }
        if (occupied != page.occupied) {
          throw BookError("a page's occupied count disagrees with its levels");
        }
        if (occupied != 0 && !ins.holds_book) {
          throw BookError("an instrument holding no book has resting orders");
        }
      }
      if (best != lad.best || best_page != lad.best_page) {
        throw BookError("a side's touch is not its best occupied level");
      }
    }
  }
  if (holding != holding_) {
    throw BookError("the count of instruments holding a book is wrong");
  }

  std::size_t free_slots = 0;
  for (std::uint32_t slot = free_; slot != kNil; slot = nodes_[slot].next) {
    if (slot >= nodes_.size()) {
      throw BookError("the free list links past the end of the slab");
    }
    if (seen[slot] != 0) {
      throw BookError("a free slot is also queued, or the free list loops");
    }
    seen[slot] = 1;
    if (nodes_[slot].page != kNil) {
      throw BookError("a free slot still names a page");
    }
    ++free_slots;
  }
  if (queued + free_slots != nodes_.size()) {
    throw BookError("a slot is neither queued nor free");
  }

  const std::size_t filled_buckets = static_cast<std::size_t>(
      std::count_if(ids_.buckets().begin(), ids_.buckets().end(),
                    [](const IdTable::Entry& e) { return e.order_id != 0; }));
  if (filled_buckets != ids_.size() || ids_.size() != queued) {
    throw BookError("the id table holds an order no level queues");
  }
}

bool FastBook::contains(std::uint32_t instrument_id, std::uint64_t order_id) const {
  const std::uint32_t instrument = find_instrument(instrument_id);
  return instrument != kNil && ids_.find(instrument, order_id) != kNil;
}

std::int64_t FastBook::best_bid(std::uint32_t instrument_id) const {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil || instruments_[instrument].bids.best == kNoKey) {
    return kUndefPrice;
  }
  const Instrument& ins = instruments_[instrument];
  return price_of(ins, Side::kBid, ins.bids.best);
}

std::int64_t FastBook::best_ask(std::uint32_t instrument_id) const {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil || instruments_[instrument].asks.best == kNoKey) {
    return kUndefPrice;
  }
  const Instrument& ins = instruments_[instrument];
  return price_of(ins, Side::kAsk, ins.asks.best);
}

std::uint64_t FastBook::queue_ahead(std::uint32_t instrument_id, std::uint64_t order_id) const {
  const std::uint32_t instrument = find_instrument(instrument_id);
  if (instrument == kNil) {
    return kNoQueuePosition;
  }
  const std::uint32_t slot = ids_.find(instrument, order_id);
  if (slot == kNil) {
    return kNoQueuePosition;
  }
  const Node& n = nodes_[slot];
  std::uint64_t ahead = 0;
  for (std::uint32_t queued = pages_[n.page]->levels[n.offset].head; queued != slot;
       queued = nodes_[queued].next) {
    if (queued == kNil) {
      throw BookError("resting order is missing from its level's queue");
    }
    ahead += nodes_[queued].size;
  }
  return ahead;
}

std::vector<std::uint32_t> FastBook::instruments() const {
  std::vector<std::uint32_t> out;
  out.reserve(holding_);
  for (const Instrument& ins : instruments_) {
    if (ins.holds_book) {
      out.push_back(ins.id);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::size_t FastBook::order_count() const noexcept {
  return ids_.size();
}

Depth10 top_ten(const FastBook& book, std::uint32_t instrument_id) {
  Depth10 depth = kPaddedDepth;
  const std::uint32_t instrument = book.find_instrument(instrument_id);
  if (instrument == FastBook::kNil) {
    return depth;
  }
  const FastBook::Instrument& ins = book.instruments_[instrument];
  for (const Side side : {Side::kBid, Side::kAsk}) {
    std::size_t level = 0;
    book.for_each_level(instrument, side, [&](std::int64_t key, const auto& lvl) {
      set_level(depth, side, level, FastBook::price_of(ins, side, key), lvl.total, lvl.count);
      return ++level < kDepthLevels;
    });
  }
  return depth;
}

std::optional<RestingView> resting_view(const FastBook& book, std::uint32_t instrument_id,
                                        std::uint64_t order_id) {
  const std::uint32_t instrument = book.find_instrument(instrument_id);
  if (instrument == FastBook::kNil) {
    return std::nullopt;
  }
  const std::uint32_t slot = book.ids_.find(instrument, order_id);
  if (slot == FastBook::kNil) {
    return std::nullopt;
  }
  const FastBook::Node& n = book.nodes_[slot];
  return RestingView{n.side, n.price, n.size, n.filled};
}

std::optional<LevelView> level_view(const FastBook& book, std::uint32_t instrument_id, Side side,
                                    std::int64_t price) {
  const FastBook::Level* lvl = book.level_at(instrument_id, side, price);
  if (lvl == nullptr) {
    return std::nullopt;
  }
  return LevelView{price, lvl->total, lvl->count};
}

std::vector<LevelView> ladder_view(const FastBook& book, std::uint32_t instrument_id, Side side) {
  std::vector<LevelView> out;
  const std::uint32_t instrument = book.find_instrument(instrument_id);
  if (instrument == FastBook::kNil || (side != Side::kBid && side != Side::kAsk)) {
    return out;
  }
  const FastBook::Instrument& ins = book.instruments_[instrument];
  book.for_each_level(instrument, side, [&](std::int64_t key, const auto& lvl) {
    out.push_back(LevelView{FastBook::price_of(ins, side, key), lvl.total, lvl.count});
    return true;
  });
  return out;
}

std::vector<std::uint64_t> queue_view(const FastBook& book, std::uint32_t instrument_id, Side side,
                                      std::int64_t price) {
  std::vector<std::uint64_t> out;
  const FastBook::Level* lvl = book.level_at(instrument_id, side, price);
  if (lvl == nullptr) {
    return out;
  }
  for (std::uint32_t slot = lvl->head; slot != FastBook::kNil; slot = book.nodes_[slot].next) {
    out.push_back(book.nodes_[slot].order_id);
  }
  return out;
}

}  // namespace bookreplay
