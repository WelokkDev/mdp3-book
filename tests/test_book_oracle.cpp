#include "bookreplay/book.hpp"
#include "bookreplay/book_oracle.hpp"
#include "bookreplay/book_view.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/depth.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "book_types.hpp"
#include "synthetic_stream.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::px;
using testing::StreamBuilder;

constexpr std::uint32_t kIid = 42004177;

enum class Fault : std::uint8_t {
  kHideOrder,
  kOrderSize,
  kQueueAhead,
  kLevelTotal,
  kLevelCount,
  kGhostLevel,
  kFilled,
  kTouch,
  kTopTen,
  kExtraInstrument,
  kExtraLevel,
  kSwappedQueue,
  kCounter,
};

/// A reference book that lies about exactly one thing, so each check can be
/// shown to catch its own kind of mistake and nothing else's.
class SkewedBook {
 public:
  explicit SkewedBook(Fault fault, std::uint64_t target = 0, std::int64_t price = 0)
      : fault_(fault), target_(target), price_(price) {}

  void apply(const MboMsg& rec) { book_.apply(rec); }

  [[nodiscard]] std::uint64_t mutation_count() const {
    return book_.mutation_count() + (fault_ == Fault::kCounter ? 1 : 0);
  }

  [[nodiscard]] std::uint64_t unknown_modifies() const { return book_.unknown_modifies(); }

  [[nodiscard]] std::uint64_t duplicate_adds() const { return book_.duplicate_adds(); }

  [[nodiscard]] std::size_t order_count() const { return book_.order_count(); }

  [[nodiscard]] std::vector<std::uint32_t> instruments() const {
    std::vector<std::uint32_t> out = book_.instruments();
    if (fault_ == Fault::kExtraInstrument) {
      out.push_back(99'999'999);
    }
    return out;
  }

  [[nodiscard]] std::uint64_t queue_ahead(std::uint32_t instrument_id,
                                          std::uint64_t order_id) const {
    return book_.queue_ahead(instrument_id, order_id) + (fault_ == Fault::kQueueAhead ? 1 : 0);
  }

  [[nodiscard]] std::int64_t best_bid(std::uint32_t instrument_id) const {
    const std::int64_t best = book_.best_bid(instrument_id);
    return fault_ == Fault::kTouch && best != kUndefPrice ? best + testing::kTick : best;
  }

  [[nodiscard]] std::int64_t best_ask(std::uint32_t instrument_id) const {
    return book_.best_ask(instrument_id);
  }

  friend Depth10 top_ten(const SkewedBook& b, std::uint32_t instrument_id) {
    Depth10 depth = top_ten(b.book_, instrument_id);
    if (b.fault_ == Fault::kTopTen) {
      ++depth[0].bid_ct;
    }
    return depth;
  }

  friend std::optional<RestingView> resting_view(const SkewedBook& b, std::uint32_t instrument_id,
                                                 std::uint64_t order_id) {
    std::optional<RestingView> view = resting_view(b.book_, instrument_id, order_id);
    if (view && b.fault_ == Fault::kHideOrder && order_id == b.target_) {
      return std::nullopt;
    }
    if (view && b.fault_ == Fault::kOrderSize) {
      ++view->size;
    }
    if (view && b.fault_ == Fault::kFilled && view->filled != 0) {
      ++view->filled;
    }
    return view;
  }

  friend std::optional<LevelView> level_view(const SkewedBook& b, std::uint32_t instrument_id,
                                             Side side, std::int64_t price) {
    std::optional<LevelView> view = level_view(b.book_, instrument_id, side, price);
    if (view && b.fault_ == Fault::kLevelTotal) {
      ++view->total;
    }
    if (view && b.fault_ == Fault::kLevelCount) {
      ++view->count;
    }
    if (!view && b.fault_ == Fault::kGhostLevel && price == b.price_) {
      return LevelView{price, 5, 1};
    }
    return view;
  }

  friend std::vector<LevelView> ladder_view(const SkewedBook& b, std::uint32_t instrument_id,
                                            Side side) {
    std::vector<LevelView> view = ladder_view(b.book_, instrument_id, side);
    if (b.fault_ == Fault::kExtraLevel && side == Side::kAsk && !view.empty()) {
      view.push_back(LevelView{view.back().price + testing::kOne, 1, 1});
    }
    return view;
  }

  friend std::vector<std::uint64_t> queue_view(const SkewedBook& b, std::uint32_t instrument_id,
                                               Side side, std::int64_t price) {
    std::vector<std::uint64_t> view = queue_view(b.book_, instrument_id, side, price);
    if (b.fault_ == Fault::kSwappedQueue && view.size() >= 2) {
      std::swap(view[view.size() - 2], view.back());
    }
    return view;
  }

