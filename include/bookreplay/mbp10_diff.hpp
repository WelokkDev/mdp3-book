// Diffs a book rebuilt from market-by-order against the venue's own mbp-10.
//
// Boundaries the venue said nothing about are checked, not skipped: the venue
// publishes whenever the top ten moves, so a book that moves while the venue
// is quiet diverges.

#ifndef BOOKREPLAY_MBP10_DIFF_HPP
#define BOOKREPLAY_MBP10_DIFF_HPP

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace bookreplay {

/// Thrown when the book holds a value mbp-10 has no room for.
class Mbp10DiffError : public BookreplayError {
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

/// The book's top ten levels per side in mbp-10's own layout: bids
/// descending, asks ascending, the rest padded. Throws Mbp10DiffError if a
/// level total or order count is too wide for the wire's 32 bits.
[[nodiscard]] Depth10 top_ten(const Book& book, std::uint32_t instrument_id);

/// All the differ asks of a book. `Book` satisfies it through the overload
/// above; the fast book and the tests' deliberately wrong books bring their
/// own.
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

/// A book update is stamped with the closing MBO record's key. The key is not
/// unique: one packet can carry two events for one instrument with identical
/// timestamps, and only one of them published.
[[nodiscard]] constexpr bool same_event(const Mbp10Msg& venue, const MboMsg& rec) noexcept {
  return venue.hd.instrument_id == rec.hd.instrument_id && venue.sequence == rec.sequence &&
         venue.hd.ts_event == rec.hd.ts_event && venue.ts_recv == rec.ts_recv;
}

/// What settles whose record it is when the key cannot: a book update repeats
/// the action, price and size of the last A, C or M in the event it closes,
/// on every one of 101,474,627 records across five days of NQ. Its side is no
/// part of that: the venue writes 'N' for an event that touched both sides,
/// and thousands of times a day writes a side that is not the mutation's.
[[nodiscard]] constexpr bool names_mutation(const Mbp10Msg& venue,
                                            const MboMsg& mutation) noexcept {
  return venue.action == mutation.action && venue.price == mutation.price &&
         venue.size == mutation.size;
}

/// A trade record repeats its MBO print field for field, so it joins on more
/// than the key.
[[nodiscard]] constexpr bool same_print(const Mbp10Msg& venue, const MboMsg& rec) noexcept {
  return same_event(venue, rec) && venue.price == rec.price && venue.size == rec.size &&
         venue.side == rec.side;
}

enum class DivergenceKind : std::uint8_t {
  kLevels,            ///< the venue published a top ten and it is not ours
  kMissingRecord,     ///< our top ten moved and the venue published nothing
  kUnclaimedRecord,   ///< a published record no event boundary claimed
  kTradeUnmatched,    ///< an MBO print with no mbp-10 trade record
  kTradeLevels,       ///< a trade record disagreeing with the previous boundary
  kSnapshotMissing,   ///< a snapshot boundary the mbp-10 file has no snapshot for
  kSnapshotLevels,    ///< an opening snapshot disagreeing with the warmed book
  kSnapshotUnclaimed  ///< a stashed snapshot no snapshot boundary claimed
};

inline constexpr std::size_t kDivergenceKinds =
    static_cast<std::size_t>(DivergenceKind::kSnapshotUnclaimed) + 1;

[[nodiscard]] const char* divergence_name(DivergenceKind kind) noexcept;

struct Divergence {
  DivergenceKind kind{};
  const char* what = "";
  /// The MBO record it was found on, or one past the last when finish()
  /// raised it. For a record the stream left behind that is where it was
  /// detected, which is not where it was caused.
  std::uint64_t record_index = 0;
  /// The event key: the MBO record's, or the mbp-10 record's own when no MBO
  /// record is implicated.
  std::uint32_t instrument_id = 0;
  std::uint32_t sequence = 0;
  std::uint64_t ts_event = 0;
  std::uint64_t ts_recv = 0;
  char action = '\0';
};

struct DivergenceContext {
  /// The MBO record it was found on and those before it, in arrival order.
  std::vector<MboMsg> records;
  /// Index of the last of `records`; the rest run back from it.
  std::uint64_t last_record_index = 0;
  std::optional<Mbp10Msg> venue_record;
  std::uint64_t venue_record_index = 0;  ///< its position in the mbp-10 stream
  /// Next to `venue_record`, what `names_mutation` was asked about.
  std::optional<MboMsg> event_mutation;
  std::optional<Depth10> ours;
  std::optional<Depth10> theirs;
  std::optional<DepthMismatch> mismatch;
};

struct InstrumentDiff {
  std::uint64_t compared = 0;
  std::uint64_t silent = 0;
  std::uint64_t trades = 0;
  std::uint64_t divergences = 0;
};

struct Mbp10DiffReport {
  std::uint64_t records = 0;     ///< MBO records seen
  std::uint64_t boundaries = 0;  ///< live ones; a snapshot's own boundary is not one
  std::uint64_t compared = 0;    ///< boundaries where the venue published and agreed
  std::uint64_t silent = 0;      ///< boundaries that claimed no record and had not moved
  std::uint64_t trades = 0;
  std::uint64_t snapshots = 0;
  /// A diagnostic, in neither identity: comparisons that agreed although our
  /// top ten had not moved.
  std::uint64_t compared_unchanged = 0;
  /// Counted in `silent`: a record shared this key and was not this event's,
  /// because it named another event's mutation or this event had none to name.
  std::uint64_t deferred = 0;
  std::uint64_t records_read = 0;  ///< mbp-10 records pulled
  std::uint64_t records_held = 0;  ///< pulled and unresolved: the head, and stashed snapshots
  std::array<std::uint64_t, kDivergenceKinds> divergences_by_kind{};
  std::uint64_t divergence_count = 0;  ///< uncapped, unlike the list
  std::vector<Divergence> divergences;
  DivergenceContext first_divergence;
  std::map<std::uint32_t, InstrumentDiff> instruments;
  /// Until this, records the MBO stream never reached may still be held, so
  /// only the divergences already raised mean anything.
  bool finished = false;

  [[nodiscard]] std::uint64_t count(DivergenceKind kind) const noexcept {
    return divergences_by_kind[static_cast<std::size_t>(kind)];
  }

  /// Every live boundary ended in exactly one of these.
  [[nodiscard]] bool boundaries_reconcile() const noexcept {
    return compared + silent + count(DivergenceKind::kLevels) +
               count(DivergenceKind::kMissingRecord) ==
           boundaries;
  }

  /// So did every mbp-10 record read.
  [[nodiscard]] bool records_reconcile() const noexcept {
    return snapshots + compared + trades + count(DivergenceKind::kLevels) +
               count(DivergenceKind::kTradeLevels) + count(DivergenceKind::kSnapshotLevels) +
               count(DivergenceKind::kUnclaimedRecord) + count(DivergenceKind::kSnapshotUnclaimed) +
               records_held ==
           records_read;
  }

  [[nodiscard]] bool reconciles() const noexcept {
    return boundaries_reconcile() && records_reconcile();
  }

  [[nodiscard]] bool ok() const noexcept { return reconciles() && divergence_count == 0; }
};

/// The mbp-10 stream, one record per call and nullptr at its end. The differ
/// copies what it keeps, so a source handing out a pointer into a buffer it
/// reuses is fine as long as nothing else advances it.
template <typename S>
concept Mbp10Source = requires(S& s) {
  { s() } -> std::convertible_to<const Mbp10Msg*>;
};

/// Apply each MBO record to the book, then call after(). Call finish() only
/// when the MBO stream was read to its end. Non-owning.
template <DepthBook Book, Mbp10Source Source>
class Mbp10Diff {
 public:
  static constexpr std::size_t kContextDepth = 8;

  struct Options {
    std::size_t max_divergences = 16;
  };

  Mbp10Diff(const Book& book, Source source, Options opts = {})
      : book_(&book), source_(std::move(source)), opts_(opts) {
    pull();
  }

  void after(const MboMsg& rec) {
    ++report_.records;
    record_index_ = report_.records - 1;
    push_context(rec);

    const bool snapshot = has_flag(rec, kFlagSnapshot);
    if (!snapshot) {
      // ts_recv and never sequence: sequence is not monotone in a real day.
      // Once the MBO stream is past a record's arrival time, no event still to
      // come can claim it. Strictly less, so records sharing an arrival time
      // are never retired, and the two files are relied on to order one
      // packet's instruments alike, as five days of NQ do.
      while (head_ && head_->rec.ts_recv < rec.ts_recv) {
        report_unclaimed();
      }
      if (action_of(rec) == Action::kTrade) {
        check_trade(rec);
      }
    }
    note_mutation(rec);

    if (!is_event_boundary(rec)) {
      return;
    }

    const Depth10 top = top_ten(*book_, rec.hd.instrument_id);
    if (snapshot) {
      check_snapshot(rec, top);
    } else {
      check_boundary(rec, top);
    }
    last_[rec.hd.instrument_id] = top;
    event_mutation_.erase(rec.hd.instrument_id);
  }

  void finish() {
    record_index_ = report_.records;
    while (head_) {
      report_unclaimed();
    }
    while (!stash_.empty()) {
      const auto it = stash_.begin();
      add(DivergenceKind::kSnapshotUnclaimed, "no snapshot boundary claimed this mbp-10 snapshot",
          it->second.rec, context_of(it->second));
      stash_.erase(it);
    }
    note_held();
    report_.finished = true;
  }

  [[nodiscard]] const Mbp10DiffReport& report() const noexcept { return report_; }

  [[nodiscard]] bool ok() const noexcept { return report_.ok(); }

 private:
  struct Pulled {
    Mbp10Msg rec{};
    std::uint64_t index = 0;
  };

  void check_trade(const MboMsg& rec) {
    if (!head_ || action_of(head_->rec) != Action::kTrade || !same_print(head_->rec, rec)) {
      add(DivergenceKind::kTradeUnmatched, "no mbp-10 trade record matches this print", rec,
          head_ ? context_of(*head_) : DivergenceContext{});
      return;
    }
    // A trade record carries the book as of that instrument's previous event
    // boundary. The reductions the print causes arrive after it in records of
    // their own, and until they close the event the book means nothing.
    const Depth10& before = previous(rec.hd.instrument_id);
    if (std::optional<DepthMismatch> mismatch = first_mismatch(before, head_->rec.levels)) {
      DivergenceContext ctx = context_of(*head_);
      ctx.ours = before;
      ctx.theirs = head_->rec.levels;
      ctx.mismatch = mismatch;
      add(DivergenceKind::kTradeLevels,
          "the mbp-10 trade record disagrees with the book at the previous boundary", rec,
          std::move(ctx));
    } else {
      ++report_.trades;
      ++report_.instruments[rec.hd.instrument_id].trades;
    }
    pull();
  }

  void check_snapshot(const MboMsg& rec, const Depth10& top) {
    const auto it = stash_.find(rec.hd.instrument_id);
    if (it == stash_.end()) {
      DivergenceContext ctx;
      ctx.ours = top;
      add(DivergenceKind::kSnapshotMissing,
          "the mbp-10 file carries no snapshot for this instrument", rec, std::move(ctx));
      return;
    }
    if (std::optional<DepthMismatch> mismatch = first_mismatch(top, it->second.rec.levels)) {
      DivergenceContext ctx = context_of(it->second);
      ctx.ours = top;
      ctx.theirs = it->second.rec.levels;
      ctx.mismatch = mismatch;
      add(DivergenceKind::kSnapshotLevels, "the mbp-10 snapshot disagrees with the warmed book",
          rec, std::move(ctx));
    } else {
      ++report_.snapshots;
    }
    stash_.erase(it);
    note_held();
  }

  // Ownership is decided by the record, never by whether our own top ten
  // moved. Deciding it by the book, by leaving an unequal record to a later
  // event whenever our top ten had not moved, lets a book that shows a move
  // one event late defer and then claim inside a shared-key packet, and pass.
  //
  // The price is two events sharing a key and ending in the same action,
  // price and size, of which only the later published: the record names both,
  // the first takes it, and a correct book fails. Telling that from a late
  // move needs the MBO stream read ahead. Five days of NQ never produced one.
  void check_boundary(const MboMsg& rec, const Depth10& top) {
    const std::uint32_t instrument_id = rec.hd.instrument_id;
    ++report_.boundaries;
    const bool changed = !same_depth(top, previous(instrument_id));
    const bool keyed = head_ && has_flag(head_->rec, kFlagLast) &&
                       action_of(head_->rec) != Action::kTrade && same_event(head_->rec, rec);
    const auto mutation = event_mutation_.find(instrument_id);
    const bool named =
        keyed && mutation != event_mutation_.end() && names_mutation(head_->rec, mutation->second);
    if (!named) {
      if (changed) {
        // A keyed record stays at the head: it is a later event's to claim.
        DivergenceContext ctx = keyed ? context_of(*head_) : DivergenceContext{};
        ctx.ours = top;
        if (mutation != event_mutation_.end()) {
          ctx.event_mutation = mutation->second;
        }
        add(DivergenceKind::kMissingRecord,
            keyed ? "the book's top ten moved and the record sharing this key names another event"
                  : "the book's top ten moved and the venue published nothing",
            rec, std::move(ctx));
        return;
      }
      if (keyed) {
        ++report_.deferred;
      }
      ++report_.silent;
      ++report_.instruments[instrument_id].silent;
      return;
    }

    if (std::optional<DepthMismatch> mismatch = first_mismatch(top, head_->rec.levels)) {
      DivergenceContext ctx = context_of(*head_);
      ctx.ours = top;
      ctx.theirs = head_->rec.levels;
      ctx.mismatch = mismatch;
      ctx.event_mutation = mutation->second;
      add(DivergenceKind::kLevels, "the venue's top ten disagrees with the book's", rec,
          std::move(ctx));
    } else {
      ++report_.compared;
      ++report_.instruments[instrument_id].compared;
      // The venue does publish an unchanged top ten: an event cancelling only
      // below the tenth level and closed by a bare N still gets a record.
      if (!changed) {
        ++report_.compared_unchanged;
      }
    }
    // The record was this event's whether or not the levels agreed, so it is
    // consumed either way and one disagreement does not put the two streams
    // out of step for the rest of the day.
    pull();
  }

  void note_mutation(const MboMsg& rec) {
    switch (action_of(rec)) {
      case Action::kAdd:
      case Action::kCancel:
      case Action::kModify:
        event_mutation_[rec.hd.instrument_id] = rec;
        break;
      // No day has shown what the venue names for an event a Clear ends. One
      // that outlived it here could only name that event by accident.
      case Action::kClear:
        event_mutation_.erase(rec.hd.instrument_id);
        break;
      case Action::kTrade:
      case Action::kFill:
      case Action::kNone:
        break;
    }
  }

  void report_unclaimed() {
    add(DivergenceKind::kUnclaimedRecord, "the MBO stream passed this mbp-10 record unclaimed",
        head_->rec, context_of(*head_));
    pull();
  }

  void pull() {
    for (;;) {
      const Mbp10Msg* rec = source_();
      if (rec == nullptr) {
        head_.reset();
        break;
      }
      const Pulled pulled{*rec, report_.records_read};
      ++report_.records_read;
      if (!has_flag(*rec, kFlagSnapshot)) {
        head_ = pulled;
        break;
      }
      // A snapshot's sequence is the feed's, unrelated to the MBO snapshot's,
      // so instrument is the only join it has.
      const auto [it, inserted] = stash_.try_emplace(rec->hd.instrument_id, pulled);
      if (!inserted) {
        add(DivergenceKind::kSnapshotUnclaimed,
            "a later mbp-10 snapshot replaced this one before any boundary claimed it",
            it->second.rec, context_of(it->second));
        it->second = pulled;
      }
    }
    note_held();
  }

  [[nodiscard]] const Depth10& previous(std::uint32_t instrument_id) const {
    const auto it = last_.find(instrument_id);
    return it == last_.end() ? kPaddedDepth : it->second;
  }

  [[nodiscard]] static DivergenceContext context_of(const Pulled& pulled) {
    DivergenceContext ctx;
    ctx.venue_record = pulled.rec;
    ctx.venue_record_index = pulled.index;
    return ctx;
  }

  void note_held() noexcept {
    report_.records_held = static_cast<std::uint64_t>(stash_.size()) + (head_ ? 1U : 0U);
  }

  void add(DivergenceKind kind, const char* what, const MboMsg& rec, DivergenceContext ctx) {
    Divergence d;
    d.instrument_id = rec.hd.instrument_id;
    d.sequence = rec.sequence;
    d.ts_event = rec.hd.ts_event;
    d.ts_recv = rec.ts_recv;
    d.action = rec.action;
    store(kind, what, d, std::move(ctx));
  }

  void add(DivergenceKind kind, const char* what, const Mbp10Msg& rec, DivergenceContext ctx) {
    Divergence d;
    d.instrument_id = rec.hd.instrument_id;
    d.sequence = rec.sequence;
    d.ts_event = rec.hd.ts_event;
    d.ts_recv = rec.ts_recv;
    d.action = rec.action;
    store(kind, what, d, std::move(ctx));
  }

  void store(DivergenceKind kind, const char* what, Divergence& d, DivergenceContext ctx) {
    d.kind = kind;
    d.what = what;
    d.record_index = record_index_;
    if (report_.divergence_count == 0) {
      ctx.records = context_records();
      ctx.last_record_index = report_.records == 0 ? 0 : report_.records - 1;
      report_.first_divergence = std::move(ctx);
    }
    ++report_.divergence_count;
    ++report_.divergences_by_kind[static_cast<std::size_t>(kind)];
    ++report_.instruments[d.instrument_id].divergences;
    if (report_.divergences.size() < opts_.max_divergences) {
      report_.divergences.push_back(d);
    }
  }

  void push_context(const MboMsg& rec) {
    ring_[ring_next_] = rec;
    ring_next_ = (ring_next_ + 1) % kContextDepth;
    if (ring_filled_ < kContextDepth) {
      ++ring_filled_;
    }
  }

  [[nodiscard]] std::vector<MboMsg> context_records() const {
    std::vector<MboMsg> out;
    out.reserve(ring_filled_);
    const std::size_t start = (ring_next_ + kContextDepth - ring_filled_) % kContextDepth;
    for (std::size_t i = 0; i < ring_filled_; ++i) {
      out.push_back(ring_[(start + i) % kContextDepth]);
    }
    return out;
  }

  const Book* book_;
  Source source_;
  Options opts_;
  Mbp10DiffReport report_{};

  std::optional<Pulled> head_;
  std::map<std::uint32_t, Pulled> stash_;
  std::map<std::uint32_t, Depth10> last_;
  /// Each instrument's last A, C or M since its previous boundary.
  std::map<std::uint32_t, MboMsg> event_mutation_;
  std::uint64_t record_index_ = 0;

  std::array<MboMsg, kContextDepth> ring_{};
  std::size_t ring_next_ = 0;
  std::size_t ring_filled_ = 0;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_MBP10_DIFF_HPP
