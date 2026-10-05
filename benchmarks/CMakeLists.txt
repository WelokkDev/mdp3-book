include(FetchContent)

# Pinned by tarball hash, as zstd is: a tag can be moved and a hash cannot.
FetchContent_Declare(benchmark
  URL      https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
  URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
  SYSTEM                     # keep our -Werror off Google Benchmark's headers
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# Its own -Werror, applied to its own sources, fails on compilers newer than it.
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# The synthetic stream, the encoder and the allocation counter are the tests'
# own; the benchmarks reuse them rather than keep a second copy.
add_executable(bookreplay_benchmarks
  book_benchmarks.cpp
  ${PROJECT_SOURCE_DIR}/tests/counting_new.cpp
)
target_include_directories(bookreplay_benchmarks PRIVATE ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(bookreplay_benchmarks PRIVATE
  bookreplay::bookreplay bookreplay::zstd benchmark::benchmark
)
bookreplay_set_warnings(bookreplay_benchmarks)
