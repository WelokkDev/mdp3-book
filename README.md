# bookreplay

CME MDP 3.0 limit order book reconstruction from Databento DBN, checked against
an independent vendor snapshot.

Status: the decoder and the invariant harness exist; the book does not. The
decoder has a validation number, stated below. The book does not have one, and
this file will not claim one until it does.

## What it will be

A C++20 library that decodes Databento DBN files for CME Globex, reconstructs
the full limit order book from the market-by-order (`mbo`) feed, and simulates
one participant's resting orders under price-time FIFO with an explicit latency
model. Its success criterion is a single number: whether the reconstructed book
agrees with Databento's own `mbp-10` on every top-10 level at every event
boundary, across a named validation corpus.

The only third-party dependency in the shipping library is zstd. The decoder is
written from the published DBN specification rather than wrapped around a vendor
library.

## The decoder

`DbnReader` reads DBN v3 files — zstd-framed or raw — for the five schemas this
project needs: `mbo`, `trades`, `mbp-10`, `status` and `definition`. Records are
read in place out of a 64-byte-aligned buffer rather than copied out one at a
time; the casts are well defined because C++20 implicit object creation
(P0593R6) applies to these implicit-lifetime types and the reader rejects any
record length that would break the alignment it relies on.

It decodes DBN version 3 and refuses versions 1 and 2 rather than converting
them. That is deliberate. v3 rewrote `InstrumentDefMsg` — `asset` widened from
7 bytes to 11, `raw_instrument_id` from 32 bits to 64, the `leg_*` fields
arrived, and `trading_reference_price`, `settl_price_type` and
`md_security_trading_status` were removed — so reading a v2 file with the v3
struct produces plausible garbage rather than an error. The committed v1 and v2
fixtures exist to prove the refusal fires.

### What it is checked against

Three independent layers, because one is not evidence:

1. The real corpus. All 28,562,350 records of a full GLBX.MDP3 `mbo` day
   (2026-08-05, NQ parent, 527 MB compressed) decode field-for-field
   identically to Databento's own decoder. Not a sample — every record in the
   file.
2. Every schema, against the vendor's test vectors. `mbo`, `trades`, `mbp-10`,
   `status` and `definition` each round-trip identically against Databento's
   published fixtures, including the 520-byte v3 definition record with all 69
   of its fields.
3. Synthetic streams. A test-only encoder generates streams the decoder has
   never seen and asserts they survive a round trip bit-exact, including
   `ts_out`-extended records, records straddling buffer refills, and
   multi-frame zstd.

Two differential oracles run against two independent implementations of the
published format: `databento-dbn` locally, field by field, which is the only
one that can see the licensed corpus; and `databento-cpp` in Linux CI, byte for
byte over the committed fixtures. Both are pinned. Neither is linked into the
shipping library.

Malformed input fails loudly rather than half-decoding: truncated preludes,
wrong magic, unsupported versions, undersized metadata frames, symbol counts
larger than the frame, unknown rtypes, record lengths shorter than their own
struct or misaligned, truncated final records, corrupt and truncated zstd, and
legacy DBZ containers each raise `DbnError` naming what was found. The suite
also runs under ASan and UBSan.

What this does not yet show: only the `mbo` schema has been checked against
a real CME pull; the other four are checked against Databento's fixtures, which
are two to four records each. The `mbp-10` agreement number in the correctness
section below needs data that has not been purchased.

## Why the invariants came first

NautilusTrader — 25k stars, 288 KB of tests, real GLBX golden fixtures — mapped
`action='F'` to a book update, shipped it, and produced a book crossed by ~220
points on a full CME day: best bid 29,909.75 against best ask 29,689.00, with
market buys filling at stale phantom asks hundreds of points away. 418,061 Fill
records became deltas. It was found by an outside user, because nothing in the
test suite was capable of failing.

Three checks would have caught it, and all three are about ten lines:

1. Mutation/action reconciliation — every `A`, `C`, `M` and `R` counts as
   exactly one mutation and every `T`, `F` and `N` as exactly zero, checked
   per record rather than as an end-of-run sum, so two miscounts cannot cancel.
2. `best_bid < best_ask` at event boundaries, gated on `Trading` state.
3. No `T` or `F` may change order membership — neither materializing an order
   id that was never added (the defect above, by cause rather than by symptom)
   nor erasing the resting order it names, which is the mirror-image bug a
   matching-engine mental model produces and no count can see.

They live in `include/bookreplay/invariants.hpp`, they were written before a
single price level existed, and `tests/toy_book.hpp` contains deliberately
broken books that reproduce the bug so the tests prove the invariants *fire*.
An invariant that has never failed is not evidence of anything.

## Build

Requires a C++20 compiler, CMake ≥ 3.25, and Ninja. GoogleTest and zstd are
fetched at configure time, zstd pinned by release-tarball hash rather than by
tag, since a tag can move. `-DBOOKREPLAY_USE_SYSTEM_ZSTD=ON` links an installed
one instead.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

CI builds GCC, Clang and MSVC with warnings as errors, runs the suite under
ASan and UBSan, and runs both differential oracles.

To re-run the local oracle against your own DBN files:

```sh
pip install "databento-dbn==0.66.0" zstandard
python tools/oracle_diff.py --dump build/debug/tools/dbn_dump YOUR_FILE.dbn.zst
```

## Market data is not in this repository

No `.dbn`, `.dbn.zst` or derived market data file is ever committed —
redistributing a licensed CME window is not something a public repo may do.
Reproducing the validation therefore requires your own Databento account.

The corpus is specified rather than shipped:

| | |
|---|---|
| Dataset | `GLBX.MDP3` |
| Schemas | `mbo`, `mbp-10`, `definition`, `status` |
| Symbology | continuous, `MNQ.v.0` — not `.c.0`, which rolls at expiration and returns the wrong contract during roll weeks |
| Window | always `[00:00 UTC, …]` — a book cannot be reconstructed from an arbitrary instant, only from a snapshot boundary |
| Delivery | batch download |

Two things about that data are load-bearing. Databento renormalized GLBX.MDP3 on
2026-08-08, retroactively across all history, so files pulled before and
after that date differ semantically and must never be mixed — record the pull
date in the path. And billing is on *uncompressed* bytes: MBO is 56 B per
record, so cost is `record_count × 56 B` regardless of how small the `.zst`
turns out to be.

What this repo will ship instead of data, once the book exists to produce it:
synthetic golden streams, and SHA-256 digests of book state at every event
boundary, so anyone holding the same pull can verify bit-identical
reconstruction from the digest alone.

The one apparent exception is `tests/data`, which holds about 5 KB of DBN
files. Those are Databento's own test vectors, published under Apache-2.0 in
their reference implementation and vendored unmodified so the decoder tests are
hermetic. Some carry real bytes — two records of ESH1 from 2020-12-28 — so the
right to redistribute them rests on Databento's license grant, not on the
content being synthetic. It is two to four records per schema, not a licensed
CME window. Provenance and SHA-256 digests are in `tests/data`.

## Licence

Not yet chosen.
