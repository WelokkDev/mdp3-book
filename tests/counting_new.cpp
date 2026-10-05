#include "counting_new.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::uint64_t> g_count{0};
std::atomic<std::uint64_t> g_bytes{0};

}  // namespace

void* operator new(std::size_t size) {
  g_count.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(size, std::memory_order_relaxed);
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* p) noexcept {
  std::free(p);
}

void operator delete[](void* p) noexcept {
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}

namespace bookreplay::testing {

Allocations Allocations::now() noexcept {
  return {g_count.load(std::memory_order_relaxed), g_bytes.load(std::memory_order_relaxed)};
}

}  // namespace bookreplay::testing
