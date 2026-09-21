#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/mbp10_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::px;
using testing::StreamBuilder;

constexpr std::uint64_t k0 = 0;
constexpr std::uint64_t k1 = 1;
constexpr std::uint64_t k2 = 2;

/// StreamBuilder's default instrument, and a second to interleave with it.
constexpr std::uint32_t kFirst = 42004177;
constexpr std::uint32_t kSecond = 42013467;

/// A book whose top ten is whatever the test says it is, so a book can be
/// wrong in one of the sixty values and right in every other.
class FakeBook {
 public:
  void set(std::uint32_t instrument_id, const Depth10& depth) { tops_[instrument_id] = depth; }

  [[nodiscard]] const Depth10& top(std::uint32_t instrument_id) const {
    const auto it = tops_.find(instrument_id);
    return it == tops_.end() ? kPaddedDepth : it->second;
  }

 private:
  std::map<std::uint32_t, Depth10> tops_;
};

[[nodiscard]] Depth10 top_ten(const FakeBook& book, std::uint32_t instrument_id) {
  return book.top(instrument_id);
}

static_assert(DepthBook<Book>);
static_assert(DepthBook<FakeBook>);

/// BidAskPair carries no equality of its own, and a failure that names the
/// level and field beats one that dumps 640 bytes.
[[nodiscard]] ::testing::AssertionResult depth_eq(const Depth10& ours, const Depth10& theirs) {
  const std::optional<DepthMismatch> mismatch = first_mismatch(ours, theirs);
  if (!mismatch) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "level " << mismatch->level << ' ' << mismatch->field
                                       << ": " << mismatch->ours << " != " << mismatch->theirs;
}

/// Levels shallowest first, each in wire order: bid_px, ask_px, bid_sz,
/// ask_sz, bid_ct, ask_ct. The rest pad.
[[nodiscard]] Depth10 depth(std::initializer_list<BidAskPair> levels) {
  Depth10 out = kPaddedDepth;
  std::size_t i = 0;
  for (const BidAskPair& level : levels) {
    out[i++] = level;
  }
  return out;
}

