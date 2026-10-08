// Compares two books fed the same stream, down to the order of every queue.
//
// This is the comparison the mbp-10 diff cannot make. The venue publishes
// levels, not queues, so a book that queues an order in the wrong place still
// matches the venue record for record. It does not match a second book that
// queued the order right.
//
// An add, cancel or modify disturbs the order it names, the level it rests at
// and the level it left, and may move a touch, so those are compared after
// every such record; an F changes only its order's attribution, so that is
// compared after every F. A fault there shows at the record that causes it for
// the price of a few lookups. Walking all of both books costs more than the
// replay itself, so what a record reaches without naming it waits for a
// periodic audit: what a clear drops, or an order a faulty book disturbs by
// mistake.

#ifndef BOOKREPLAY_BOOK_ORACLE_HPP
#define BOOKREPLAY_BOOK_ORACLE_HPP

#include "bookreplay/book.hpp"
#include "bookreplay/book_view.hpp"
#include "bookreplay/context_ring.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/depth.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bookreplay {

template <typename B>
concept InspectableBook =
    DepthBook<B> && requires(const B& b, std::uint32_t instrument_id, std::uint64_t order_id,
                             Side side, std::int64_t price) {
      { b.mutation_count() } -> std::convertible_to<std::uint64_t>;
      { b.unknown_modifies() } -> std::convertible_to<std::uint64_t>;
      { b.duplicate_adds() } -> std::convertible_to<std::uint64_t>;
      { b.order_count() } -> std::convertible_to<std::size_t>;
      { b.instruments() } -> std::convertible_to<std::vector<std::uint32_t>>;
      { b.queue_ahead(instrument_id, order_id) } -> std::convertible_to<std::uint64_t>;
      { b.best_bid(instrument_id) } -> std::convertible_to<std::int64_t>;
      { b.best_ask(instrument_id) } -> std::convertible_to<std::int64_t>;
      {
        resting_view(b, instrument_id, order_id)
      } -> std::convertible_to<std::optional<RestingView>>;
      {
        level_view(b, instrument_id, side, price)
      } -> std::convertible_to<std::optional<LevelView>>;
      { ladder_view(b, instrument_id, side) } -> std::convertible_to<std::vector<LevelView>>;
      {
        queue_view(b, instrument_id, side, price)
      } -> std::convertible_to<std::vector<std::uint64_t>>;
    };

enum class OracleDifferenceKind : std::uint8_t {
  kResting,     ///< an order a record or a queue names rests in one book only
  kOrder,       ///< an order rests in both with another side, price, size or fill
  kQueueAhead,  ///< an order rests in both with another quantity ahead of it
  kLevelTotal,
  kLevelCount,
  kTouch,
  kTopTen,
  kInstruments,  ///< one book holds an instrument the other does not
  kLadder,       ///< one book holds a level at a price the other does not
  kQueue,        ///< a level queues other orders, or the same ones in another order
  kCounter
};

inline constexpr std::size_t kOracleDifferenceKinds =
    static_cast<std::size_t>(OracleDifferenceKind::kCounter) + 1;

[[nodiscard]] const char* oracle_difference_name(OracleDifferenceKind kind) noexcept;

struct OracleDifference {
  OracleDifferenceKind kind{};
  /// The value that disagreed: "size", "queue_ahead", "bid_ct", and so on.
  const char* field = "";
  /// The record after which it was found. An audit finds what may have been
  /// caused many records earlier.
  std::uint64_t record_index = 0;
  std::uint32_t instrument_id = 0;
  std::uint64_t order_id = 0;  ///< 0 where no order is implicated
  Side side = Side::kNone;     ///< kNone where no level is implicated
  std::int64_t price = kUndefPrice;
  /// The level's depth in the top ten, or the place in a queue.
  std::uint64_t position = 0;
  std::int64_t reference = 0;
  std::int64_t candidate = 0;
};

