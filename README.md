# bookreplay

CME MDP 3.0 limit order book reconstruction from Databento DBN.

**Work in progress.** The decoder, invariant harness, trades-only replay
driver, order-by-order book and instrument catalog are built and tested, and the
book replays five real trading days with every invariant holding and its top ten
levels matching the venue's own `mbp-10` at every event boundary. A second book,
built for speed, applies a day in just over a quarter of the time and gives
`book_check` and `book_diff` the same output on all five days. Nothing fills
against either.

## Why

Backtesting on bars cannot answer whether a resting limit order filled at a price
the market touched but did not trade through. That depends on queue position, and
a bar has no queue.

Rebuilding a book from market-by-order is easy to do approximately and hard to
know you got right. The check is the venue's own top-ten snapshot: match
`mbp-10` at every event boundary across a real day and the aggregate is no
longer in question. The queue inside it still is.

## Decoder

`DbnReader` reads DBN v3, zstd-framed or raw, for the five schemas this project
needs: `mbo`, `trades`, `mbp-10`, `status`, `definition`. Records are read in
place from a 64-byte-aligned buffer instead of copied out one at a time. zstd is
the only third-party dependency in the shipping library, and the decoder is
written from the published DBN spec rather than wrapped around a vendor library.

v1 and v2 files are refused, not converted. v3 rewrote `InstrumentDefMsg`, so
reading a v2 file with the v3 struct gives plausible garbage instead of an error.
Committed v1 and v2 fixtures prove the refusal fires.

Every record decodes field-for-field identically to Databento's own decoder on
real CME data: a full `mbo` day (2026-08-05, NQ parent, 28,562,350 records,
pulled before the 2026-08-08 renormalization), and the 2026-08-24 to 28 corpus:
103,318,862 `mbp-10` records, 638 `status` and 160 `definition`. All five schemas
also round-trip against Databento's published fixtures. A test-only encoder
generates streams the decoder has never seen: `ts_out` records, records
straddling buffer refills, multi-frame zstd. Two differential
oracles run against independent implementations, both pinned and neither linked
into the library: `databento-dbn` locally, the only one that can see the licensed
corpus, and `databento-cpp` in CI. The suite runs under ASan and UBSan.

Still open: `trades` is checked only against a two-record fixture, since `mbo`
carries the prints this project uses and the schema was never pulled.

## Fills

`Replay` fills orders against trade prints, reporting each fill in order with
exact timestamps. `Book` rebuilds the queue those fills should consult. Nothing
connects them yet, and that gap is the point of the project.

| Order type | Fills when |
|---|---|
| Market | first print at or after it goes live, at that price, one tick worse if the other side aggressed it |
| Resting limit | a print goes strictly through its price, filled at its own price |
| Stop | a print at or through the trigger, filled the same way |
| Stop-limit | elected the same way, then rests at its cap |

Four rules the naive version gets wrong:

- Databento's trade `Side` is the aggressor's, so a print aggressed by the other
  side is the touch we are not crossing, and one tick is the least the book can
  be apart. `ReplayStats::tick_charged_qty` reports how much was charged: a floor
  on the cost of crossing, not on the cost of the order.
- Stops are trade-elected, as CME's are. A quote resting at the trigger does not
  fire one.
- CME refuses a stop whose trigger is not strictly beyond the last trade price at
  order entry, and so does this, judged at arrival. The position then shows as
  unprotected rather than exiting in a way that never happened.
- OCO is quantity-linked, not cancel-on-first-fill. A fill of *q* on any member
  reduces every sibling by *q*, and a member reaching zero cancels its siblings.
  The two rules agree at one lot, which is how the wrong one survives review.

Latency is three explicit parameters with no defaults (decision to live, fill to
protection armed, cancel to removed) and the CLI sweeps a curve across the first
rather than quoting one number. `NakedWindowScan` measures the gap between
an entry filling and its stop reaching the exchange: whether a print reached the
stop first, when, and the worst excursion in ticks.

