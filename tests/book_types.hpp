// The two books the typed book tests run against, and what the fast one needs
// before it can take an order.

#ifndef BOOKREPLAY_TESTS_BOOK_TYPES_HPP
#define BOOKREPLAY_TESTS_BOOK_TYPES_HPP

#include "bookreplay/book.hpp"
#include "bookreplay/fast_book.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

#include "toy_stream.hpp"

namespace bookreplay::testing {

/// Every instrument the hand-built streams use. All of them take NQ's outright
/// tick, because `px()` counts in quarter points whatever the instrument.
inline constexpr std::array<std::uint32_t, 3> kTestInstruments{42004177, 42019315, 42013467};

template <typename B>
[[nodiscard]] B make_book() {
  B book;
  if constexpr (std::is_same_v<B, FastBook>) {
    for (const std::uint32_t instrument_id : kTestInstruments) {
      book.set_tick_size(instrument_id, kTick);
    }
  }
  return book;
}

using BookTypes = ::testing::Types<Book, FastBook>;

struct BookTypeNames {
  template <typename B>
  static std::string GetName(int) {
    return std::is_same_v<B, Book> ? "Reference" : "Fast";
  }
};

template <typename B>
class BookTest : public ::testing::Test {};

}  // namespace bookreplay::testing

/// Declares a typed suite that runs over both books. gtest wants a fixture
/// template per suite name, and these suites differ in nothing else.
#define BOOKREPLAY_BOOK_SUITE(Suite)                \
  template <typename B>                             \
  using Suite = ::bookreplay::testing::BookTest<B>; \
  TYPED_TEST_SUITE(Suite, ::bookreplay::testing::BookTypes, ::bookreplay::testing::BookTypeNames)

#endif  // BOOKREPLAY_TESTS_BOOK_TYPES_HPP