struct OracleReport {
  std::uint64_t records = 0;
  /// Adds and modifies naming a placeable order, and cancels carrying an
  /// order id.
  std::uint64_t order_checks = 0;
  std::uint64_t fill_checks = 0;      ///< F records naming an order
  std::uint64_t boundary_checks = 0;  ///< event boundaries whose touches and top ten were compared
  std::uint64_t audits = 0;
  std::array<std::uint64_t, kOracleDifferenceKinds> differences_by_kind{};
  std::uint64_t difference_count = 0;  ///< uncapped, unlike the list
  std::vector<OracleDifference> differences;
  /// The record the first difference was found after, and those before it,
  /// in arrival order.
  std::vector<MboMsg> first_difference_context;
  bool finished = false;

  [[nodiscard]] std::uint64_t count(OracleDifferenceKind kind) const noexcept {
    return differences_by_kind[static_cast<std::size_t>(kind)];
  }

  [[nodiscard]] bool ok() const noexcept { return difference_count == 0; }
};

/// Call before(), apply the record to both books, then call after(). Call
/// finish() when the stream ends or is cut short, but not after a record only
/// one book applied, which its audit would report as their difference.
/// Non-owning.
template <InspectableBook Reference, InspectableBook Candidate>
class BookOracle {
 public:
  static constexpr std::size_t kContextDepth = 8;

  struct Options {
    /// Audit after every Nth record; 0 audits only at finish().
    std::uint64_t audit_period = 1'000'000;
    std::size_t max_differences = 16;
  };

  BookOracle(const Reference& reference, const Candidate& candidate, Options opts = {})
      : reference_(&reference), candidate_(&candidate), opts_(opts) {}

  /// Only the reference, before the record, knows the level an order is about
  /// to leave.
  void before(const MboMsg& rec) {
    left_.reset();
    if (names_order(rec)) {
      if (const std::optional<RestingView> resting =
              resting_view(*reference_, rec.hd.instrument_id, rec.order_id)) {
        left_ = Place{resting->side, resting->price};
      }
    }
  }

  void after(const MboMsg& rec) {
    ++report_.records;
    context_.push(rec);
    const bool named = names_order(rec);
    if (named) {
      check_order(rec);
    } else if (action_of(rec) == Action::kFill && rec.order_id != 0) {
      check_fill(rec);
    }
    const bool boundary = is_event_boundary(rec);
    if (boundary) {
      check_top_ten(rec);
    }
    if (named || boundary) {
      check_touch(rec.hd.instrument_id);
    }
    if (opts_.audit_period != 0 && report_.records % opts_.audit_period == 0) {
      audit();
    }
    left_.reset();
  }

  void finish() {
    if (report_.audits == 0 || audited_at_ != report_.records) {
      audit();
    }
    report_.finished = true;
  }

  [[nodiscard]] const OracleReport& report() const noexcept { return report_; }

  [[nodiscard]] bool ok() const noexcept { return report_.ok(); }

 private:
  struct Place {
    Side side = Side::kNone;
    std::int64_t price = 0;
  };

  // A book erases a cancelled order by its id alone, so a cancel names one
  // whatever side it carries. An add or a modify has to say where it rests.
  [[nodiscard]] static bool names_order(const MboMsg& rec) noexcept {
    const Action action = action_of(rec);
    if (action == Action::kCancel) {
      return rec.order_id != 0;
    }
    return (action == Action::kAdd || action == Action::kModify) &&
           placeable(side_of(rec), rec.order_id);
  }

