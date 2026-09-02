from __future__ import annotations

import pytest

import bookreplay as br

TICK = 250_000_000
POINT = 1_000_000_000
T0 = 1_785_888_000_000_000_000
MS = 1_000_000


def px(whole: int, ticks: int = 0) -> int:
    return whole * POINT + ticks * TICK


def tk(whole: int, ticks: int = 0) -> int:
    return whole * 4 + ticks


def config(entry_ns: int = 0, arm_ns: int = 0, cancel_ns: int = 0) -> br.ReplayConfig:
    return br.ReplayConfig(
        latency=br.Latency(
            order_entry_ns=entry_ns, protection_arm_ns=arm_ns, cancel_ns=cancel_ns
        ),
        scale=br.TickScale(TICK),
    )


def rising() -> br.TickBuffer:
    return br.ticks_from(
        [
            br.Tick(ts=T0 + i * MS, price=px(29000, i), size=5, aggressor=br.Side.BID)
            for i in (1, 2, 3)
        ]
    )


def falling() -> br.TickBuffer:
    return br.ticks_from(
        [
            br.Tick(ts=T0 + MS, price=px(29000, 0), size=5, aggressor=br.Side.ASK),
            br.Tick(ts=T0 + 2 * MS, price=px(28999, 3), size=5, aggressor=br.Side.ASK),
            br.Tick(ts=T0 + 3 * MS, price=px(28999, 2), size=5, aggressor=br.Side.ASK),
        ]
    )


def test_tick_buffer_is_indexable_and_counts_its_prints() -> None:
    ticks = rising()
    assert len(ticks) == 3
    assert ticks[0].price == px(29000, 1)
    assert ticks.stats.ticks == 3
    with pytest.raises(IndexError):
        _ = ticks[3]


def test_tick_buffer_indexes_from_the_end_like_a_sequence() -> None:
    ticks = rising()
    assert ticks[-1].price == ticks[2].price
    assert ticks[-3].price == ticks[0].price
    with pytest.raises(IndexError):
        _ = ticks[-4]


def test_a_config_reads_back_the_latency_and_scale_it_was_built_with() -> None:
    c = config(entry_ns=7, arm_ns=11, cancel_ns=13)
    assert c.latency.order_entry_ns == 7
    assert c.latency.protection_arm_ns == 11
    assert c.latency.cancel_ns == 13
    assert c.scale.tick_size == TICK


def test_market_order_fills_at_the_first_print_at_or_after_it_is_live() -> None:
    replay = br.Replay(rising(), config())
    replay.submit(
        br.Order(id=1, type=br.OrderType.MARKET, side=br.Side.BID, qty=1, live_from_ns=T0)
    )

    fills = replay.advance_to(T0 + 10 * MS)
    assert len(fills) == 1
    assert fills[0].ts_ns == T0 + MS
    assert fills[0].price == px(29000, 1)
    assert fills[0].reason == br.FillReason.MARKET


def test_resting_limit_needs_a_print_strictly_through_its_price() -> None:
    replay = br.Replay(falling(), config())
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.LIMIT,
            side=br.Side.BID,
            qty=1,
            live_from_ns=T0,
            limit_ticks=tk(29000, 0),
        )
    )

    fills = replay.advance_to(T0 + 10 * MS)
    assert len(fills) == 1
    assert fills[0].ts_ns == T0 + 2 * MS
    assert fills[0].price == px(29000, 0)


def test_a_stop_is_elected_by_a_print_at_its_trigger() -> None:
    replay = br.Replay(rising(), config())
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.STOP,
            side=br.Side.BID,
            qty=1,
            live_from_ns=T0,
            trigger_ticks=tk(29000, 1),
        )
    )

    fills = replay.advance_to(T0 + 10 * MS)
    assert len(fills) == 1
    assert fills[0].reason == br.FillReason.STOP_ELECTED
    assert replay.stats.elections == 1


def test_oco_is_quantity_linked_rather_than_cancel_on_first_fill() -> None:
    ticks = br.ticks_from(
        [br.Tick(ts=T0 + MS, price=px(29000, 1), size=3, aggressor=br.Side.BID)]
    )
    replay = br.Replay(ticks, config())
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.MARKET,
            side=br.Side.BID,
            qty=10,
            live_from_ns=T0,
            oco_group=7,
        )
    )
    replay.submit(
        br.Order(
            id=2,
            type=br.OrderType.LIMIT,
            side=br.Side.ASK,
            qty=10,
            live_from_ns=T0,
            limit_ticks=tk(29500, 0),
            oco_group=7,
        )
    )

    replay.advance_to(T0 + 10 * MS)
    sibling = replay.order(2)
    assert sibling.remaining == 7
    assert sibling.oco_reduced == 3
    assert sibling.status != br.OrderStatus.OCO_CANCELLED