 private:
  Book book_;
  Fault fault_;
  std::uint64_t target_;
  std::int64_t price_;
};

static_assert(InspectableBook<Book>);
static_assert(InspectableBook<FastBook>);
static_assert(InspectableBook<SkewedBook>);

/// The mistake an aggregate view cannot see: every modify re-queued, as if
/// size and price never let an order keep its place.
class RequeueingBook {
 public:
  void apply(const MboMsg& rec) {
    if (action_of(rec) != Action::kModify || !book_.contains(rec.hd.instrument_id, rec.order_id)) {
      book_.apply(rec);
      return;
    }
    MboMsg cancel = rec;
    cancel.action = static_cast<char>(Action::kCancel);
    MboMsg add = rec;
    add.action = static_cast<char>(Action::kAdd);
    book_.apply(cancel);
    book_.apply(add);
    ++split_;
  }

  [[nodiscard]] std::uint64_t mutation_count() const { return book_.mutation_count() - split_; }

  [[nodiscard]] std::uint64_t unknown_modifies() const { return book_.unknown_modifies(); }

  [[nodiscard]] std::uint64_t duplicate_adds() const { return book_.duplicate_adds(); }

  [[nodiscard]] std::size_t order_count() const { return book_.order_count(); }

  [[nodiscard]] std::vector<std::uint32_t> instruments() const { return book_.instruments(); }

  [[nodiscard]] std::uint64_t queue_ahead(std::uint32_t instrument_id,
                                          std::uint64_t order_id) const {
    return book_.queue_ahead(instrument_id, order_id);
  }

  [[nodiscard]] std::int64_t best_bid(std::uint32_t instrument_id) const {
    return book_.best_bid(instrument_id);
  }

  [[nodiscard]] std::int64_t best_ask(std::uint32_t instrument_id) const {
    return book_.best_ask(instrument_id);
  }

  friend Depth10 top_ten(const RequeueingBook& b, std::uint32_t instrument_id) {
    return top_ten(b.book_, instrument_id);
  }

  friend std::optional<RestingView> resting_view(const RequeueingBook& b,
                                                 std::uint32_t instrument_id,
                                                 std::uint64_t order_id) {
    return resting_view(b.book_, instrument_id, order_id);
  }

  friend std::optional<LevelView> level_view(const RequeueingBook& b, std::uint32_t instrument_id,
                                             Side side, std::int64_t price) {
    return level_view(b.book_, instrument_id, side, price);
  }

  friend std::vector<LevelView> ladder_view(const RequeueingBook& b, std::uint32_t instrument_id,
                                            Side side) {
    return ladder_view(b.book_, instrument_id, side);
  }

  friend std::vector<std::uint64_t> queue_view(const RequeueingBook& b, std::uint32_t instrument_id,
                                               Side side, std::int64_t price) {
    return queue_view(b.book_, instrument_id, side, price);
  }

 private:
  Book book_;
  std::uint64_t split_ = 0;
};

template <typename Candidate>
OracleReport run_oracle(const std::vector<MboMsg>& records, Candidate& candidate,
                        typename BookOracle<Book, Candidate>::Options opts = {}) {
  Book reference;
  BookOracle oracle{reference, candidate, opts};
  for (const MboMsg& rec : records) {
    oracle.before(rec);
    reference.apply(rec);
    candidate.apply(rec);
    oracle.after(rec);
  }
  oracle.finish();
  return oracle.report();
}

/// The opening snapshot, then a second order behind the best bid's first.
[[nodiscard]] std::vector<MboMsg> small_book() {
  StreamBuilder s = testing::opening_snapshot();
  s.add(102, Side::kBid, px(29000), 3).last();
  return s.records();
}

// Every add, cancel and modify the generator writes names a bid or an ask by a
// nonzero id, so counting them by action alone is counting what the oracle
// should check.
TEST(BookOracle, TwoReferenceBooksAgreeAndEveryCheckIsCounted) {
  const std::vector<MboMsg> records = testing::synthetic_stream(20'000);
  std::uint64_t named = 0;
  std::uint64_t boundaries = 0;
  for (const MboMsg& rec : records) {
    const Action action = action_of(rec);
    if (action == Action::kAdd || action == Action::kCancel || action == Action::kModify) {
      ++named;
    }
    if (is_event_boundary(rec)) {
      ++boundaries;
    }
  }

  Book candidate;
  BookOracle<Book, Book>::Options opts;
  opts.audit_period = 5'000;
  const OracleReport report = run_oracle(records, candidate, opts);

  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.finished);
  EXPECT_EQ(report.records, records.size());
  EXPECT_EQ(report.order_checks, named);
  EXPECT_EQ(report.boundary_checks, boundaries);
  EXPECT_EQ(report.audits, records.size() / 5'000 + (records.size() % 5'000 == 0 ? 0 : 1));
}

