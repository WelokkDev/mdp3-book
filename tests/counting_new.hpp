// What the process has allocated through `operator new`, for the binaries that
// measure it. Linking counting_new.cpp replaces allocation for the whole
// binary, which also hides a mismatched new and delete from ASan, so only the
// binaries that need the count link it.

#ifndef BOOKREPLAY_TESTS_COUNTING_NEW_HPP
#define BOOKREPLAY_TESTS_COUNTING_NEW_HPP

#include <cstdint>

namespace bookreplay::testing {

struct Allocations {
  std::uint64_t count = 0;
  std::uint64_t bytes = 0;

  /// The plain and nothrow forms are counted; the over-aligned ones are not
  /// replaced. Nothing on either book's path asks for over-aligned storage;
  /// the decoder's buffer does, and zstd calls malloc.
  [[nodiscard]] static Allocations now() noexcept;

  [[nodiscard]] Allocations operator-(const Allocations& earlier) const noexcept {
    return {count - earlier.count, bytes - earlier.bytes};
  }

  Allocations& operator+=(const Allocations& more) noexcept {
    count += more.count;
    bytes += more.bytes;
    return *this;
  }
};

}  // namespace bookreplay::testing

#endif  // BOOKREPLAY_TESTS_COUNTING_NEW_HPP