/// The book `testing::opening_snapshot()` leaves behind.
[[nodiscard]] Depth10 opening_depth() {
  return depth(
      {{px(29000, 0), px(29000, 1), 10, 10, 1, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
}

/// Ten bid levels a tick apart from `best`, ten lots and one order each.
[[nodiscard]] Depth10 bid_ladder(std::int64_t best) {
  Depth10 out = kPaddedDepth;
  for (std::size_t i = 0; i < kDepthLevels; ++i) {
    out[i].bid_px = best - static_cast<std::int64_t>(i) * testing::kTick;
    out[i].bid_sz = 10;
    out[i].bid_ct = 1;
  }
  return out;
}

[[nodiscard]] Mbp10Msg stamped_record(const MboMsg& stamp, const Depth10& levels) {
  Mbp10Msg r{};
  r.hd.length = kLengthUnits<Mbp10Msg>;
  r.hd.rtype = kRTypeMbp10;
  r.hd.publisher_id = stamp.hd.publisher_id;
  r.hd.instrument_id = stamp.hd.instrument_id;
  r.hd.ts_event = stamp.hd.ts_event;
  r.ts_recv = stamp.ts_recv;
  r.ts_in_delta = stamp.ts_in_delta;
  r.sequence = stamp.sequence;
  r.price = stamp.price;
  r.size = stamp.size;
  r.levels = levels;
  return r;
}

/// Side is 'N' throughout because the differ must not read it.
[[nodiscard]] Mbp10Msg book_update(const MboMsg& closing, const MboMsg& mutation,
                                   const Depth10& levels) {
  Mbp10Msg r = stamped_record(closing, levels);
  r.action = mutation.action;
  r.price = mutation.price;
  r.size = mutation.size;
  r.side = static_cast<char>(Side::kNone);
  r.flags = kFlagLast;
  return r;
}

/// The usual event, closed by its own last mutation.
[[nodiscard]] Mbp10Msg book_update(const MboMsg& closing, const Depth10& levels) {
  return book_update(closing, closing, levels);
}

/// One packet, several events for one instrument: the venue stamps them all
/// with one sequence and one pair of timestamps.
void share_key(std::vector<MboMsg>& records, std::size_t from, const MboMsg& key) {
  for (std::size_t i = from; i < records.size(); ++i) {
    records[i].hd.ts_event = key.hd.ts_event;
    records[i].ts_recv = key.ts_recv;
    records[i].sequence = key.sequence;
  }
}

/// A trade record repeats its print and carries no F_LAST.
[[nodiscard]] Mbp10Msg trade_record(const MboMsg& print, const Depth10& levels) {
  Mbp10Msg r = stamped_record(print, levels);
  r.action = static_cast<char>(Action::kTrade);
  r.side = print.side;
  r.flags = 0;
  return r;
}

/// The file's opening picture of one instrument. The sequence is the feed's
/// and nothing joins on it.
[[nodiscard]] Mbp10Msg venue_snapshot(std::uint32_t instrument_id, const Depth10& levels) {
  Mbp10Msg r{};
  r.hd.length = kLengthUnits<Mbp10Msg>;
  r.hd.rtype = kRTypeMbp10;
  r.hd.publisher_id = 1;
  r.hd.instrument_id = instrument_id;
  r.price = kUndefPrice;
  r.action = static_cast<char>(Action::kAdd);
  r.side = static_cast<char>(Side::kNone);
  r.flags = kFlagLast | kFlagSnapshot | kFlagBadTsRecv;
  r.sequence = 192'304'724;
  r.levels = levels;
  return r;
}

class VectorSource {
 public:
  explicit VectorSource(const std::vector<Mbp10Msg>* records) : records_(records) {}

  const Mbp10Msg* operator()() {
    return next_ < records_->size() ? &(*records_)[next_++] : nullptr;
  }

 private:
  const std::vector<Mbp10Msg>* records_;
  std::size_t next_ = 0;
};

using BookDiff = Mbp10Diff<Book, VectorSource>;

[[nodiscard]] Mbp10DiffReport run_diff(const std::vector<MboMsg>& records,
                                       const std::vector<Mbp10Msg>& venue, bool finish = true) {
  Book book;
  VectorSource source{&venue};
  BookDiff diff{book, source};
  for (const MboMsg& rec : records) {
    book.apply(rec);
    diff.after(rec);
  }
  if (finish) {
    diff.finish();
  }
  return diff.report();
}

[[nodiscard]] Book warmed_book() {
  const StreamBuilder opening = testing::opening_snapshot();
  Book book;
  for (const MboMsg& rec : opening.records()) {
    book.apply(rec);
  }
  return book;
}

TEST(TopTen, AnEmptyBookAndAnUnknownInstrumentAreAllPadding) {
  const Book empty;
  EXPECT_TRUE(depth_eq(top_ten(empty, kFirst), kPaddedDepth));

  const Book book = warmed_book();
  EXPECT_TRUE(depth_eq(top_ten(book, kSecond), kPaddedDepth));
  EXPECT_EQ(kPaddedDepth[0].bid_px, kUndefPrice);
  EXPECT_EQ(kPaddedDepth[0].bid_sz, 0u);
  EXPECT_EQ(kPaddedDepth[0].bid_ct, 0u);
}

TEST(TopTen, LevelsBelowTheDeepestArePadded) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 10);
  b.add(101, Side::kBid, px(28999, 3), 7);
  b.add(200, Side::kAsk, px(29000, 1), 4).last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  const Depth10 expected =
      depth({{px(29000, 0), px(29000, 1), 10, 4, 1, 1}, {px(28999, 3), kUndefPrice, 7, 0, 1, 0}});
  EXPECT_TRUE(depth_eq(top_ten(book, kFirst), expected));
}

TEST(TopTen, BidsDescendAndAsksAscend) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(28999, 3), 7);
  b.add(101, Side::kBid, px(29000, 0), 10);
  b.add(200, Side::kAsk, px(29000, 2), 7);
  b.add(201, Side::kAsk, px(29000, 1), 10).last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  const Depth10 top = top_ten(book, kFirst);
  EXPECT_EQ(top[0].bid_px, px(29000, 0));
  EXPECT_EQ(top[1].bid_px, px(28999, 3));
  EXPECT_EQ(top[0].ask_px, px(29000, 1));
  EXPECT_EQ(top[1].ask_px, px(29000, 2));
}

TEST(TopTen, TheEleventhLevelIsExcluded) {
  StreamBuilder b;
  for (std::int64_t i = 0; i < 11; ++i) {
    b.add(100 + static_cast<std::uint64_t>(i), Side::kBid, px(29000, -i), 10);
  }
  b.last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  EXPECT_TRUE(depth_eq(top_ten(book, kFirst), bid_ladder(px(29000, 0))));
  EXPECT_NE(book.level(kFirst, Side::kBid, px(29000, -10)), nullptr);
}

TEST(TopTen, TheCountIsOrdersAndNotQuantity) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 6);
  b.add(101, Side::kBid, px(29000, 0), 9).last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  const Depth10 top = top_ten(book, kFirst);
  EXPECT_EQ(top[0].bid_sz, 15u);
  EXPECT_EQ(top[0].bid_ct, 2u);
}