`Book` rebuilds every instrument order by order: a FIFO queue per price level,
the resting order behind every id, and `queue_ahead`, the quantity a fill has to
consume before it reaches a given order. FIFO is what NQ outrights use
(`match_algorithm` 'F'); a pro-rata product has no single queue position, and the
book does not check which it was handed. The invariant harness was written
against deliberately broken books before the book existed; `Book` runs it
unchanged.

The hard part is what a modify does to priority. A price change or a size
increase queues at the tail, and a size that shrinks or holds keeps its place.
An iceberg refreshing its displayed tranche breaks that, because it looks
identical in aggregate to a shrink and still queues at the tail. The fill in
front of the M separates the two: after an F, the M keeps priority only if its
size is exactly what the fill left behind. A level total is the same either
way, so no aggregate view can falsify that rule and the `mbp-10` diff will not
settle it. Only fill ordering can.

The crossed-book invariant is checked per instrument, and only while the status
schema says that instrument is trading. The venue reports a mid-session halt as
a pre-open carrying a market-event reason rather than as a halt action; NQ did
exactly that for five seconds on 2026-08-25. So `is_trading` decides, not the
action, and `book_check` merges the two schemas on `ts_event`: gating takes
2026-08-26 from 504 crossed boundaries to 0, and all five days from 2026-08-24
to 28 pass with every counter reconciling.

## Depth

`book_diff` replays a day of `mbo` into `Book` and compares its top ten levels
per side against the venue's own `mbp-10` at every event boundary: a price, a
size and an order count per level, sixty values a comparison. The counts are
what make it more than a ladder check, since one order and two orders of the
same total size differ only there.

Across 2026-08-24 to 28, outrights and calendar spreads, inside and outside
trading hours, all 101,474,627 comparisons agree, as do 1,844,195 trade records
and the 40 opening snapshots: every one of the 103,318,862 `mbp-10` records in
the pull. The other 14,769,811 boundaries are checked too. The venue publishes
whenever the top ten moves, so a book that moves while the venue is quiet fails.

The trap is alignment. A record's key is not unique, because one packet can
carry two events for one instrument with identical timestamps, and matching on
it alone invents divergences millions of records in. The record settles whose
it is: its action, price and size are those of the last add, cancel or modify
in the event it closes, on every record in the five days.

What this cannot see is queue order. Force `Book::modify` to always re-queue
and `book_diff` prints byte-identical output on all five days.

## Throughput

On 2026-08-26, 20,131,692 records, one core of an Apple M5 Pro, AppleClang 21,
the `release` preset (`RelWithDebInfo`), median of five repetitions. The book
rows replay records already decoded into memory, so they time the book and not
zstd:

| | ns per record | million records/s | allocations |
|---|---:|---:|---:|
| decode, zstd-framed file | 42.6 | 23.5 | |
| decode, raw file | 12.2 | 82.3 | |
| `Book::apply` | 83.6 | 12.0 | 24,094,584 |
| `FastBook::apply` | 22.9 | 43.7 | 141 |
| `Book`, with `top_ten` at every event boundary | 128.5 | 7.8 | 24,094,584 |
| `FastBook`, with `top_ten` at every event boundary | 50.7 | 19.7 | 141 |

End to end, under `/usr/bin/time -l`, `book_check --status --definition` takes
2.77 s and peaks at 17.4 MB resident with the reference book, 1.50 s and 9.2 MB
with `--book fast`. `book_diff` gains less, 6.5 s to 4.9 s, because decoding the
day's 16.5 million `mbp-10` records and diffing them is most of its time.

The reference spends its time allocating: each level is a `std::deque`, and
opening one costs a tree node and, on libc++, a 4 KB block. The fast book keeps
its orders in a slab, each level an intrusive list threaded through it, so a
level opens and closes without allocating. Order ids go through an
open-addressing table, and levels are addressed by tick in pages of 1,024 with a
bitmap per page, so the touch and the top ten are bit scans.

