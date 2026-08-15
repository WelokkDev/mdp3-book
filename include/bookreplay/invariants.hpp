#ifndef BOOKREPLAY_INVARIANTS_HPP
#define BOOKREPLAY_INVARIANTS_HPP

#include "bookreplay/dbn.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bookreplay {

enum class SessionState : std::uint8_t {
  kUnknown,
  kPreOpen,
  kTrading,
  kHalted,
  kClosed,
};

/// `mutation_count()` counts applied records, not orders touched: a Clear
/// counts once. `best_bid`/`best_ask` return kUndefPrice for an empty side.
template <typename B>
concept BookLike = requires(const B& b, std::uint32_t iid, std::uint64_t oid) {
  { b.mutation_count() } -> std::convertible_to<std::uint64_t>;
  { b.contains(iid, oid) } -> std::convertible_to<bool>;
  { b.best_bid(iid) } -> std::convertible_to<std::int64_t>;
  { b.best_ask(iid) } -> std::convertible_to<std::int64_t>;
};

enum class Invariant : std::uint8_t {
  kMutationReconciliation = 1,
  kUncrossedBook = 2,
  kNoMaterialization = 3,
  kWellFormedRecord = 4,
};

struct Violation {
  Invariant invariant{};
  const char* what = "";
  std::uint64_t record_index = 0;
  std::uint32_t instrument_id = 0;
  char action = '\0';
  std::uint64_t order_id = 0;
  std::int64_t best_bid = 0;
  std::int64_t best_ask = 0;
};

struct InvariantReport {
  std::uint64_t records = 0;
  std::uint64_t mutating_records = 0;  ///< A + C + M + R
  std::uint64_t passive_records = 0;   ///< T + F + N
  std::uint64_t observed_mutations = 0;
  std::uint64_t passive_mutations = 0;   ///< violations: a T/F/N that mutated
  std::uint64_t mutation_miscounts = 0;  ///< violations: an A/C/M/R counted != 1
  std::uint64_t boundaries = 0;          ///< F_LAST records seen while Trading
  std::uint64_t cross_checks = 0;        ///< boundaries where both sides existed
  std::uint64_t cross_violations = 0;
  std::uint64_t materializations = 0;
  std::uint64_t dematerializations = 0;  ///< violations: a T/F erased the order it names
  std::uint64_t unknown_order_fills = 0;
  std::uint64_t malformed_records = 0;
  std::vector<Violation> violations;

  /// The first violation's record and the records preceding it, in arrival
  /// order.
  std::vector<MboMsg> first_violation_context;

  [[nodiscard]] bool reconciles() const noexcept { return observed_mutations == mutating_records; }

  [[nodiscard]] bool ok() const noexcept {
    return reconciles() && passive_mutations == 0 && mutation_miscounts == 0 &&
           cross_violations == 0 && materializations == 0 && dematerializations == 0 &&
           malformed_records == 0;
  }
};

/// Call before(), apply the record to the book, then after(). Non-owning.
template <BookLike Book>
class InvariantHarness {
 public:
  static constexpr std::size_t kContextDepth = 8;

  struct Options {
    /// Check the crossed-book invariant every Nth event boundary; 0 disables
    /// it.
    std::uint32_t cross_check_period = 1;
    std::size_t max_violations = 16;
  };

  explicit InvariantHarness(const Book& book, Options opts = {}) : book_(&book), opts_(opts) {}

  void set_session_state(SessionState s) noexcept { session_ = s; }

  void before(const MboMsg& rec) {
    pre_mutations_ = book_->mutation_count();
    pre_contained_ = false;
    if (probes_membership(action_of(rec)) && rec.order_id != 0) {
      pre_contained_ = book_->contains(rec.hd.instrument_id, rec.order_id);
    }
  }

  void after(const MboMsg& rec) {
    ++report_.records;
    push_context(rec);

    check_well_formed(rec);

    const Action action = action_of(rec);
    const std::uint64_t delta = book_->mutation_count() - pre_mutations_;
    report_.observed_mutations += delta;

    if (mutates_book(action)) {
      ++report_.mutating_records;
      if (delta != 1) {
        ++report_.mutation_miscounts;
        add(Invariant::kMutationReconciliation, "an A/C/M/R record counted as != 1 mutation", rec);
      }
    } else {
      ++report_.passive_records;
      if (delta != 0) {
        ++report_.passive_mutations;
        add(Invariant::kMutationReconciliation, "book mutated on a T/F/N record", rec);
      }
    }

    check_materialization(rec, action);
    check_uncrossed(rec);
  }

