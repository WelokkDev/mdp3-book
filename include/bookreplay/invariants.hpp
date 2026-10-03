#ifndef BOOKREPLAY_INVARIANTS_HPP
#define BOOKREPLAY_INVARIANTS_HPP

#include "bookreplay/context_ring.hpp"
#include "bookreplay/dbn.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bookreplay {

enum class SessionState : std::uint8_t {
  kUnknown,
  kPreOpen,
  kTrading,
  kHalted,
  kClosed,
};

/// The venue reports a mid-session halt as a pre-open carrying a market-event
/// reason rather than as a halt action: NQ did exactly that on 2026-08-25 at
/// 14:30:27 UTC. So `is_trading` decides, and the action only names which
/// non-trading state it was.
[[nodiscard]] constexpr SessionState session_state_of(const StatusMsg& status) noexcept {
  if (status.is_trading == kTriStateYes) {
    return SessionState::kTrading;
  }
  switch (status.action) {
    case kStatusActionPreOpen:
    case kStatusActionPreCross:
    case kStatusActionQuoting:
    case kStatusActionCross:
    case kStatusActionRotation:
    case kStatusActionNewPriceIndication:
      return SessionState::kPreOpen;
    case kStatusActionHalt:
    case kStatusActionPause:
    case kStatusActionSuspend:
      return SessionState::kHalted;
    case kStatusActionPreClose:
    case kStatusActionClose:
    case kStatusActionPostClose:
    case kStatusActionNotAvailableForTrading:
      return SessionState::kClosed;
    default:
      return SessionState::kUnknown;
  }
}

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
  std::uint64_t status_records = 0;
  /// Status records the mapping could not place. Each one stops the
  /// crossed-book check for its instrument until the venue speaks again.
  std::uint64_t unmapped_status_records = 0;
  std::uint64_t mutating_records = 0;  ///< A + C + M + R
  std::uint64_t passive_records = 0;   ///< T + F + N
  std::uint64_t observed_mutations = 0;
  std::uint64_t passive_mutations = 0;   ///< violations: a T/F/N that mutated
  std::uint64_t mutation_miscounts = 0;  ///< violations: an A/C/M/R counted != 1
  std::uint64_t boundaries = 0;          ///< F_LAST records seen while Trading
  /// F_LAST records seen while the instrument was not trading. A run whose
  /// status records never arrived checks nothing and still reports ok(); this
  /// is what makes that visible.
  std::uint64_t boundaries_outside_trading = 0;
  std::uint64_t cross_checks = 0;  ///< boundaries where both sides existed
  std::uint64_t cross_violations = 0;
  std::uint64_t materializations = 0;
  std::uint64_t dematerializations = 0;  ///< violations: a T/F erased the order it names
  std::uint64_t unknown_order_fills = 0;
  std::uint64_t malformed_records = 0;
  std::vector<Violation> violations;  ///< capped by max_violations, unlike the counters

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

  /// The state of an instrument no status record has described yet. A stream
  /// carrying no status records at all is therefore entirely this, which is
  /// how the toy fixtures pin themselves Trading.
  void set_session_state(SessionState s) noexcept { default_state_ = s; }

  void observe(const StatusMsg& status) {
    const SessionState state = session_state_of(status);
    states_[status.hd.instrument_id] = state;
    ++report_.status_records;
    if (state == SessionState::kUnknown) {
      ++report_.unmapped_status_records;
    }
  }

  [[nodiscard]] SessionState session_state(std::uint32_t instrument_id) const noexcept {
    const auto it = states_.find(instrument_id);
    return it == states_.end() ? default_state_ : it->second;
  }

  void before(const MboMsg& rec) {
    pre_mutations_ = book_->mutation_count();
    pre_contained_ = false;
    if (probes_membership(action_of(rec)) && rec.order_id != 0) {
      pre_contained_ = book_->contains(rec.hd.instrument_id, rec.order_id);
    }
  }

  void after(const MboMsg& rec) {
    ++report_.records;
    context_.push(rec);

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
    if (!is_event_boundary(rec)) {
      return;
    }
    if (session_state(rec.hd.instrument_id) != SessionState::kTrading) {
      ++report_.boundaries_outside_trading;
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
    if (!context_captured_) {
      context_captured_ = true;
      report_.first_violation_context = context_.items();
    }
    if (report_.violations.size() < opts_.max_violations) {
      report_.violations.push_back(v);
    }
  }

  const Book* book_;
  Options opts_;
  InvariantReport report_{};
  std::unordered_map<std::uint32_t, SessionState> states_;
  SessionState default_state_ = SessionState::kUnknown;

  std::uint64_t pre_mutations_ = 0;
  bool pre_contained_ = false;

  ContextRing<MboMsg, kContextDepth> context_;
  bool context_captured_ = false;
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

#endif  // BOOKREPLAY_INVARIANTS_HPP