TEST(TopTen, AnInPlaceModifyMovesSizeAndNotCount) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 10);
  b.add(101, Side::kBid, px(29000, 0), 5).last();
  b.modify(100, Side::kBid, px(29000, 0), 4).last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  const Depth10 top = top_ten(book, kFirst);
  EXPECT_EQ(top[0].bid_sz, 9u);
  EXPECT_EQ(top[0].bid_ct, 2u);
  EXPECT_EQ(book.queue_ahead(kFirst, 101), std::uint64_t{4});
}

TEST(TopTen, ALevelTotalTooWideForTheWireThrows) {
  constexpr std::uint32_t kHalfOfTheRange = 1U << 31;
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), kHalfOfTheRange);
  b.add(101, Side::kBid, px(29000, 0), kHalfOfTheRange).last();

  Book book;
  for (const MboMsg& rec : b.records()) {
    book.apply(rec);
  }

  EXPECT_EQ(book.level(kFirst, Side::kBid, px(29000, 0))->size, std::uint64_t{1} << 32);
  EXPECT_THROW((void)top_ten(book, kFirst), Mbp10DiffError);
}

TEST(TopTen, AClearLeavesAllPadding) {
  Book book = warmed_book();
  ASSERT_FALSE(depth_eq(top_ten(book, kFirst), kPaddedDepth));

  StreamBuilder b;
  b.clear();
  book.apply(b.records().front());

  EXPECT_TRUE(depth_eq(top_ten(book, kFirst), kPaddedDepth));
}

TEST(FirstMismatch, ReportsTheHighestDisagreement) {
  const Depth10 ours =
      depth({{px(29000, 0), px(29000, 1), 10, 10, 1, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  Depth10 theirs = ours;
  theirs[1].bid_sz = 8;
  theirs[0].ask_ct = 2;

  const std::optional<DepthMismatch> mismatch = first_mismatch(ours, theirs);
  ASSERT_TRUE(mismatch);
  EXPECT_EQ(mismatch->level, std::size_t{0});
  EXPECT_STREQ(mismatch->field, "ask_ct");
  EXPECT_EQ(mismatch->ours, 1);
  EXPECT_EQ(mismatch->theirs, 2);
  EXPECT_TRUE(same_depth(ours, ours));
}

TEST(Mbp10Diff, ASnapshotThenCleanEventsAgreeAndBothIdentitiesHold) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  b.cancel(101, Side::kBid, px(28999, 3), 7).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const Depth10 after_cancel =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {kUndefPrice, px(29000, 2), 0, 7, 0, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records[5], after_add),
                                    book_update(records[6], after_cancel)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_TRUE(report.boundaries_reconcile());
  EXPECT_TRUE(report.records_reconcile());
  EXPECT_TRUE(report.finished);
  EXPECT_EQ(report.snapshots, k1);
  EXPECT_EQ(report.boundaries, k2);
  EXPECT_EQ(report.compared, k2);
  EXPECT_EQ(report.silent, k0);
  EXPECT_EQ(report.records_read, std::uint64_t{3});
  EXPECT_EQ(report.records_held, k0);
  EXPECT_EQ(report.instruments.at(kFirst).compared, k2);
}

TEST(Mbp10Diff, AnEventBelowTheTenthLevelIsSilent) {
  StreamBuilder b;
  b.clear();
  for (std::int64_t i = 0; i < 11; ++i) {
    b.add(100 + static_cast<std::uint64_t>(i), Side::kBid, px(29000, -i), 10).snapshot();
  }
  b.last();
  b.cancel(110, Side::kBid, px(29000, -10), 10).last();

  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, bid_ladder(px(29000, 0)))};

  const Mbp10DiffReport report = run_diff(b.records(), venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.snapshots, k1);
  EXPECT_EQ(report.boundaries, k1);
  EXPECT_EQ(report.silent, k1);
  EXPECT_EQ(report.compared, k0);
  EXPECT_EQ(report.instruments.at(kFirst).silent, k1);
}

TEST(Mbp10Diff, ASharedKeyLeavesTheRecordForTheEventThatMoved) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9).last();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  std::vector<MboMsg> records = b.records();
  share_key(records, 6, records[5]);

  // The record names the second event's add, not the first event's cancel.
  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_add)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.boundaries, k2);
  EXPECT_EQ(report.compared, k1);
  EXPECT_EQ(report.silent, k1);
  EXPECT_EQ(report.deferred, k1);
}