TEST(BookOracle, AnUnplaceableAddAndACancelWithNoIdAreNotOrderChecks) {
  StreamBuilder s = testing::opening_snapshot();
  s.add(300, Side::kNone, px(29001), 1);
  s.add(0, Side::kAsk, px(29001), 1);
  s.cancel(0, Side::kAsk, px(29001), 1);
  s.trade(Side::kBid, px(29000, 1), 1).fill(200, Side::kAsk, px(29000, 1), 1);
  s.modify(200, Side::kAsk, px(29000, 1), 9).last();

  Book candidate;
  const OracleReport report = run_oracle(s.records(), candidate);

  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.order_checks, 5U);
  EXPECT_EQ(report.fill_checks, 1U);
  EXPECT_EQ(report.boundary_checks, 2U);
  EXPECT_EQ(report.audits, 1U);
}

// A run that ends on the audit's period has just audited; a second audit of
// the same state would count every audit-found difference twice.
TEST(BookOracle, FinishDoesNotRepeatTheAuditTheLastRecordRan) {
  const std::vector<MboMsg> records = small_book();
  BookOracle<Book, SkewedBook>::Options opts;

  SkewedBook on_period{Fault::kExtraLevel};
  opts.audit_period = records.size();
  const OracleReport once = run_oracle(records, on_period, opts);
  EXPECT_EQ(once.audits, 1U);
  EXPECT_EQ(once.count(OracleDifferenceKind::kLadder), 1U);

  SkewedBook off_period{Fault::kExtraLevel};
  opts.audit_period = records.size() - 1;
  const OracleReport twice = run_oracle(records, off_period, opts);
  EXPECT_EQ(twice.audits, 2U);
  EXPECT_EQ(twice.count(OracleDifferenceKind::kLadder), 2U);
}

