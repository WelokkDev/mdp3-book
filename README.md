# bookreplay

CME MDP 3.0 limit order book reconstruction from Databento DBN, checked against
an independent vendor snapshot.

**Work in progress.** The decoder, the invariant harness and a trades-only
replay driver are built and tested. The book itself does not exist yet.

## Why

Backtesting on bars cannot answer whether a resting limit order filled at a price
the market touched but did not trade through. That depends on queue position, and
a bar has no queue.

Rebuilding a book from market-by-order is easy to do approximately and hard to
know you got right. The check is the venue's own top-ten snapshot: if the rebuilt
book matches `mbp-10` at every event boundary across a real trading day, there is
nothing left to argue about.

## Decoder

`DbnReader` reads DBN v3, zstd-framed or raw, for the five schemas this project
needs: `mbo`, `trades`, `mbp-10`, `status`, `definition`. Records are read in
place from a 64-byte-aligned buffer instead of copied out one at a time. zstd is
the only third-party dependency in the shipping library, and the decoder is
written from the published DBN spec rather than wrapped around a vendor library.

v1 and v2 files are refused, not converted. v3 rewrote `InstrumentDefMsg`, so
reading a v2 file with the v3 struct gives plausible garbage instead of an error.
Committed v1 and v2 fixtures prove the refusal fires.

Checked against a full GLBX.MDP3 `mbo` day (2026-08-05, NQ parent, 28,562,350
records): every record decodes field-for-field identically to Databento's own
decoder. All five schemas round-trip against Databento's published fixtures. A
test-only encoder generates streams the decoder has never seen, including
`ts_out`-extended records, records straddling buffer refills and multi-frame
zstd. Two differential oracles run against independent implementations, both
pinned and neither linked into the library: `databento-dbn` locally, which is the
only one that can see the licensed corpus, and `databento-cpp` in CI. The suite
runs under ASan and UBSan.

Still open: only `mbo` has been checked against a real CME pull. The other four
schemas are checked against fixtures of two to four records each.

## Replay, without a book

`Replay` takes orders with a side, type, tick level, quantity, an instant they go
live and an optional OCO group, replays the market between two points, and
reports every fill in order with exact timestamps. Trade prints drive it. There
is no book and no queue model, which bounds what it can answer.

| Order type | Fills when |
|---|---|
| Market | first print at or after it goes live, at that price, one tick worse if the other side aggressed it |
| Resting limit | a print goes strictly through its price, filled at its own price |
| Stop | a print at or through the trigger, filled the same way |
| Stop-limit | elected the same way, then rests at its cap |

Four rules are worth calling out, since the naive version gets each one wrong:

- Databento's trade `Side` is the aggressor's, so a print aggressed by the other
  side is the touch we are not crossing, and one tick is the least the book can
  be apart. `ReplayStats::tick_charged_qty` reports how much of that was charged.
  It is a floor on the cost of crossing, not on the cost of the order; a large
  order still fills print by print, and that residual needs the book.
- Stops are trade-elected, as CME's are. A quote resting at the trigger does not
  fire one.
- CME refuses a stop whose trigger is not strictly beyond the last trade price at
  order entry, and so does this, judged at arrival. The position it was meant to
  protect then shows as unprotected for the rest of the run instead of exiting in
  an orderly way that never happened.
- OCO is quantity-linked, not cancel-on-first-fill. A fill of *q* on any member
  reduces every sibling by *q*, and a member reaching zero cancels its siblings.
  The two rules agree at one lot, which is how the wrong one survives review.

Latency is three explicit parameters with no defaults (decision to live, fill to
protection armed, cancel to removed) and the CLI sweeps a curve across them
instead of quoting one number. `NakedWindowScan` measures the window between an
entry filling and its stop reaching the exchange: whether a print reached the
stop first, when, and the worst excursion in ticks.

Book invariants are already written and tested against deliberately broken books
in `tests/toy_book.hpp`, ahead of the book itself.

## Build

Needs a C++20 compiler, CMake ≥ 3.25 and Ninja. GoogleTest and zstd are fetched
at configure time, zstd pinned by tarball hash rather than by tag, since a tag
can move. `-DBOOKREPLAY_USE_SYSTEM_ZSTD=ON` links an installed one.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The Python extension is off by default, since it needs a Python development
environment the library does not. It exposes `submit`, `cancel`, `advance_to`
and fills, and nothing else.

```sh
pip install pytest
cmake -S . -B build -G Ninja -DBOOKREPLAY_BUILD_PYTHON=ON
cmake --build build
ctest --test-dir build -L python
```

CI builds GCC, Clang and MSVC with warnings as errors, runs the suite under ASan
and UBSan, builds the Python module on Linux and Windows, and runs both oracles.

## Market data is not in this repository

No `.dbn`, `.dbn.zst` or derived market data file is ever committed; a public
repo may not redistribute a licensed CME window. Reproducing the validation needs
your own Databento account, so the corpus is specified instead of shipped:

| | |
|---|---|
| Dataset | `GLBX.MDP3` |
| Schemas | `mbo`, `mbp-10`, `definition`, `status` |
| Symbology | continuous, `MNQ.v.0`, not `.c.0`, which returns the wrong contract during roll weeks |
| Window | always from `00:00 UTC`; a book can only be rebuilt from a snapshot boundary |
| Delivery | batch download |

Two things to watch. Databento renormalized GLBX.MDP3 on 2026-08-08,
retroactively across all history, so files pulled before and after that date
differ semantically and must not be mixed; record the pull date in the path. And
billing is on *uncompressed* bytes: MBO is 56 B per record, so cost is the record
count times 56 B no matter how small the `.zst` turns out to be.

Once the book exists, verification will come from SHA-256 digests of book state
at every event boundary, so anyone holding the same pull can check the
reconstruction without the data itself being shared.

`tests/data` is the one apparent exception: about 5 KB of Databento's own test
vectors, Apache-2.0 and vendored unmodified so the decoder tests are hermetic.
Some carry real bytes, so the right to redistribute rests on Databento's licence
grant, not on the content being synthetic. Provenance and SHA-256 digests are in
`tests/data`.

## Licence

Not yet chosen.