  [[nodiscard]] const InvariantReport& report() const noexcept { return report_; }

  [[nodiscard]] bool ok() const noexcept { return report_.ok(); }

 private:
  static constexpr bool probes_membership(Action a) noexcept {
    return a == Action::kTrade || a == Action::kFill;
  }

  void check_well_formed(const MboMsg& rec) {
    if (rec.hd.rtype != kRTypeMbo || !is_known_action(rec.action) || !is_known_side(rec.side)) {
      ++report_.malformed_records;
      add(Invariant::kWellFormedRecord, "record is not a well-formed MboMsg", rec);
    }
  }

  void check_materialization(const MboMsg& rec, Action action) {
    if (!probes_membership(action) || rec.order_id == 0) {
      return;
    }
    const bool now_contained = book_->contains(rec.hd.instrument_id, rec.order_id);
    if (!pre_contained_ && now_contained) {
      ++report_.materializations;
      add(Invariant::kNoMaterialization, "order id materialized by a T/F record", rec);
    } else if (pre_contained_ && !now_contained) {
      ++report_.dematerializations;
      add(Invariant::kNoMaterialization, "resting order erased by a T/F record", rec);
    } else if (action == Action::kFill && !pre_contained_ && !now_contained) {
      ++report_.unknown_order_fills;
    }
  }

  // CME and Databento both document the book as undefined mid-event.
  void check_uncrossed(const MboMsg& rec) {
    if (!is_event_boundary(rec) || session_ != SessionState::kTrading) {
      return;
    }
    ++report_.boundaries;
    if (opts_.cross_check_period == 0 || (report_.boundaries % opts_.cross_check_period) != 0) {
      return;
    }
    const std::int64_t bid = book_->best_bid(rec.hd.instrument_id);
    const std::int64_t ask = book_->best_ask(rec.hd.instrument_id);
    if (is_undef_price(bid) || is_undef_price(ask)) {
      return;
    }
    ++report_.cross_checks;
    if (bid >= ask) {
      ++report_.cross_violations;
      Violation v = make(Invariant::kUncrossedBook, "best_bid >= best_ask while Trading", rec);
      v.best_bid = bid;
      v.best_ask = ask;
      store(v);
    }
  }

  [[nodiscard]] Violation make(Invariant inv, const char* what, const MboMsg& rec) const {
    Violation v;
    v.invariant = inv;
    v.what = what;
    v.record_index = report_.records - 1;
    v.instrument_id = rec.hd.instrument_id;
    v.action = rec.action;
    v.order_id = rec.order_id;
    return v;
  }

  void add(Invariant inv, const char* what, const MboMsg& rec) { store(make(inv, what, rec)); }

  void store(const Violation& v) {
    if (report_.violations.empty()) {
      capture_context();
    }
    if (report_.violations.size() < opts_.max_violations) {
      report_.violations.push_back(v);
    }
  }

  void push_context(const MboMsg& rec) {
    ring_[ring_next_] = rec;
    ring_next_ = (ring_next_ + 1) % kContextDepth;
    if (ring_filled_ < kContextDepth) {
      ++ring_filled_;
    }
  }

  void capture_context() {
    report_.first_violation_context.reserve(ring_filled_);
    const std::size_t start = (ring_next_ + kContextDepth - ring_filled_) % kContextDepth;
    for (std::size_t i = 0; i < ring_filled_; ++i) {
      report_.first_violation_context.push_back(ring_[(start + i) % kContextDepth]);
    }
  }

  const Book* book_;
  Options opts_;
  InvariantReport report_{};
  SessionState session_ = SessionState::kUnknown;

  std::uint64_t pre_mutations_ = 0;
  bool pre_contained_ = false;

  std::array<MboMsg, kContextDepth> ring_{};
  std::size_t ring_next_ = 0;
  std::size_t ring_filled_ = 0;
};

/// `Book` must additionally expose `apply(const MboMsg&)`.
template <BookLike Book, typename Range>
InvariantReport run_checked(Book& book, const Range& records, SessionState state,
                            typename InvariantHarness<Book>::Options opts = {}) {
  InvariantHarness<Book> harness(book, opts);
  harness.set_session_state(state);
  for (const MboMsg& rec : records) {
    harness.before(rec);
    book.apply(rec);
    harness.after(rec);
  }
  return harness.report();
}

}  // namespace bookreplay

#endif