  void check_order(const MboMsg& rec) {
    ++report_.order_checks;
    const std::uint32_t instrument_id = rec.hd.instrument_id;
    const std::optional<RestingView> in_reference =
        compare_resting(instrument_id, rec.order_id, side_of(rec), rec.price);
    if (in_reference) {
      const std::uint64_t reference_ahead = reference_->queue_ahead(instrument_id, rec.order_id);
      const std::uint64_t candidate_ahead = candidate_->queue_ahead(instrument_id, rec.order_id);
      if (reference_ahead != candidate_ahead) {
        OracleDifference d =
            difference(OracleDifferenceKind::kQueueAhead, "queue_ahead", instrument_id);
        d.order_id = rec.order_id;
        d.side = in_reference->side;
        d.price = in_reference->price;
        d.reference = static_cast<std::int64_t>(reference_ahead);
        d.candidate = static_cast<std::int64_t>(candidate_ahead);
        record(d);
      }
    }
    compare_level_at(instrument_id, side_of(rec), rec.price);
    if (left_ && (left_->side != side_of(rec) || left_->price != rec.price)) {
      compare_level_at(instrument_id, left_->side, left_->price);
    }
  }

  void check_fill(const MboMsg& rec) {
    ++report_.fill_checks;
    (void)compare_resting(rec.hd.instrument_id, rec.order_id, side_of(rec), rec.price);
  }

  /// The reference's view of the order if it rests in both books, else empty.
  /// Resting in one only is reported here, and the empty return keeps
  /// check_order from reporting it again as a queue position.
  std::optional<RestingView> compare_resting(std::uint32_t instrument_id, std::uint64_t order_id,
                                             Side side, std::int64_t price) {
    const std::optional<RestingView> in_reference =
        resting_view(*reference_, instrument_id, order_id);
    const std::optional<RestingView> in_candidate =
        resting_view(*candidate_, instrument_id, order_id);
    if (in_reference.has_value() != in_candidate.has_value()) {
      OracleDifference d = difference(OracleDifferenceKind::kResting, "resting", instrument_id);
      d.order_id = order_id;
      d.side = side;
      d.price = price;
      d.reference = in_reference ? 1 : 0;
      d.candidate = in_candidate ? 1 : 0;
      record(d);
      return std::nullopt;
    }
    if (in_reference) {
      compare_order(instrument_id, order_id, *in_reference, *in_candidate);
    }
    return in_reference;
  }

  void compare_level_at(std::uint32_t instrument_id, Side side, std::int64_t price) {
    const LevelView empty{price, 0, 0};
    compare_level(instrument_id, side,
                  level_view(*reference_, instrument_id, side, price).value_or(empty),
                  level_view(*candidate_, instrument_id, side, price).value_or(empty));
  }

  void check_touch(std::uint32_t instrument_id) {
    compare_touch(instrument_id, Side::kBid, reference_->best_bid(instrument_id),
                  candidate_->best_bid(instrument_id));
    compare_touch(instrument_id, Side::kAsk, reference_->best_ask(instrument_id),
                  candidate_->best_ask(instrument_id));
  }

  void compare_touch(std::uint32_t instrument_id, Side side, std::int64_t in_reference,
                     std::int64_t in_candidate) {
    if (in_reference != in_candidate) {
      OracleDifference d = difference(OracleDifferenceKind::kTouch,
                                      side == Side::kBid ? "best_bid" : "best_ask", instrument_id);
      d.side = side;
      d.reference = in_reference;
      d.candidate = in_candidate;
      record(d);
    }
  }

  void check_top_ten(const MboMsg& rec) {
    ++report_.boundary_checks;
    const Depth10 reference_top = top_ten(*reference_, rec.hd.instrument_id);
    const Depth10 candidate_top = top_ten(*candidate_, rec.hd.instrument_id);
    if (const std::optional<DepthMismatch> mismatch =
            first_mismatch(reference_top, candidate_top)) {
      OracleDifference d =
          difference(OracleDifferenceKind::kTopTen, mismatch->field, rec.hd.instrument_id);
      d.position = mismatch->level;
      d.reference = mismatch->ours;
      d.candidate = mismatch->theirs;
      record(d);
    }
  }

