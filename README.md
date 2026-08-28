# bookreplay

CME MDP 3.0 limit order book reconstruction from Databento DBN, checked against
an independent vendor snapshot.

**Status:** the decoder, the invariant harness, and a trades-only replay driver
work. The book itself does not exist yet. The decoder has a validation number,
below; the book will not get one here until it earns it.

## What it will be

A C++20 library that decodes Databento DBN for CME Globex, rebuilds the full
limit order book from the market-by-order (`mbo`) feed, and simulates one
participant's resting orders under price-time FIFO with an explicit latency
model. Success is one number: whether the reconstructed book matches
Databento's own `mbp-10` on every top-10 level at every event boundary, across
a named corpus.

zstd is the only third-party dependency in the shipping library, and the
decoder is written from the published DBN spec, not wrapped around a vendor
library.

## The decoder

`DbnReader` reads DBN v3 — zstd-framed or raw — for the five schemas this
project needs: `mbo`, `trades`, `mbp-10`, `status`, `definition`. Records are
read in place from a 64-byte-aligned buffer instead of copied out one at a
time. The casts are well defined under C++20 implicit object creation
(P0593R6), and the reader rejects any record length that would break the
alignment it relies on.

v1 and v2 files are refused, not converted. v3 rewrote `InstrumentDefMsg`:
`asset` widened from 7 bytes to 11, `raw_instrument_id` from 32 to 64 bits, the
`leg_*` fields arrived, and three fields were dropped. Reading a v2 file with
the v3 struct gives plausible garbage instead of an error, so the committed v1
and v2 fixtures exist to prove the refusal fires.

### Checked against

- **A real day.** All 28,562,350 records of a GLBX.MDP3 `mbo` day (2026-08-05,
  NQ parent, 527 MB compressed) decode field-for-field identically to
  Databento's decoder. Every record, not a sample.
- **Every schema.** All five round-trip against Databento's published fixtures,
  including the 520-byte v3 definition record and all 69 of its fields.
- **Synthetic streams.** A test-only encoder generates streams the decoder has
  never seen — `ts_out`-extended records, records straddling buffer refills,
  multi-frame zstd — and asserts bit-exact round trips.

Two differential oracles run against independent implementations, both pinned,
neither linked into the library: `databento-dbn` locally, field by field, the
only one that can see the licensed corpus; and `databento-cpp` in CI, byte for
byte over the fixtures. Malformed input raises `DbnError` naming what was
found, and the suite runs under ASan and UBSan.

Still open: only `mbo` has been checked against a real CME pull. The other four
schemas are checked against fixtures of two to four records each.

## Replay without a book

`Replay` answers one question: given orders with a side, type, tick level,
quantity, an instant they go live, and an optional OCO group, replay the market
from A to B and report every fill in order with exact timestamps. It knows
nothing about brackets, risk, targets, strategies or bars.

Trade prints drive it. There is no book and no queue model, which bounds what
it can answer:

| Order type | Fills when |
|---|---|
| Market | the first print at or after it goes live, at that print's price |
| Resting limit | a print goes strictly through its price — filled at its own price, never the better one |
| Stop | a print is at or through the trigger, filled at that print's price |
| Stop-limit | elected the same way, then rests at its cap; it cannot fill on the electing print |

Stops are trade-elected because CME stops are: a quote resting at the trigger
does not fire one. A print with no aggressor still elects a stop, but does not
fill a resting limit, since it cannot be attributed to a side. Whether a limit
that was merely *touched* would have filled needs queue position, so it stays
unanswered until the book exists.

OCO is quantity-linked, not cancel-on-first-fill: a fill of *q* on any member
reduces every sibling by *q*, and a member reaching zero cancels its siblings.
The two rules agree at one lot, which is how the wrong one survives review.