The fast book allocates only when its storage grows, and counts each time it
does: the day's 141 allocations are exactly those events, 29 of them after the
opening snapshot. A unit test replays a synthetic stream past its warm-up and
fails on any allocation.

## Instruments

`InstrumentCatalog` reads the `definition` schema, which publishes a tick size
per instrument. The NQ parent pull for 2026-08-24 to 28 carries outrights at
0.25 and calendar spreads at 0.05, and delivers each spread as one record per
leg under the spread's own id. The catalog collapses those legs into one
instrument, takes outright or spread from the venue's `instrument_class` rather
than inferring it from the leg count, and hands out each instrument's
`TickScale`. One tick typed by hand cannot serve both: 20 ticks is 5.00 on NQU6
at 0.25 and 1.00 on NQU6-NQZ6 at 0.05.

`book_check --definition` checks every priced record against its own
instrument's increment, and fails the run on one the catalog cannot place at
all. On 2026-08-26 all 19,804,343 priced records sit on their instrument's grid;
re-encode that day's definition file with 0.25 as the spread tick and the same
run fails on 37,327. The check reads one way only: too coarse an increment
fails, while one that divides the true increment leaves every price on grid. All
five days hold 22 instruments, 12 outrights and 10 spreads, none off grid.

## Build

Needs a C++20 compiler, CMake ≥ 3.25 and Ninja. GoogleTest and zstd are fetched
at configure time, zstd pinned by tarball hash rather than by tag, since a tag
can move. `-DBOOKREPLAY_USE_SYSTEM_ZSTD=ON` links an installed one.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`book_check` and `book_diff` take `--book fast`, which needs `--definition` for
its ticks. On a day whose every price is on its grid it prints the same bytes as
the default reference run; where the reference counts off-grid prices, the fast
book stops at the first.

The benchmarks are off by default, since they fetch Google Benchmark, pinned by
tarball hash like zstd. Given no file they replay a synthetic stream, which is
what CI runs; those numbers describe the generator, not a trading day.

```sh
cmake --preset release -DBOOKREPLAY_BUILD_BENCHMARKS=ON
cmake --build --preset release
build/release/benchmarks/bookreplay_benchmarks \
    --mbo DAY.mbo.dbn.zst --definition DAY.definition.dbn.zst \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

The Python extension is off by default, since it needs a Python development
environment the library does not. It binds order entry and cancellation,
`advance_to` and its fills, the order and statistics views, `load_ticks` and
`naked_window_scan`. `InstrumentCatalog` is not bound.

```sh
pip install pytest
cmake -S . -B build -G Ninja -DBOOKREPLAY_BUILD_PYTHON=ON
cmake --build build
ctest --test-dir build -L python
```

CI builds GCC and Clang on Linux, AppleClang on macOS and MSVC on Windows, all
with warnings as errors, runs the suite under ASan and UBSan, builds the Python
module on Linux and Windows, runs both oracles, and runs every benchmark for one
iteration on the synthetic stream.

## Corpus

Every number here was measured on one pull, which a Databento account
reproduces:

| | |
|---|---|
| Dataset | `GLBX.MDP3` |
| Schemas | `mbo`, `mbp-10`, `definition`, `status` |
| Symbology | parent, `NQ.FUT`: every outright and every calendar spread in one pull |
| Window | 2026-08-24 to 28, from `00:00 UTC`; a book can only be rebuilt from a snapshot boundary |
| Delivery | batch download |

Databento renormalized GLBX.MDP3 on 2026-08-08, retroactively across all
history, so files pulled before and after that date differ semantically and must
not be mixed; record the pull date in the path.

The `.dbn` files under `tests/data` are Databento's own test vectors, Apache-2.0
and vendored unmodified so the decoder tests run offline; see
`tests/data/NOTICE`. No licensed window is committed.

## Licence

MIT; see `LICENSE`. The vendored fixtures in `tests/data` stay under Databento's
Apache-2.0 grant; see `tests/data/NOTICE`.