TEST(BookOracle, TheFastBookAgreesWithTheReference) {
  const std::vector<MboMsg> records = testing::rough_stream(50'000, testing::kTestInstruments);
  FastBook candidate = testing::make_book<FastBook>();
  BookOracle<Book, FastBook>::Options opts;
  opts.audit_period = 10'000;
  const OracleReport report = run_oracle(records, candidate, opts);

  EXPECT_TRUE(report.ok()) << report.difference_count << " differences";
  EXPECT_GT(report.order_checks, 0U);
  EXPECT_GT(report.boundary_checks, 0U);
}

TEST(BookOracle, EachDifferenceKindIsCaughtWhereItFirstShows) {
  struct Case {
    Fault fault;
    OracleDifferenceKind kind;
    const char* field;
    std::uint64_t record_index;
  };

  // Records 1 to 4 are the snapshot's adds, 4 closes its event and 5 adds
  // order 102 behind order 100. The last four only an audit can see, and the
  // only audit here is the one finish() runs after record 5.
  constexpr Case kCases[] = {
      {Fault::kHideOrder, OracleDifferenceKind::kResting, "resting", 5},
      {Fault::kOrderSize, OracleDifferenceKind::kOrder, "size", 1},
      {Fault::kQueueAhead, OracleDifferenceKind::kQueueAhead, "queue_ahead", 1},
      {Fault::kLevelTotal, OracleDifferenceKind::kLevelTotal, "total", 1},
      {Fault::kLevelCount, OracleDifferenceKind::kLevelCount, "count", 1},
      {Fault::kTouch, OracleDifferenceKind::kTouch, "best_bid", 1},
      {Fault::kTopTen, OracleDifferenceKind::kTopTen, "bid_ct", 4},
      {Fault::kExtraInstrument, OracleDifferenceKind::kInstruments, "held", 5},
      {Fault::kExtraLevel, OracleDifferenceKind::kLadder, "held", 5},
      {Fault::kSwappedQueue, OracleDifferenceKind::kQueue, "order_id", 5},
      {Fault::kCounter, OracleDifferenceKind::kCounter, "mutation_count", 5},
  };

  for (const Case& c : kCases) {
    SCOPED_TRACE(oracle_difference_name(c.kind));
    SkewedBook candidate{c.fault, 102};
    BookOracle<Book, SkewedBook>::Options opts;
    opts.audit_period = 0;
    const OracleReport report = run_oracle(small_book(), candidate, opts);

    EXPECT_FALSE(report.ok());
    EXPECT_GT(report.count(c.kind), 0U);
    EXPECT_EQ(report.count(c.kind), report.difference_count);
    ASSERT_FALSE(report.differences.empty());
    const OracleDifference& first = report.differences.front();
    EXPECT_EQ(first.kind, c.kind);
    EXPECT_STREQ(first.field, c.field);
    EXPECT_EQ(first.record_index, c.record_index);
  }
}

// In a replay a record's own check reports these first, which hides whether
// the audit would. Here the books are built before the oracle looks, so the
// audit is the only check there is.
TEST(BookOracle, TheAuditAloneFindsAHiddenOrderAWrongSizeAndAMovedTouch) {
  struct Case {
    Fault fault;
    OracleDifferenceKind kind;
    const char* field;
    std::int64_t price;  ///< of the level the order was queued at; a touch names none
  };

  constexpr Case kCases[] = {
      {Fault::kHideOrder, OracleDifferenceKind::kResting, "resting", px(29000)},
      {Fault::kOrderSize, OracleDifferenceKind::kOrder, "size", px(29000)},
      {Fault::kTouch, OracleDifferenceKind::kTouch, "best_bid", kUndefPrice},
  };

  for (const Case& c : kCases) {
    SCOPED_TRACE(oracle_difference_name(c.kind));
    Book reference;
    SkewedBook candidate{c.fault, 102};
    for (const MboMsg& rec : small_book()) {
      reference.apply(rec);
      candidate.apply(rec);
    }
    BookOracle oracle{reference, candidate};
    oracle.finish();

    const OracleReport& report = oracle.report();
    EXPECT_EQ(report.audits, 1U);
    EXPECT_GT(report.count(c.kind), 0U);
    EXPECT_EQ(report.count(c.kind), report.difference_count);
    ASSERT_FALSE(report.differences.empty());
    const OracleDifference& first = report.differences.front();
    EXPECT_STREQ(first.field, c.field);
    EXPECT_EQ(first.side, Side::kBid);
    EXPECT_EQ(first.price, c.price);
  }
}

// Neither the record's price nor any later view names the level a modify
// leaves; only the reference, asked before the record, can.
TEST(BookOracle, TheLevelAModifyLeavesIsComparedAtTheModify) {
  StreamBuilder s = testing::opening_snapshot();
  s.modify(101, Side::kBid, px(28999, 2), 7).last();
  const std::vector<MboMsg> records = s.records();

  SkewedBook candidate{Fault::kGhostLevel, 0, px(28999, 3)};
  BookOracle<Book, SkewedBook>::Options opts;
  opts.audit_period = 0;
  const OracleReport report = run_oracle(records, candidate, opts);

  ASSERT_FALSE(report.differences.empty());
  const OracleDifference& first = report.differences.front();
  EXPECT_EQ(first.kind, OracleDifferenceKind::kLevelTotal);
  EXPECT_EQ(first.record_index, records.size() - 1);
  EXPECT_EQ(first.price, px(28999, 3));
  EXPECT_EQ(first.reference, 0);
  EXPECT_EQ(first.candidate, 5);
}

// This cancel names no level, so the one it empties is known only from where
// the reference held the order before it.
TEST(BookOracle, ACancelCarryingOnlyAnIdIsStillAnOrderCheck) {
  StreamBuilder s = testing::opening_snapshot();
  s.cancel(101, Side::kNone, kUndefPrice, 0).last();
  const std::vector<MboMsg> records = s.records();

  SkewedBook candidate{Fault::kGhostLevel, 0, px(28999, 3)};
  BookOracle<Book, SkewedBook>::Options opts;
  opts.audit_period = 0;
  const OracleReport report = run_oracle(records, candidate, opts);

  EXPECT_EQ(report.order_checks, 5U);
  ASSERT_FALSE(report.differences.empty());
  const OracleDifference& first = report.differences.front();
  EXPECT_EQ(first.kind, OracleDifferenceKind::kLevelTotal);
  EXPECT_EQ(first.record_index, records.size() - 1);
  EXPECT_EQ(first.side, Side::kBid);
  EXPECT_EQ(first.price, px(28999, 3));
  EXPECT_EQ(first.reference, 0);
  EXPECT_EQ(first.candidate, 5);
}

TEST(BookOracle, FillAttributionIsComparedAtTheFill) {
  StreamBuilder s = testing::opening_snapshot();
  s.trade(Side::kBid, px(29000, 1), 4).fill(200, Side::kAsk, px(29000, 1), 4);
  s.modify(200, Side::kAsk, px(29000, 1), 6).last();
  const std::vector<MboMsg> records = s.records();

  SkewedBook candidate{Fault::kFilled};
  const OracleReport report = run_oracle(records, candidate);

  ASSERT_FALSE(report.differences.empty());
  const OracleDifference& first = report.differences.front();
  EXPECT_EQ(first.kind, OracleDifferenceKind::kOrder);
  EXPECT_STREQ(first.field, "filled");
  EXPECT_EQ(first.record_index, records.size() - 2);
  EXPECT_EQ(first.reference, 4);
  EXPECT_EQ(first.candidate, 5);
  EXPECT_EQ(report.fill_checks, 1U);
}

// The last two of three are swapped: a mismatch at the front would be at
// position 0, which a difference never given a position also reports.
TEST(BookOracle, AQueueSwapIsNamedByPositionAndBothIds) {
  StreamBuilder s = testing::opening_snapshot();
  s.add(102, Side::kBid, px(29000), 3).add(103, Side::kBid, px(29000), 2).last();

  SkewedBook candidate{Fault::kSwappedQueue};
  const OracleReport report = run_oracle(s.records(), candidate);

  ASSERT_EQ(report.count(OracleDifferenceKind::kQueue), 1U);
  const OracleDifference& d = report.differences.front();
  EXPECT_EQ(d.instrument_id, kIid);
  EXPECT_EQ(d.side, Side::kBid);
  EXPECT_EQ(d.price, px(29000));
  EXPECT_EQ(d.position, 1U);
  EXPECT_EQ(d.reference, 102);
  EXPECT_EQ(d.candidate, 103);
}

// Aggregates cannot see this book's mistake: every level total, every count
// and every top ten matches. The oracle sees it at the modify that should have
// kept its place, by the quantity queued ahead of it.
TEST(BookOracle, RequeueingEveryModifyIsCaughtAtTheModify) {
  StreamBuilder s = testing::opening_snapshot();
  s.add(102, Side::kBid, px(29000), 3).last();
  s.modify(100, Side::kBid, px(29000), 6).last();
  const std::vector<MboMsg> records = s.records();

  RequeueingBook candidate;
  const OracleReport report = run_oracle(records, candidate);

  EXPECT_EQ(report.count(OracleDifferenceKind::kTopTen), 0U);
  EXPECT_EQ(report.count(OracleDifferenceKind::kLevelTotal), 0U);
  EXPECT_EQ(report.count(OracleDifferenceKind::kLevelCount), 0U);
  ASSERT_FALSE(report.differences.empty());
  const OracleDifference& first = report.differences.front();
  EXPECT_EQ(first.kind, OracleDifferenceKind::kQueueAhead);
  EXPECT_EQ(first.record_index, records.size() - 1);
  EXPECT_EQ(first.order_id, 100U);
  EXPECT_EQ(first.reference, 0);
  EXPECT_EQ(first.candidate, 3);
}

TEST(BookOracle, TheFirstDifferenceCarriesTheRecordsBeforeIt) {
  StreamBuilder s = testing::opening_snapshot();
  for (std::uint64_t id = 300; id < 310; ++id) {
    s.add(id, Side::kAsk, px(29001), 1).last();
  }
  const std::vector<MboMsg> records = s.records();

  SkewedBook candidate{Fault::kHideOrder, 309};
  const OracleReport report = run_oracle(records, candidate);

  ASSERT_FALSE(report.differences.empty());
  const OracleDifference& first = report.differences.front();
  EXPECT_EQ(first.record_index, records.size() - 1);
  EXPECT_EQ(first.order_id, 309U);
  EXPECT_EQ(first.reference, 1);
  EXPECT_EQ(first.candidate, 0);
  ASSERT_EQ(report.first_difference_context.size(), (BookOracle<Book, SkewedBook>::kContextDepth));
  EXPECT_EQ(report.first_difference_context.back().order_id, 309U);
  EXPECT_EQ(report.first_difference_context.front().order_id, 302U);
}

TEST(BookOracle, TheListIsCappedAndTheCountIsNot) {
  SkewedBook candidate{Fault::kLevelTotal};
  BookOracle<Book, SkewedBook>::Options opts;
  opts.max_differences = 3;
  const OracleReport report = run_oracle(testing::synthetic_stream(1'000), candidate, opts);

  EXPECT_EQ(report.differences.size(), 3U);
  EXPECT_GT(report.difference_count, 3U);
  EXPECT_EQ(report.count(OracleDifferenceKind::kLevelTotal), report.difference_count);
}

}  // namespace
}  // namespace bookreplay
