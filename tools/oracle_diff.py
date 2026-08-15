"""Differential oracle: our decoder against Databento's, record for record.

Runs `dbn_dump` over a DBN file and regenerates the same tab-separated lines
with `databento_dbn`, the vendor's reference decoder. Any difference in any
field of any record is a failure, reported with the first mismatch.

This is the local half of the oracle and the only half that can see the real
GLBX corpus, which is licensed and never leaves the machine. The CI half runs
databento-cpp over the committed fixtures instead.

    python tools/oracle_diff.py --dump build/debug/tools/dbn_dump.exe FILE...

Requires: pip install "databento-dbn==0.66.0" zstandard
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
from typing import Iterator

import databento_dbn as dbn
import zstandard

CHUNK = 1 << 20


def scalar(value) -> str:
    """Normalize a binding value to the text dbn_dump emits."""
    value = getattr(value, "value", value)
    if isinstance(value, str):
        return value
    return str(int(value))


def tristate(value) -> str:
    """The bindings surface the wire chars 'Y'/'N'/'~' as True/False/None."""
    if value is None:
        return "~"
    if isinstance(value, bool):
        return "Y" if value else "N"
    return str(getattr(value, "value", value))


def header(rec) -> list[str]:
    return [
        scalar(rec.rtype),
        scalar(rec.publisher_id),
        scalar(rec.instrument_id),
        scalar(rec.ts_event),
    ]


def fields_for(rec) -> list[str]:
    name = type(rec).__name__
    if name == "MBOMsg":
        return header(rec) + [
            scalar(rec.order_id),
            scalar(rec.price),
            scalar(rec.size),
            scalar(rec.flags),
            scalar(rec.channel_id),
            scalar(rec.action),
            scalar(rec.side),
            scalar(rec.ts_recv),
            scalar(rec.ts_in_delta),
            scalar(rec.sequence),
        ]
    if name == "TradeMsg":
        return header(rec) + [
            scalar(rec.price),
            scalar(rec.size),
            scalar(rec.action),
            scalar(rec.side),
            scalar(rec.flags),
            scalar(rec.depth),
            scalar(rec.ts_recv),
            scalar(rec.ts_in_delta),
            scalar(rec.sequence),
        ]
    if name == "MBP10Msg":
        out = header(rec) + [
            scalar(rec.price),
            scalar(rec.size),
            scalar(rec.action),
            scalar(rec.side),
            scalar(rec.flags),
            scalar(rec.depth),
            scalar(rec.ts_recv),
            scalar(rec.ts_in_delta),
            scalar(rec.sequence),
        ]
        for level in rec.levels:
            out += [
                scalar(level.bid_px),
                scalar(level.ask_px),
                scalar(level.bid_sz),
                scalar(level.ask_sz),
                scalar(level.bid_ct),
                scalar(level.ask_ct),
            ]
        return out
    if name == "StatusMsg":
        return header(rec) + [
            scalar(rec.ts_recv),
            scalar(rec.action),
            scalar(rec.reason),
            scalar(rec.trading_event),
            tristate(rec.is_trading),
            tristate(rec.is_quoting),
            tristate(rec.is_short_sell_restricted),
        ]
    if name == "InstrumentDefMsg":
        return header(rec) + [
            scalar(getattr(rec, field))
            for field in DEFINITION_FIELDS
        ]
    raise SystemExit(f"oracle_diff does not handle {name}")


DEFINITION_FIELDS = [
    "ts_recv",
    "min_price_increment",
    "display_factor",
    "expiration",
    "activation",
    "high_limit_price",
    "low_limit_price",
    "max_price_variation",
    "unit_of_measure_qty",
    "min_price_increment_amount",
    "price_ratio",
    "strike_price",
    "raw_instrument_id",
    "leg_price",
    "leg_delta",
    "inst_attrib_value",
    "underlying_id",
    "market_depth_implied",
    "market_depth",
    "market_segment_id",
    "max_trade_vol",
    "min_lot_size",
    "min_lot_size_block",
    "min_lot_size_round_lot",
    "min_trade_vol",
    "contract_multiplier",
    "decay_quantity",
    "original_contract_size",
    "leg_instrument_id",
    "leg_ratio_price_numerator",
    "leg_ratio_price_denominator",
    "leg_ratio_qty_numerator",
    "leg_ratio_qty_denominator",
    "leg_underlying_id",
    "appl_id",
    "maturity_year",
    "decay_start_date",
    "channel_id",
    "leg_count",
    "leg_index",
    "currency",
    "settl_currency",
    "secsubtype",
    "raw_symbol",
    "group",
    "exchange",
    "asset",
    "cfi",
    "security_type",
    "unit_of_measure",
    "underlying",
    "strike_price_currency",
    "leg_raw_symbol",
    "instrument_class",
    "match_algorithm",
    "main_fraction",
    "price_display_format",
    "sub_fraction",
    "underlying_product",
    "security_update_action",
    "maturity_month",
    "maturity_day",
    "maturity_week",
    "user_defined_instrument",
    "contract_multiplier_unit",
    "flow_schedule_type",
    "tick_rule",
    "leg_instrument_class",
    "leg_side",
]


def oracle_records(path: pathlib.Path, limit: int) -> Iterator[object]:
    decoder = dbn.DBNDecoder()
    seen = 0
    with path.open("rb") as raw:
        stream = (
            zstandard.ZstdDecompressor().stream_reader(raw)
            if path.suffix == ".zst"
            else raw
        )
        while True:
            chunk = stream.read(CHUNK)
            if not chunk:
                return
            decoder.write(chunk)
            for record in decoder.decode():
                if isinstance(record, dbn.Metadata):
                    continue
                yield record
                seen += 1
                if limit and seen >= limit:
                    return


def diff_file(dump: str, path: pathlib.Path, limit: int) -> int:
    argv = [dump, str(path)]
    if limit:
        argv += ["--limit", str(limit)]
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    assert proc.stdout is not None

    count = 0
    for record in oracle_records(path, limit):
        expected = "\t".join(fields_for(record))
        actual = proc.stdout.readline().rstrip("\n")
        if not actual:
            print(f"FAIL {path.name}: our decoder stopped after {count} records", file=sys.stderr)
            proc.kill()
            return 1
        if actual != expected:
            print(f"FAIL {path.name}: record {count} differs", file=sys.stderr)
            for i, (a, b) in enumerate(zip(actual.split("\t"), expected.split("\t"))):
                if a != b:
                    print(f"  field {i}: ours={a!r} oracle={b!r}", file=sys.stderr)
            proc.kill()
            return 1
        count += 1

    trailing = proc.stdout.readline()
    proc.stdout.close()
    proc.wait()
    if trailing:
        print(f"FAIL {path.name}: our decoder produced records past the oracle's end", file=sys.stderr)
        return 1
    if proc.returncode != 0:
        print(f"FAIL {path.name}: dbn_dump exited {proc.returncode}", file=sys.stderr)
        return 1

    print(f"ok   {path.name}: {count} records identical")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", required=True, help="path to the dbn_dump executable")
    parser.add_argument("--limit", type=int, default=0, help="stop after N records")
    parser.add_argument("files", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    failures = 0
    for path in args.files:
        failures += diff_file(args.dump, path, args.limit)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