def test_entry_latency_moves_which_print_fills_the_order() -> None:
    ticks = rising()
    prices = []
    for entry_ns in (0, MS + MS // 2, 2 * MS + MS // 2):
        replay = br.Replay(ticks, config(entry_ns=entry_ns))
        replay.submit(
            br.Order(id=1, type=br.OrderType.MARKET, side=br.Side.BID, qty=1, live_from_ns=T0)
        )
        prices.append(replay.advance_to(T0 + 10 * MS)[0].price)

    assert prices == [px(29000, 1), px(29000, 2), px(29000, 3)]


def test_advance_to_refuses_to_rewind() -> None:
    replay = br.Replay(rising(), config())
    replay.advance_to(T0 + 2 * MS)
    with pytest.raises(br.ReplayError):
        replay.advance_to(T0 + MS)


def test_a_protective_leg_pays_the_arm_latency() -> None:
    replay = br.Replay(rising(), config(arm_ns=2 * MS + MS // 2))
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.MARKET,
            side=br.Side.BID,
            qty=1,
            live_from_ns=T0,
            latency=br.LatencyClass.PROTECTION_ARM,
        )
    )

    fills = replay.advance_to(T0 + 10 * MS)
    assert fills[0].ts_ns == T0 + 3 * MS


def test_latency_requires_every_parameter_by_name() -> None:
    with pytest.raises(TypeError):
        br.Latency(order_entry_ns=0, protection_arm_ns=0)  # type: ignore[call-arg]
    with pytest.raises(br.ReplayError):
        br.Latency(order_entry_ns=-1, protection_arm_ns=0, cancel_ns=0)


def test_naked_window_scan_reports_a_stop_reached_inside_the_window() -> None:
    queries = [
        br.NakedWindowQuery(
            entry_fill_ts=T0 + MS,
            window_ns=3 * MS,
            entry_price=px(29000, 0),
            stop_price=px(28999, 2),
            position_side=br.Side.BID,
        )
    ]
    results = br.naked_window_scan(falling(), queries, br.TickScale(TICK))

    assert len(results) == 1
    assert results[0].stop_reached
    assert results[0].first_reach_ts == T0 + 3 * MS
    assert results[0].mae_ticks == 2


def test_source_counters_reconcile() -> None:
    replay = br.Replay(rising(), config())
    replay.advance_to(T0 + 10 * MS)
    assert replay.source_stats.reconciles
    assert replay.stats.reallocations == 0
    assert replay.exhausted


def test_an_unknown_order_id_raises() -> None:
    replay = br.Replay(rising(), config())
    with pytest.raises(br.ReplayError):
        replay.order(9)


def test_running_to_never_ns_leaves_a_working_order_live() -> None:
    replay = br.Replay(rising(), config())
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.LIMIT,
            side=br.Side.BID,
            qty=1,
            live_from_ns=T0,
            limit_ticks=tk(28000, 0),
        )
    )

    assert replay.advance_to(br.NEVER_NS) == []
    assert replay.order(1).status == br.OrderStatus.LIVE
    assert replay.stats.cancels_applied == 0


def test_ticks_from_refuses_an_unsorted_stream() -> None:
    with pytest.raises(br.ReplayError):
        br.ticks_from(
            [
                br.Tick(ts=T0 + 3 * MS, price=px(29000, 0), size=5, aggressor=br.Side.ASK),
                br.Tick(ts=T0 + MS, price=px(29000, 0), size=5, aggressor=br.Side.ASK),
            ]
        )


def test_ticks_from_refuses_a_zero_size_print() -> None:
    with pytest.raises(br.ReplayError):
        br.ticks_from([br.Tick(ts=T0, price=px(29000, 0), size=0, aggressor=br.Side.ASK)])


def test_ticks_from_refuses_an_undefined_price() -> None:
    with pytest.raises(br.ReplayError):
        br.ticks_from([br.Tick(ts=T0, price=2**63 - 1, size=5, aggressor=br.Side.ASK)])


def test_a_market_buy_pays_a_tick_through_a_print_the_other_side_aggressed() -> None:
    replay = br.Replay(falling(), config())
    replay.submit(
        br.Order(id=1, type=br.OrderType.MARKET, side=br.Side.BID, qty=1, live_from_ns=T0)
    )

    fills = replay.advance_to(T0 + 10 * MS)
    assert len(fills) == 1
    assert fills[0].price == px(29000, 1)
    assert replay.stats.tick_charged_qty == 1


def test_a_stop_already_through_the_market_is_refused_at_order_entry() -> None:
    replay = br.Replay(rising(), config(entry_ns=MS + MS // 2))
    replay.submit(
        br.Order(
            id=1,
            type=br.OrderType.STOP,
            side=br.Side.ASK,
            qty=1,
            trigger_ticks=tk(29010, 0),
            live_from_ns=T0,
        )
    )

    assert replay.advance_to(T0 + 10 * MS) == []
    assert replay.order(1).status == br.OrderStatus.REJECTED
    assert replay.stats.stop_entry_rejects == 1


def test_ticks_from_refuses_a_negative_timestamp() -> None:
    with pytest.raises(br.ReplayError):
        br.ticks_from([br.Tick(ts=-1, price=px(29000, 0), size=5, aggressor=br.Side.ASK)])


def test_a_decode_failure_surfaces_as_dbn_error_not_as_its_base() -> None:
    assert issubclass(br.DbnError, br.BookreplayError)
    assert issubclass(br.ReplayError, br.BookreplayError)
    with pytest.raises(br.DbnError):
        br.load_ticks("no_such_file.dbn", instrument_id=br.ANY_INSTRUMENT)


def test_dbn_entry_points_require_an_explicit_instrument_id() -> None:
    with pytest.raises(TypeError):
        br.load_ticks("unused.dbn")  # type: ignore[call-arg]
    with pytest.raises(TypeError):
        br.Replay.open_dbn("unused.dbn", config())  # type: ignore[call-arg]


def test_a_replay_refuses_prints_from_a_second_instrument() -> None:
    ticks = br.ticks_from(
        [
            br.Tick(
                ts=T0 + MS, price=px(29000, 0), size=5, aggressor=br.Side.BID, instrument_id=1
            ),
            br.Tick(ts=T0 + 2 * MS, price=px(1, 0), size=5, aggressor=br.Side.BID, instrument_id=2),
        ]
    )
    replay = br.Replay(ticks, config())
    with pytest.raises(br.ReplayError):
        replay.advance_to(T0 + 10 * MS)