TEST(Mbp10Diff, ADelayedMutationInsideASharedKeyDiverges) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9).last();
  std::vector<MboMsg> records = b.records();
  share_key(records, 6, records[5]);

  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records[5], after_add)};

  // The venue moved on the first event. This book shows the move one event
  // late.
  FakeBook book;
  book.set(kFirst, opening_depth());
  VectorSource source{&venue};
  Mbp10Diff diff{book, source};
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (i == records.size() - 1) {
      book.set(kFirst, after_add);
    }
    diff.after(records[i]);
  }
  diff.finish();

  const Mbp10DiffReport& report = diff.report();
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.deferred, k0);
  EXPECT_EQ(report.count(DivergenceKind::kLevels), k1);
  EXPECT_EQ(report.count(DivergenceKind::kMissingRecord), k1);
  ASSERT_FALSE(report.divergences.empty());
  EXPECT_EQ(report.divergences.front().record_index, std::uint64_t{5});
  ASSERT_TRUE(report.first_divergence.event_mutation);
  EXPECT_EQ(report.first_divergence.event_mutation->order_id, std::uint64_t{102});
}

/// Two events after the opening snapshot share a key, neither moves the top
/// ten, and the venue publishes for the second, naming its last mutation. The
/// levels fit both events, so only the name can say whose the record is: the
/// first event has to leave it and the second has to claim it.
void expect_the_second_event_claims(const StreamBuilder& b) {
  std::vector<MboMsg> records = b.records();
  share_key(records, 5, records.back());
  const std::vector<Mbp10Msg> venue{
      venue_snapshot(kFirst, opening_depth()),
      book_update(records.back(), records[records.size() - 2], opening_depth())};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.deferred, k1);
  EXPECT_EQ(report.silent, k1);
  EXPECT_EQ(report.compared, k1);
  EXPECT_EQ(report.compared_unchanged, k1);
}

TEST(Mbp10Diff, ARecordNamingAnotherEventBySizeAloneIsNotClaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9).last();
  b.add(301, Side::kBid, px(28000, 0), 8);
  b.cancel(301, Side::kBid, px(28000, 0), 8);
  b.none();
  expect_the_second_event_claims(b);
}

TEST(Mbp10Diff, ARecordNamingAnotherEventByPriceAloneIsNotClaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9).last();
  b.add(301, Side::kBid, px(27999, 0), 9);
  b.cancel(301, Side::kBid, px(27999, 0), 9);
  b.none();
  expect_the_second_event_claims(b);
}

TEST(Mbp10Diff, ARecordNamingAnotherEventByActionAloneIsNotClaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.cancel(101, Side::kBid, px(28999, 3), 7);
  b.add(500, Side::kBid, px(28999, 3), 7).last();
  b.add(501, Side::kBid, px(28999, 3), 7);
  b.cancel(501, Side::kBid, px(28999, 3), 7);
  b.none();
  expect_the_second_event_claims(b);
}

