// Its own binary, because counting allocations means replacing operator new
// for every test that shares the process.

#include "bookreplay/dbn.hpp"
#include "bookreplay/fast_book.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "book_types.hpp"
#include "counting_new.hpp"
#include "synthetic_stream.hpp"
#include "toy_stream.hpp"

namespace bookreplay {
namespace {

using testing::Allocations;
using testing::make_book;
using testing::px;
using testing::StreamBuilder;

std::uint64_t allocations_applying(FastBook& book, std::span<const MboMsg> records) {
  const Allocations before = Allocations::now();
  for (const MboMsg& rec : records) {
    book.apply(rec);
  }
  return (Allocations::now() - before).count;
}

// Any allocation at all fails this, including one a later change makes for a
// good reason. That is deliberate: the claim is zero, and a test that allowed
// a few would stop checking it the first time a few crept in.
TEST(FastBookAllocation, NothingIsAllocatedOnceTheBookHasHeldItsMost) {
  const std::vector<MboMsg> records = testing::synthetic_stream(200'000);
  const std::span<const MboMsg> all{records};
  const std::span<const MboMsg> warm_up = all.first(all.size() / 2);

  FastBook book = make_book<FastBook>();
  (void)allocations_applying(book, warm_up);
  const FastBook::Growth grown = book.growth();
  ASSERT_GT(grown.slab, 0U) << "the warm-up never grew the slab, so it proves nothing";
  ASSERT_GT(book.order_count(), testing::kSyntheticMaxResting / 2);

  EXPECT_EQ(allocations_applying(book, all.subspan(warm_up.size())), 0U);
  EXPECT_EQ(book.growth().total(), grown.total());
  EXPECT_NO_THROW(book.verify());
}

// Microsoft's debug standard library gives every container a heap-allocated
// proxy for its checked iterators to hang from. Doubling the id table builds
// one vector and move-constructs another, so there it costs two allocations
// beyond the buckets; no other growth constructs a container.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr std::uint64_t kProxiesPerIdTableGrowth = 2;
#else
constexpr std::uint64_t kProxiesPerIdTableGrowth = 0;
#endif

// Enough of each thing to outgrow its starting size: orders for the slab and
// the id table, far-apart prices for the pages and both lists of them, and
// fills inside one event for the attribution. A growth that allocated without
// being counted would leave the two totals apart.
TEST(FastBookAllocation, EachGrowthIsOneAllocationAndIsCounted) {
  StreamBuilder s;
  for (std::uint64_t id = 1; id <= 1500; ++id) {
    s.add(id, Side::kBid, px(29000, -static_cast<std::int64_t>(id % 7)), 1).last();
  }
  for (std::int64_t page = 1; page <= 100; ++page) {
    s.add(2000 + static_cast<std::uint64_t>(page), Side::kAsk, px(29000, 2000 * page), 1).last();
  }
  for (std::uint64_t id = 1; id <= 100; ++id) {
    s.fill(id, Side::kBid, px(29000, -static_cast<std::int64_t>(id % 7)), 1);
  }
  s.last();
  FastBook book = make_book<FastBook>();
  const std::uint64_t allocated = allocations_applying(book, s.records());

  const FastBook::Growth& growth = book.growth();
  EXPECT_EQ(growth.slab, 1U);
  EXPECT_EQ(growth.id_table, 1U);
  EXPECT_EQ(growth.pages, 101U);
  EXPECT_GT(growth.page_directory, 1U);
  EXPECT_GT(growth.attribution, 0U);
  EXPECT_EQ(allocated, growth.total() + kProxiesPerIdTableGrowth * growth.id_table);
  EXPECT_EQ(book.order_count(), 1600U);
}

TEST(FastBookAllocation, ABookRefilledAfterAClearAllocatesNothing) {
  StreamBuilder s;
  for (std::uint64_t id = 1; id <= 50; ++id) {
    const auto away = static_cast<std::int64_t>(id % 10);
    s.add(id, Side::kBid, px(29000, -away), 2);
    s.add(1000 + id, Side::kAsk, px(29000, 1 + away), 2);
  }
  s.last();
  StreamBuilder cleared;
  cleared.clear();

  FastBook book = make_book<FastBook>();
  (void)allocations_applying(book, s.records());
  (void)allocations_applying(book, cleared.records());

  EXPECT_EQ(allocations_applying(book, s.records()), 0U);
  EXPECT_EQ(book.order_count(), 100U);
}

}  // namespace
}  // namespace bookreplay