Latency is three explicit parameters with no defaults — decision to live, fill
to protection armed, cancel to removed — and the CLI sweeps a curve across them
instead of quoting one number. `NakedWindowScan` covers the gap between an
entry filling and its protection reaching the venue: whether a print reached
the stop, when, and the worst excursion in ticks. `advance_to` is monotonic and
order lookup uses sorted vectors, so no result depends on hash iteration order.

Checked like the decoder. `tests/toy_replay.hpp` holds drivers each broken in
one real way, and the tests assert which invariant catches which. On a real
`mbo` day the front-month outright yields 415,579 prints, and Debug and
RelWithDebInfo produce byte-identical fill lists from the same order script.

## Why the invariants came first

NautilusTrader — 25k stars, 288 KB of tests, real GLBX golden fixtures — mapped
`action='F'` to a book update and shipped it. The result was a book crossed by
~220 points on a full CME day: best bid 29,909.75 against best ask 29,689.00,
with market buys filling at phantom asks hundreds of points away. 418,061 Fill
records became deltas. An outside user found it, because nothing in the test
suite could fail.

Three checks would have caught it, all about ten lines:

1. Every `A`, `C`, `M`, `R` counts as exactly one mutation and every `T`, `F`,
   `N` as zero. Checked per record, so two miscounts cannot cancel out.
2. `best_bid < best_ask` at event boundaries, gated on `Trading` state.
3. No `T` or `F` may change order membership: neither inventing an order id
   that was never added, which is the bug above by cause instead of symptom,
   nor erasing the resting order it names.

They live in `include/bookreplay/invariants.hpp` and were written before a
single price level existed. `tests/toy_book.hpp` holds broken books that
reproduce the bug, so the tests prove the invariants fire.

## Build

Needs a C++20 compiler, CMake ≥ 3.25 and Ninja. GoogleTest and zstd are fetched
at configure time, zstd pinned by tarball hash instead of by tag, since a tag
can move. `-DBOOKREPLAY_USE_SYSTEM_ZSTD=ON` links an installed one.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The Python extension is off by default, since it needs a Python development
environment the library does not. It exposes the contract above and nothing
else: `submit`, `cancel`, `advance_to`, fills.

```sh
pip install pytest
cmake -S . -B build -G Ninja -DBOOKREPLAY_BUILD_PYTHON=ON
cmake --build build
ctest --test-dir build -L python
```

CI builds GCC, Clang and MSVC with warnings as errors, runs the suite under
ASan and UBSan, builds the Python module on Linux and Windows, and runs both
oracles.

## Market data is not in this repository

No `.dbn`, `.dbn.zst` or derived market data file is ever committed; a public
repo may not redistribute a licensed CME window. Reproducing the validation
needs your own Databento account, so the corpus is specified instead of
shipped:

| | |
|---|---|
| Dataset | `GLBX.MDP3` |
| Schemas | `mbo`, `mbp-10`, `definition`, `status` |
| Symbology | continuous, `MNQ.v.0` — not `.c.0`, which returns the wrong contract during roll weeks |
| Window | always from `00:00 UTC`; a book can only be rebuilt from a snapshot boundary |
| Delivery | batch download |

Two things to watch. Databento renormalized GLBX.MDP3 on 2026-08-08,
retroactively across all history, so files pulled before and after that date
differ semantically and must not be mixed; record the pull date in the path.
And billing is on *uncompressed* bytes: MBO is 56 B per record, so cost is
`record_count × 56 B` no matter how small the `.zst` turns out to be.

Once the book exists, verification will come from SHA-256 digests of book state
at every event boundary, so anyone holding the same pull can check the
reconstruction without the data itself being shared.

`tests/data` is the one apparent exception: about 5 KB of Databento's own test
vectors, Apache-2.0 and vendored unmodified so the decoder tests are hermetic.
Some carry real bytes, so the right to redistribute rests on Databento's
licence grant, not on the content being synthetic. Provenance and SHA-256
digests are in `tests/data`.

## Licence

Not yet chosen.