TEST(Mbp10Diff, ARecordUnderAnotherKeyIsNotClaimedForNamingTheSameMutation) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9).last();
  b.add(102, Side::kBid, px(29000, 0), 5);
  b.add(301, Side::kBid, px(28000, 0), 9);
  b.cancel(301, Side::kBid, px(28000, 0), 9).last();
  const std::vector<MboMsg> records = b.records();

  // Both events end in the same cancel, in different packets. The record at
  // the head while the first closes is the second's, and only the key says so.
  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_add)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.silent, k1);
  EXPECT_EQ(report.deferred, k0);
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, AMutationDoesNotOutliveItsEvent) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5);
  b.add(103, Side::kBid, px(29000, 0), 5).last();
  b.cancel(102, Side::kBid, px(29000, 0), 5).last();
  b.none();
  b.cancel(103, Side::kBid, px(29000, 0), 5).last();
  std::vector<MboMsg> records = b.records();
  share_key(records, 8, records[7]);

  // Three events in one packet: a cancel, a bare N, the same cancel again.
  // While the N closes, the head is the third event's record, and it names
  // exactly what the first event ended in.
  const Depth10 after_adds =
      depth({{px(29000, 0), px(29000, 1), 20, 10, 3, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const Depth10 after_first_cancel =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  const std::vector<Mbp10Msg> venue{
      venue_snapshot(kFirst, opening_depth()), book_update(records[6], after_adds),
      book_update(records[7], after_first_cancel), book_update(records[9], opening_depth())};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, std::uint64_t{3});
  EXPECT_EQ(report.deferred, k1);
}

TEST(Mbp10Diff, AModifyToANewPriceIsWhatTheRecordNames) {
  StreamBuilder b = testing::opening_snapshot();
  b.modify(101, Side::kBid, px(28999, 2), 7).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_modify =
      depth({{px(29000, 0), px(29000, 1), 10, 10, 1, 1}, {px(28999, 2), px(29000, 2), 7, 7, 1, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_modify)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, ARecordMatchingAnUnchangedTopTenIsClaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(300, Side::kBid, px(28000, 0), 9);
  b.cancel(300, Side::kBid, px(28000, 0), 9);
  b.none();
  const std::vector<MboMsg> records = b.records();

  const std::vector<Mbp10Msg> venue{
      venue_snapshot(kFirst, opening_depth()),
      book_update(records.back(), records[records.size() - 2], opening_depth())};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k1);
  EXPECT_EQ(report.compared_unchanged, k1);
  EXPECT_EQ(report.deferred, k0);
  EXPECT_EQ(report.silent, k0);
}

TEST(Mbp10Diff, AnNClosedEventIsKeyedOnTheCarriersSequence) {
  StreamBuilder b = testing::opening_snapshot();
  b.cancel(101, Side::kBid, px(28999, 3), 7);
  b.none();
  const std::vector<MboMsg> records = b.records();
  ASSERT_NE(records.back().sequence, records[records.size() - 2].sequence);

  const Depth10 after_cancel =
      depth({{px(29000, 0), px(29000, 1), 10, 10, 1, 1}, {kUndefPrice, px(29000, 2), 0, 7, 0, 1}});
  const std::vector<Mbp10Msg> venue{
      venue_snapshot(kFirst, opening_depth()),
      book_update(records.back(), records[records.size() - 2], after_cancel)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k1);
}

/// StreamBuilder only writes the Clear that opens a snapshot. A live one, such
/// as an instrument's first appearance, carries no snapshot flag.
void make_live(MboMsg& clear) {
  clear.flags = kFlagBadTsRecv;
}

TEST(Mbp10Diff, AnAddAfterAClearStillNamesItsEvent) {
  StreamBuilder b = testing::opening_snapshot();
  b.clear();
  b.add(400, Side::kAsk, px(30326, 2), 1).last();
  std::vector<MboMsg> records = b.records();
  make_live(records[5]);

  // The only shape a live Clear took in five days of NQ. Forgetting what came
  // before a Clear must not stop what comes after it from naming the event.
  const Depth10 after_add = depth({{kUndefPrice, px(30326, 2), 0, 1, 0, 1}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_add)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, AMutationBeforeAClearNamesNothing) {
  StreamBuilder b = testing::opening_snapshot();
  b.cancel(101, Side::kBid, px(28999, 3), 7);
  b.clear();
  b.none();
  std::vector<MboMsg> records = b.records();
  make_live(records[6]);

  // The record names the cancel the Clear overtook.
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), records[5], kPaddedDepth)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kMissingRecord), k1);
  EXPECT_EQ(report.count(DivergenceKind::kUnclaimedRecord), k1);
  ASSERT_TRUE(report.first_divergence.venue_record);
  EXPECT_FALSE(report.first_divergence.event_mutation);
}

TEST(Mbp10Diff, TwoInstrumentsInterleaveInOnePacket) {
  StreamBuilder b;
  b.instrument(kFirst).add(100, Side::kBid, px(29000, 0), 10);
  b.instrument(kSecond).add(500, Side::kAsk, px(283, 1), 4);
  b.instrument(kFirst).none();
  b.instrument(kSecond).none();
  const std::vector<MboMsg> records = b.records();

  const Depth10 first = depth({{px(29000, 0), kUndefPrice, 10, 0, 1, 0}});
  const Depth10 second = depth({{kUndefPrice, px(283, 1), 0, 4, 0, 1}});
  // Each record names its own instrument's add, though the other instrument's
  // add is the later of the two in the stream.
  const std::vector<Mbp10Msg> venue{book_update(records[2], records[0], first),
                                    book_update(records[3], records[1], second)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k2);
  EXPECT_EQ(report.instruments.at(kFirst).compared, k1);
  EXPECT_EQ(report.instruments.at(kSecond).compared, k1);
}