  void audit() {
    ++report_.audits;
    audited_at_ = report_.records;
    compare_counter("mutation_count", reference_->mutation_count(), candidate_->mutation_count());
    compare_counter("unknown_modifies", reference_->unknown_modifies(),
                    candidate_->unknown_modifies());
    compare_counter("duplicate_adds", reference_->duplicate_adds(), candidate_->duplicate_adds());
    compare_counter("order_count", reference_->order_count(), candidate_->order_count());

    const std::vector<std::uint32_t> in_reference = reference_->instruments();
    const std::vector<std::uint32_t> in_candidate = candidate_->instruments();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < in_reference.size() || j < in_candidate.size()) {
      std::uint32_t instrument_id = 0;
      if (j == in_candidate.size() ||
          (i < in_reference.size() && in_reference[i] < in_candidate[j])) {
        instrument_id = in_reference[i++];
        record_instrument_held(instrument_id, true);
      } else if (i == in_reference.size() || in_candidate[j] < in_reference[i]) {
        instrument_id = in_candidate[j++];
        record_instrument_held(instrument_id, false);
      } else {
        instrument_id = in_reference[i++];
        ++j;
      }
      audit_side(instrument_id, Side::kBid);
      audit_side(instrument_id, Side::kAsk);
      // A book may cache its touch instead of reading it off the ladder, so
      // ladders that agree do not settle it.
      check_touch(instrument_id);
    }
  }

  // Both ladders run best first, so they merge on price like two sorted lists.
  void audit_side(std::uint32_t instrument_id, Side side) {
    const std::vector<LevelView> in_reference = ladder_view(*reference_, instrument_id, side);
    const std::vector<LevelView> in_candidate = ladder_view(*candidate_, instrument_id, side);
    const auto better = [side](std::int64_t a, std::int64_t b) {
      return side == Side::kBid ? a > b : a < b;
    };
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < in_reference.size() || j < in_candidate.size()) {
      if (j == in_candidate.size() ||
          (i < in_reference.size() && better(in_reference[i].price, in_candidate[j].price))) {
        record_level_held(instrument_id, side, in_reference[i++].price, true);
      } else if (i == in_reference.size() || better(in_candidate[j].price, in_reference[i].price)) {
        record_level_held(instrument_id, side, in_candidate[j++].price, false);
      } else {
        compare_level(instrument_id, side, in_reference[i], in_candidate[j]);
        audit_queue(instrument_id, side, in_reference[i].price);
        ++i;
        ++j;
      }
    }
  }

  void audit_queue(std::uint32_t instrument_id, Side side, std::int64_t price) {
    const std::vector<std::uint64_t> in_reference =
        queue_view(*reference_, instrument_id, side, price);
    const std::vector<std::uint64_t> in_candidate =
        queue_view(*candidate_, instrument_id, side, price);
    const std::size_t longest = std::max(in_reference.size(), in_candidate.size());
    for (std::size_t k = 0; k < longest; ++k) {
      const std::uint64_t reference_id = k < in_reference.size() ? in_reference[k] : 0;
      const std::uint64_t candidate_id = k < in_candidate.size() ? in_candidate[k] : 0;
      if (reference_id != candidate_id) {
        OracleDifference d = difference(OracleDifferenceKind::kQueue, "order_id", instrument_id);
        d.side = side;
        d.price = price;
        d.position = k;
        d.reference = static_cast<std::int64_t>(reference_id);
        d.candidate = static_cast<std::int64_t>(candidate_id);
        record(d);
        break;
      }
    }
    for (const std::uint64_t order_id : in_reference) {
      (void)compare_resting(instrument_id, order_id, side, price);
    }
  }

  void compare_order(std::uint32_t instrument_id, std::uint64_t order_id,
                     const RestingView& in_reference, const RestingView& in_candidate) {
    const char* field = nullptr;
    std::int64_t reference_value = 0;
    std::int64_t candidate_value = 0;
    if (in_reference.side != in_candidate.side) {
      field = "side";
      reference_value = static_cast<char>(in_reference.side);
      candidate_value = static_cast<char>(in_candidate.side);
    } else if (in_reference.price != in_candidate.price) {
      field = "price";
      reference_value = in_reference.price;
      candidate_value = in_candidate.price;
    } else if (in_reference.size != in_candidate.size) {
      field = "size";
      reference_value = in_reference.size;
      candidate_value = in_candidate.size;
    } else if (in_reference.filled != in_candidate.filled) {
      field = "filled";
      reference_value = static_cast<std::int64_t>(in_reference.filled);
      candidate_value = static_cast<std::int64_t>(in_candidate.filled);
    } else {
      return;
    }
    OracleDifference d = difference(OracleDifferenceKind::kOrder, field, instrument_id);
    d.order_id = order_id;
    d.side = in_reference.side;
    d.price = in_reference.price;
    d.reference = reference_value;
    d.candidate = candidate_value;
    record(d);
  }

  void compare_level(std::uint32_t instrument_id, Side side, const LevelView& in_reference,
                     const LevelView& in_candidate) {
    if (in_reference.total != in_candidate.total) {
      OracleDifference d = difference(OracleDifferenceKind::kLevelTotal, "total", instrument_id);
      d.side = side;
      d.price = in_reference.price;
      d.reference = static_cast<std::int64_t>(in_reference.total);
      d.candidate = static_cast<std::int64_t>(in_candidate.total);
      record(d);
    }
    if (in_reference.count != in_candidate.count) {
      OracleDifference d = difference(OracleDifferenceKind::kLevelCount, "count", instrument_id);
      d.side = side;
      d.price = in_reference.price;
      d.reference = static_cast<std::int64_t>(in_reference.count);
      d.candidate = static_cast<std::int64_t>(in_candidate.count);
      record(d);
    }
  }

  void compare_counter(const char* field, std::uint64_t in_reference, std::uint64_t in_candidate) {
    if (in_reference != in_candidate) {
      OracleDifference d = difference(OracleDifferenceKind::kCounter, field, 0);
      d.reference = static_cast<std::int64_t>(in_reference);
      d.candidate = static_cast<std::int64_t>(in_candidate);
      record(d);
    }
  }

  void record_instrument_held(std::uint32_t instrument_id, bool by_reference) {
    OracleDifference d = difference(OracleDifferenceKind::kInstruments, "held", instrument_id);
    d.reference = by_reference ? 1 : 0;
    d.candidate = by_reference ? 0 : 1;
    record(d);
  }

  void record_level_held(std::uint32_t instrument_id, Side side, std::int64_t price,
                         bool by_reference) {
    OracleDifference d = difference(OracleDifferenceKind::kLadder, "held", instrument_id);
    d.side = side;
    d.price = price;
    d.reference = by_reference ? 1 : 0;
    d.candidate = by_reference ? 0 : 1;
    record(d);
  }

  [[nodiscard]] OracleDifference difference(OracleDifferenceKind kind, const char* field,
                                            std::uint32_t instrument_id) const {
    OracleDifference d;
    d.kind = kind;
    d.field = field;
    d.record_index = report_.records == 0 ? 0 : report_.records - 1;
    d.instrument_id = instrument_id;
    return d;
  }

  void record(const OracleDifference& d) {
    if (report_.difference_count == 0) {
      report_.first_difference_context = context_.items();
    }
    ++report_.difference_count;
    ++report_.differences_by_kind[static_cast<std::size_t>(d.kind)];
    if (report_.differences.size() < opts_.max_differences) {
      report_.differences.push_back(d);
    }
  }

  const Reference* reference_;
  const Candidate* candidate_;
  Options opts_;
  OracleReport report_{};
  std::uint64_t audited_at_ = 0;
  ContextRing<MboMsg, kContextDepth> context_;
  /// Where the order the current record names rested before it, per the
  /// reference; empty if it rested nowhere.
  std::optional<Place> left_;
};

}  // namespace bookreplay

#endif  // BOOKREPLAY_BOOK_ORACLE_HPP
