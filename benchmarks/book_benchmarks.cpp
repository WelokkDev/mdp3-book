// Throughput of the decoder and of both books.
//
// Every book case replays records already decoded into memory. From a file,
// zstd alone costs more per record than the fast book does, so preloading is
// what leaves the book as the only thing the clock sees. The decoder has cases
// of its own, file to records, so its floor sits in the same table. Whichever
// encoding the input is not in is written to a temporary file for them and
// removed on exit.
//
//   bookreplay_benchmarks [--mbo FILE.dbn[.zst] --definition FILE.dbn[.zst]]
//                         [--benchmark_* flags]
//
// With no file it replays a synthetic stream, so the binary runs, and keeps
// compiling, without the licensed corpus. Those numbers describe the
// generator, not a trading day.

#include "bookreplay/book.hpp"
#include "bookreplay/dbn.hpp"
#include "bookreplay/dbn_reader.hpp"
#include "bookreplay/definition.hpp"
#include "bookreplay/depth.hpp"
#include "bookreplay/fast_book.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "counting_new.hpp"
#include "dbn_encoder.hpp"
#include "synthetic_stream.hpp"
#include "toy_stream.hpp"

namespace {

using namespace bookreplay;
using testing::Allocations;

using TickSizes = std::vector<std::pair<std::uint32_t, std::int64_t>>;

constexpr std::size_t kSyntheticEvents = 1'000'000;

/// A file written for the decode cases to read back.
class TempFile {
 public:
  TempFile(const std::vector<std::byte>& bytes, const char* extension)
      : path_(std::filesystem::temp_directory_path() /
              ("bookreplay_benchmarks_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               extension)) {
    std::ofstream out{path_, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      throw BookreplayError("cannot write " + path_.string());
    }
  }

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

struct Input {
  std::string source;
  std::vector<MboMsg> records;
  /// Where the opening snapshot ends and the steady state begins.
  std::size_t snapshot_end = 0;
  TickSizes ticks;
  std::filesystem::path zstd_path;
  std::filesystem::path raw_path;
  std::vector<std::unique_ptr<TempFile>> written;
};

[[nodiscard]] bool is_zstd(const std::filesystem::path& path) {
  std::array<char, 4> magic{};
  std::ifstream in{path, std::ios::binary};
  in.read(magic.data(), magic.size());
  return in && magic == std::array<char, 4>{'\x28', '\xB5', '\x2F', '\xFD'};
}

/// The same records whichever encoding a case reads. A file written here
/// carries none of Databento's symbology metadata, which a reader parses once
/// and so costs nothing a record.
std::vector<std::byte> encode(const std::vector<MboMsg>& records, const std::string& dataset) {
  testing::DbnEncoder encoder;
  encoder.dataset(dataset).schema(kSchemaMbo);
  for (const MboMsg& rec : records) {
    encoder.add(rec);
  }
  return encoder.encode();
}

std::filesystem::path write(Input& in, const std::vector<std::byte>& bytes, const char* extension) {
  in.written.push_back(std::make_unique<TempFile>(bytes, extension));
  return in.written.back()->path();
}

void find_snapshot_end(Input& in) {
  while (in.snapshot_end < in.records.size() &&
         has_flag(in.records[in.snapshot_end], kFlagSnapshot)) {
    ++in.snapshot_end;
  }
}

Input file_input(const std::string& mbo_path, const std::string& definition_path) {
  Input in;
  in.source = std::filesystem::path{mbo_path}.filename().string();
  DbnReader reader{std::filesystem::path{mbo_path}};
  while (const RecordHeader* hd = reader.next()) {
    const MboMsg* rec = record_cast<MboMsg>(*hd);
    if (rec == nullptr) {
      throw BookreplayError(mbo_path + " carries a record that is not MBO");
    }
    in.records.push_back(*rec);
  }
  const std::vector<std::byte> raw = encode(in.records, reader.metadata().dataset);
  if (is_zstd(mbo_path)) {
    in.zstd_path = mbo_path;
    in.raw_path = write(in, raw, ".dbn");
  } else {
    in.raw_path = mbo_path;
    in.zstd_path = write(in, testing::DbnEncoder::compress(raw), ".dbn.zst");
  }
  in.ticks = read_definitions(std::filesystem::path{definition_path}).tick_sizes();
  find_snapshot_end(in);
  return in;
}

const TickSizes& synthetic_ticks() {
  static const TickSizes ticks{{testing::StreamBuilder{}.instrument_id(), testing::kTick}};
  return ticks;
}

Input synthetic_input() {
  Input in;
  in.source = "synthetic, " + std::to_string(kSyntheticEvents) + " events";
  in.records = testing::synthetic_stream(kSyntheticEvents);
  in.ticks = synthetic_ticks();
  const std::vector<std::byte> raw = encode(in.records, "SYNTHETIC");
  in.raw_path = write(in, raw, ".dbn");
  in.zstd_path = write(in, testing::DbnEncoder::compress(raw), ".dbn.zst");
  find_snapshot_end(in);
  return in;
}

template <typename B>
B make_book(const TickSizes& ticks);

template <>
Book make_book<Book>(const TickSizes&) {
  return Book{};
}

template <>
FastBook make_book<FastBook>(const TickSizes& ticks) {
  FastBook book;
  for (const auto& [instrument_id, tick_size] : ticks) {
    book.set_tick_size(instrument_id, tick_size);
  }
  return book;
}

void set_counters(benchmark::State& state, std::size_t records_per_iteration,
                  const Allocations& allocations) {
  const double records =
      static_cast<double>(records_per_iteration) * static_cast<double>(state.iterations());
  state.counters["time/record"] =
      benchmark::Counter(records, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["records/s"] = benchmark::Counter(records, benchmark::Counter::kIsRate);
  state.counters["allocs"] = benchmark::Counter(static_cast<double>(allocations.count),
                                                benchmark::Counter::kAvgIterations);
  state.counters["allocs/record"] = static_cast<double>(allocations.count) / records;
  state.counters["bytes/record"] = static_cast<double>(allocations.bytes) / records;
}

void decode(benchmark::State& state, const Input* in, const std::filesystem::path* path) {
  Allocations allocations;
  for (auto _ : state) {
    const Allocations start = Allocations::now();
    DbnReader reader{*path};
    while (const RecordHeader* hd = reader.next()) {
      benchmark::DoNotOptimize(hd);
    }
    allocations += Allocations::now() - start;
  }
  set_counters(state, in->records.size(), allocations);
}

template <typename B, bool kTopTen>
void replay(B& book, const MboMsg* first, const MboMsg* last) {
  for (const MboMsg* rec = first; rec != last; ++rec) {
    book.apply(*rec);
    if constexpr (kTopTen) {
      if (is_event_boundary(*rec)) {
        Depth10 top = top_ten(book, rec->hd.instrument_id);
        benchmark::DoNotOptimize(top);
      }
    }
  }
}

/// The whole input per iteration, from an empty book: a stretch of it would
/// time the snapshot and the quiet pre-open and call that the day.
template <typename B, bool kTopTen>
void replay_input(benchmark::State& state, const Input* in) {
  const MboMsg* first = in->records.data();
  const MboMsg* steady = first + in->snapshot_end;
  const MboMsg* last = first + in->records.size();
  std::optional<B> book;
  Allocations allocations;
  Allocations after_snapshot;
  for (auto _ : state) {
    state.PauseTiming();
    book.reset();
    book.emplace(make_book<B>(in->ticks));
    const Allocations start = Allocations::now();
    state.ResumeTiming();
    replay<B, kTopTen>(*book, first, steady);
    const Allocations warm = Allocations::now();
    replay<B, kTopTen>(*book, steady, last);
    const Allocations end = Allocations::now();
    allocations += end - start;
    after_snapshot += end - warm;
  }
  set_counters(state, in->records.size(), allocations);
  state.counters["steady_allocs"] = benchmark::Counter(static_cast<double>(after_snapshot.count),
                                                       benchmark::Counter::kAvgIterations);
}

// A ladder `depth` levels deep on each side, three orders a level, with a
// one-tick gap above the best bid for a probe to open a new touch in.
template <typename B>
B ladder_book(std::size_t depth) {
  B book = make_book<B>(synthetic_ticks());
  testing::StreamBuilder s;
  std::uint64_t id = 1;
  for (std::size_t level = 0; level < depth; ++level) {
    const auto ticks = static_cast<std::int64_t>(level);
    for (int order = 0; order < 3; ++order) {
      s.add(id++, Side::kBid, testing::px(29000, -ticks), 5);
      s.add(id++, Side::kAsk, testing::px(29000, 2 + ticks), 5);
    }
  }
  if (!s.records().empty()) {
    s.last();
  }
  for (const MboMsg& rec : s.records()) {
    book.apply(rec);
  }
  return book;
}

constexpr std::uint64_t kProbe = 1'000'000'000;

template <typename B>
void cycle(benchmark::State& state, B& book, const std::vector<MboMsg>& records) {
  const Allocations start = Allocations::now();
  for (auto _ : state) {
    for (const MboMsg& rec : records) {
      book.apply(rec);
    }
  }
  set_counters(state, records.size(), Allocations::now() - start);
}

/// An add that opens a level at the touch and the cancel that closes it: the
/// reference allocates and frees a deque block and rebalances its tree on
/// each; the fast book flips a bit and walks the touch one level.
template <typename B>
void open_and_close_level(benchmark::State& state) {
  B book = ladder_book<B>(static_cast<std::size_t>(state.range(0)));
  testing::StreamBuilder s;
  s.add(kProbe, Side::kBid, testing::px(29000, 1), 5).last();
  s.cancel(kProbe, Side::kBid, testing::px(29000, 1), 5).last();
  cycle(state, book, s.records());
}

/// A modify that moves one of the best bid's orders to the tail of the next
/// level down and back again, both levels staying open: the re-queue alone.
template <typename B>
void requeue(benchmark::State& state) {
  B book = ladder_book<B>(static_cast<std::size_t>(state.range(0)));
  testing::StreamBuilder s;
  s.modify(1, Side::kBid, testing::px(29000, -1), 5).last();
  s.modify(1, Side::kBid, testing::px(29000, 0), 5).last();
  cycle(state, book, s.records());
}

std::string growth_text(const FastBook::Growth& growth) {
  return "slab " + std::to_string(growth.slab) + ", id_table " + std::to_string(growth.id_table) +
         ", pages " + std::to_string(growth.pages) + ", page_directory " +
         std::to_string(growth.page_directory) + ", attribution " +
         std::to_string(growth.attribution);
}

FastBook::Growth operator-(const FastBook::Growth& later, const FastBook::Growth& earlier) {
  return {later.slab - earlier.slab, later.id_table - earlier.id_table, later.pages - earlier.pages,
          later.page_directory - earlier.page_directory, later.attribution - earlier.attribution};
}

/// Lists every growth event one replay of the input causes, and sets the
/// allocations after the snapshot beside them: each event is one allocation,
/// so the two agree or something else is allocating.
void describe_growth(const Input& in) {
  FastBook book = make_book<FastBook>(in.ticks);
  const MboMsg* first = in.records.data();
  replay<FastBook, false>(book, first, first + in.snapshot_end);
  const FastBook::Growth warm = book.growth();
  const Allocations start = Allocations::now();
  replay<FastBook, false>(book, first + in.snapshot_end, first + in.records.size());
  const Allocations steady = Allocations::now() - start;
  benchmark::AddCustomContext("fast_book_growth", growth_text(book.growth()));
  benchmark::AddCustomContext("fast_book_growth_after_snapshot", growth_text(book.growth() - warm));
  benchmark::AddCustomContext("fast_book_allocations_after_snapshot", std::to_string(steady.count));
}

void register_benchmarks(const Input& in) {
  const auto day = [](auto* b) { b->Unit(benchmark::kMillisecond)->UseRealTime(); };
  day(benchmark::RegisterBenchmark("decode/zstd", decode, &in, &in.zstd_path));
  day(benchmark::RegisterBenchmark("decode/raw", decode, &in, &in.raw_path));
  day(benchmark::RegisterBenchmark("apply/reference", replay_input<Book, false>, &in));
  day(benchmark::RegisterBenchmark("apply/fast", replay_input<FastBook, false>, &in));
  day(benchmark::RegisterBenchmark("apply_top_ten/reference", replay_input<Book, true>, &in));
  day(benchmark::RegisterBenchmark("apply_top_ten/fast", replay_input<FastBook, true>, &in));
  const auto sized = [](auto* b) { b->Arg(10)->Arg(100)->Arg(1000); };
  sized(benchmark::RegisterBenchmark("open_close_level/reference", open_and_close_level<Book>));
  sized(benchmark::RegisterBenchmark("open_close_level/fast", open_and_close_level<FastBook>));
  sized(benchmark::RegisterBenchmark("requeue/reference", requeue<Book>));
  sized(benchmark::RegisterBenchmark("requeue/fast", requeue<FastBook>));
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  std::string mbo_path;
  std::string definition_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mbo" && i + 1 < argc) {
      mbo_path = argv[++i];
    } else if (arg == "--definition" && i + 1 < argc) {
      definition_path = argv[++i];
    } else {
      std::fprintf(stderr, "bookreplay_benchmarks: unrecognized argument %s\n", argv[i]);
      return 2;
    }
  }
  if (mbo_path.empty() != definition_path.empty()) {
    std::fputs(
        "bookreplay_benchmarks: --mbo and --definition go together; the fast book needs every "
        "instrument's tick\n",
        stderr);
    return 2;
  }

  try {
    const Input input =
        mbo_path.empty() ? synthetic_input() : file_input(mbo_path, definition_path);
    benchmark::AddCustomContext("input", input.source);
    benchmark::AddCustomContext("records", std::to_string(input.records.size()));
    benchmark::AddCustomContext("snapshot_records", std::to_string(input.snapshot_end));
    describe_growth(input);
    register_benchmarks(input);
    benchmark::RunSpecifiedBenchmarks();
  } catch (const BookreplayError& e) {
    std::fprintf(stderr, "bookreplay_benchmarks: %s\n", e.what());
    return 1;
  }
  benchmark::Shutdown();
  return 0;
}