TEST(Mbp10Diff, ASilentEventEndingLikeThePublishingOneFailsACorrectBook) {
  StreamBuilder b;
  b.clear();
  for (std::int64_t i = 0; i < 12; ++i) {
    b.add(100 + static_cast<std::uint64_t>(i), Side::kBid, px(29000, -i), 10).snapshot();
  }
  b.add(120, Side::kBid, px(29000, -11), 10).snapshot().last();
  b.cancel(111, Side::kBid, px(29000, -11), 10).last();
  b.cancel(100, Side::kBid, px(29000, 0), 10);
  b.cancel(120, Side::kBid, px(29000, -11), 10).last();
  std::vector<MboMsg> records = b.records();
  share_key(records, 15, records[14]);

  // Both events end in the same cancel of ten lots at the twelfth level. The
  // first is silent and the second publishes, so the record names both and
  // goes to the first.
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, bid_ladder(px(29000, 0))),
                                    book_update(records.back(), bid_ladder(px(29000, -1)))};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kLevels), k1);
  EXPECT_EQ(report.count(DivergenceKind::kMissingRecord), k1);
  ASSERT_FALSE(report.divergences.empty());
  EXPECT_EQ(report.divergences.front().record_index, std::uint64_t{14});
}

TEST(Mbp10Diff, ATradeComparesAgainstThePreviousBoundary) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5);  // the book moves before the print
  b.trade(Side::kBid, px(29000, 1), 10);
  b.fill(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(200, Side::kAsk, px(29000, 1), 10).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_event =
      depth({{px(29000, 0), px(29000, 2), 15, 7, 2, 1}, {px(28999, 3), kUndefPrice, 7, 0, 1, 0}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    trade_record(records[6], opening_depth()),
                                    book_update(records.back(), after_event)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.trades, k1);
  EXPECT_EQ(report.compared, k1);
  EXPECT_EQ(report.instruments.at(kFirst).trades, k1);
}

TEST(Mbp10Diff, AnEmptySideComparesAsPadding) {
  StreamBuilder b = testing::opening_snapshot();
  b.cancel(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(201, Side::kAsk, px(29000, 2), 7).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 bids_only =
      depth({{px(29000, 0), kUndefPrice, 10, 0, 1, 0}, {px(28999, 3), kUndefPrice, 7, 0, 1, 0}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), bids_only)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_TRUE(report.ok());
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, APriceDisagreementIsALevelsDivergence) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  const std::vector<MboMsg> records = b.records();

  Depth10 wrong =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  wrong[1].bid_px = px(28999, 2);
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), wrong)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kLevels), k1);
  ASSERT_TRUE(report.first_divergence.mismatch);
  EXPECT_EQ(report.first_divergence.mismatch->level, std::size_t{1});
  EXPECT_STREQ(report.first_divergence.mismatch->field, "bid_px");
}

TEST(Mbp10Diff, ASizeDisagreementIsALevelsDivergence) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  const std::vector<MboMsg> records = b.records();

  Depth10 wrong =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  wrong[0].bid_sz = 10;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), wrong)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_EQ(report.count(DivergenceKind::kLevels), k1);
  ASSERT_TRUE(report.first_divergence.mismatch);
  EXPECT_STREQ(report.first_divergence.mismatch->field, "bid_sz");
  EXPECT_EQ(report.first_divergence.mismatch->ours, 15);
  EXPECT_EQ(report.first_divergence.mismatch->theirs, 10);
}

TEST(Mbp10Diff, AnOrderCountDisagreementAloneIsALevelsDivergence) {
  const Depth10 per_order = depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}});
  const Depth10 aggregated = depth({{px(29000, 0), px(29000, 1), 15, 10, 1, 1}});

  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 15).last();
  const std::vector<MboMsg> records = b.records();
  const std::vector<Mbp10Msg> venue{book_update(records.back(), per_order)};

  FakeBook book;
  book.set(kFirst, aggregated);
  VectorSource source{&venue};
  Mbp10Diff diff{book, source};
  for (const MboMsg& rec : records) {
    diff.after(rec);
  }
  diff.finish();

  const Mbp10DiffReport& report = diff.report();
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kLevels), k1);
  ASSERT_TRUE(report.first_divergence.mismatch);
  EXPECT_STREQ(report.first_divergence.mismatch->field, "bid_ct");
  EXPECT_EQ(report.first_divergence.mismatch->ours, 1);
  EXPECT_EQ(report.first_divergence.mismatch->theirs, 2);
}

TEST(Mbp10Diff, MovingWhileTheVenueIsQuietIsADivergence) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 10).last();

  const Mbp10DiffReport report = run_diff(b.records(), {});
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.boundaries_reconcile());
  EXPECT_EQ(report.count(DivergenceKind::kMissingRecord), k1);
  EXPECT_EQ(report.boundaries, k1);
  ASSERT_TRUE(report.first_divergence.ours);
  EXPECT_FALSE(report.first_divergence.theirs);
}

TEST(Mbp10Diff, ARecordTheStreamPassesIsUnclaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  b.cancel(102, Side::kBid, px(29000, 0), 5).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  Mbp10Msg stray = book_update(records[5], after_add);
  stray.sequence += 1000;
  stray.ts_recv += 1;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records[5], after_add), stray,
                                    book_update(records[6], opening_depth())};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kUnclaimedRecord), k1);
  EXPECT_EQ(report.compared, k2);
  ASSERT_EQ(report.divergences.size(), std::size_t{1});
  EXPECT_EQ(report.divergences.front().sequence, stray.sequence);
  EXPECT_EQ(report.divergences.front().record_index, std::uint64_t{6});
}

TEST(Mbp10Diff, RecordsLeftAtTheEndAreUnclaimed) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  Mbp10Msg trailing = book_update(records.back(), after_add);
  trailing.ts_recv += 1'000'000;
  trailing.sequence += 1;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_add), trailing};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kUnclaimedRecord), k1);
  EXPECT_EQ(report.divergences.front().record_index, report.records);
}

TEST(Mbp10Diff, WithoutFinishLeftoverRecordsAreNotReported) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_add =
      depth({{px(29000, 0), px(29000, 1), 15, 10, 2, 1}, {px(28999, 3), px(29000, 2), 7, 7, 1, 1}});
  Mbp10Msg trailing = book_update(records.back(), after_add);
  trailing.ts_recv += 1'000'000;
  trailing.sequence += 1;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_add), trailing};

  const Mbp10DiffReport report = run_diff(records, venue, false);
  EXPECT_TRUE(report.ok());
  EXPECT_FALSE(report.finished);
  EXPECT_TRUE(report.records_reconcile());
  EXPECT_EQ(report.records_held, k1);
}

TEST(Mbp10Diff, APrintWithNoTradeRecordIsUnmatched) {
  StreamBuilder b = testing::opening_snapshot();
  b.trade(Side::kBid, px(29000, 1), 10);
  b.fill(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(200, Side::kAsk, px(29000, 1), 10).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_event =
      depth({{px(29000, 0), px(29000, 2), 10, 7, 1, 1}, {px(28999, 3), kUndefPrice, 7, 0, 1, 0}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records.back(), after_event)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kTradeUnmatched), k1);
  EXPECT_EQ(report.trades, k0);
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, ATradeRecordAgainstTheWrongBookIsADivergence) {
  StreamBuilder b = testing::opening_snapshot();
  b.trade(Side::kBid, px(29000, 1), 10);
  b.fill(200, Side::kAsk, px(29000, 1), 10);
  b.cancel(200, Side::kAsk, px(29000, 1), 10).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 after_event =
      depth({{px(29000, 0), px(29000, 2), 10, 7, 1, 1}, {px(28999, 3), kUndefPrice, 7, 0, 1, 0}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    trade_record(records[5], after_event),
                                    book_update(records.back(), after_event)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kTradeLevels), k1);
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, ASnapshotBoundaryWithNoVenueSnapshotDiverges) {
  const Mbp10DiffReport report = run_diff(testing::opening_snapshot().records(), {});
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kSnapshotMissing), k1);
  EXPECT_EQ(report.snapshots, k0);
  EXPECT_EQ(report.boundaries, k0);
}

TEST(Mbp10Diff, AVenueSnapshotDisagreeingWithTheWarmedBookDiverges) {
  Depth10 wrong = opening_depth();
  wrong[0].ask_sz = 11;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, wrong)};

  const Mbp10DiffReport report = run_diff(testing::opening_snapshot().records(), venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kSnapshotLevels), k1);
  ASSERT_TRUE(report.first_divergence.mismatch);
  EXPECT_STREQ(report.first_divergence.mismatch->field, "ask_sz");
}

TEST(Mbp10Diff, ASnapshotNoBoundaryClaimsIsUnclaimed) {
  StreamBuilder b;
  b.add(100, Side::kBid, px(29000, 0), 10).last();
  const std::vector<MboMsg> records = b.records();

  const Depth10 only = depth({{px(29000, 0), kUndefPrice, 10, 0, 1, 0}});
  const std::vector<Mbp10Msg> venue{venue_snapshot(kSecond, opening_depth()),
                                    book_update(records.back(), only)};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kSnapshotUnclaimed), k1);
  EXPECT_EQ(report.compared, k1);
}

TEST(Mbp10Diff, ASnapshotReplacedBeforeItIsClaimedIsUnclaimed) {
  const std::vector<MboMsg> records = testing::opening_snapshot().records();

  Depth10 stale = opening_depth();
  stale[0].bid_sz = 11;
  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, stale),
                                    venue_snapshot(kFirst, opening_depth())};

  const Mbp10DiffReport report = run_diff(records, venue);
  EXPECT_FALSE(report.ok());
  EXPECT_TRUE(report.reconciles());
  EXPECT_EQ(report.count(DivergenceKind::kSnapshotUnclaimed), k1);
  EXPECT_EQ(report.snapshots, k1);
  ASSERT_EQ(report.divergences.size(), std::size_t{1});
  EXPECT_EQ(report.divergences.front().record_index, k0);
  EXPECT_EQ(report.first_divergence.venue_record_index, k0);
}

TEST(Mbp10Diff, TheFirstDivergenceCarriesItsContext) {
  StreamBuilder b = testing::opening_snapshot();
  b.add(102, Side::kBid, px(29000, 0), 5).last();
  b.add(103, Side::kBid, px(28999, 2), 3).last();
  b.add(104, Side::kBid, px(28999, 1), 2).last();
  b.add(105, Side::kBid, px(28999, 0), 1).last();
  const std::vector<MboMsg> records = b.records();
  ASSERT_GT(records.size(), BookDiff::kContextDepth);

  Depth10 first = opening_depth();
  first[0].bid_sz = 15;
  first[0].bid_ct = 2;
  Depth10 second = first;
  second[2] = BidAskPair{px(28999, 2), kUndefPrice, 3, 0, 1, 0};
  Depth10 third = second;
  third[3] = BidAskPair{px(28999, 1), kUndefPrice, 2, 0, 1, 0};
  Depth10 fourth = third;
  fourth[4] = BidAskPair{px(28999, 0), kUndefPrice, 1, 0, 1, 0};
  Depth10 wrong = fourth;
  wrong[4].bid_ct = 2;

  const std::vector<Mbp10Msg> venue{venue_snapshot(kFirst, opening_depth()),
                                    book_update(records[5], first), book_update(records[6], second),
                                    book_update(records[7], third), book_update(records[8], wrong)};

  const Mbp10DiffReport report = run_diff(records, venue);
  ASSERT_EQ(report.count(DivergenceKind::kLevels), k1);

  const DivergenceContext& ctx = report.first_divergence;
  EXPECT_EQ(ctx.records.size(), BookDiff::kContextDepth);
  EXPECT_EQ(ctx.records.front().order_id, std::uint64_t{100});
  EXPECT_EQ(ctx.records.back().order_id, std::uint64_t{105});
  EXPECT_EQ(ctx.last_record_index, std::uint64_t{8});
  ASSERT_TRUE(ctx.venue_record);
  EXPECT_EQ(ctx.venue_record->sequence, records[8].sequence);
  EXPECT_EQ(ctx.venue_record_index, std::uint64_t{4});
  ASSERT_TRUE(ctx.ours);
  ASSERT_TRUE(ctx.theirs);
  EXPECT_TRUE(depth_eq(*ctx.ours, fourth));
  EXPECT_TRUE(depth_eq(*ctx.theirs, wrong));
  ASSERT_TRUE(ctx.mismatch);
  EXPECT_EQ(ctx.mismatch->level, std::size_t{4});
  EXPECT_STREQ(ctx.mismatch->field, "bid_ct");
}

TEST(Mbp10Diff, TheDivergenceListIsCappedAndTheCountsAreNot) {
  constexpr std::uint64_t kMoves = 20;
  StreamBuilder b;
  for (std::uint64_t i = 0; i < kMoves; ++i) {
    b.add(100 + i, Side::kBid, px(29000, static_cast<std::int64_t>(i)), 10).last();
  }
  const std::vector<MboMsg> records = b.records();
  const std::vector<Mbp10Msg> venue;

  FakeBook book;
  VectorSource source{&venue};
  Mbp10Diff diff{book, source};
  for (std::uint64_t i = 0; i < kMoves; ++i) {
    book.set(kFirst, depth({{px(29000, static_cast<std::int64_t>(i)), kUndefPrice, 10, 0, 1, 0}}));
    diff.after(records[static_cast<std::size_t>(i)]);
  }
  diff.finish();

  const Mbp10DiffReport& report = diff.report();
  EXPECT_EQ(report.divergence_count, kMoves);
  EXPECT_EQ(report.count(DivergenceKind::kMissingRecord), kMoves);
  EXPECT_EQ(report.divergences.size(), std::size_t{16});
  EXPECT_EQ(report.instruments.at(kFirst).divergences, kMoves);
  EXPECT_TRUE(report.boundaries_reconcile());
}

}  // namespace
}  // namespace bookreplay
