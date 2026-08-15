# Decoder fixtures

**No licensed market data is committed here.** These are Databento's own DBN
test vectors, published under Apache-2.0 in their reference implementation and
vendored unmodified so the test suite is hermetic and runs offline. Some carry
real bytes — the `mbo`, `trades` and `mbp-10` files are two records each of
`ESH1` from 2020-12-28 — so the right to redistribute them rests on Databento's
license grant, not on the content being synthetic. The validation corpus is a
purchased GLBX window that never enters this repository; see the corpus section
of the top-level README for how it is obtained.

Source: [databento/dbn](https://github.com/databento/dbn) at tag `v0.66.0`,
path `tests/data/`. Licensed Apache-2.0; see `NOTICE`.

Two files per schema serve opposite purposes. The `v3` files are the accept
corpus — this decoder targets DBN v3 and must read them record-for-record. The
`v1` and `v2` files are the reject corpus: the version gate must refuse them
loudly rather than misread a struct whose layout changed underneath it. That
matters most for `definition`, where v3 moved to 520 bytes, widened `asset` to
11 and `raw_instrument_id` to 64 bits, added the `leg_*` fields, and dropped
`trading_reference_price`, `settl_price_type` and `md_security_trading_status`.

`test_data.mbo.v3.dbn` is uncompressed, covering the path where a file is raw
DBN rather than zstd-framed.

| File | SHA-256 |
|---|---|
| `test_data.definition.v1.dbn.zst` | `b20172e487942cd94200850eb56fad5419037a5666cd34514cac99aad1b550d6` |
| `test_data.definition.v2.dbn.zst` | `8f77c53b2fa9bdabc7d3b5ac67732d4314cb5ff21ddf431e281c9ec25f4fa925` |
| `test_data.definition.v3.dbn.zst` | `7479ca8d05e64e4b58daf2512ddee831aec3e9f3551a959cb9ed28fe9d58fc4b` |
| `test_data.mbo.v1.dbn.zst` | `553590d8f28e348de34877b71f7b676db3ccdbe515c8086b6211192c2c4e6519` |
| `test_data.mbo.v2.dbn.zst` | `821461fc2a66ec68e049bd9a6fbe2b4822bfdddc0fc3bee7dea8fa4d8a3e585b` |
| `test_data.mbo.v3.dbn` | `d2a526d952f845ab8f03ceac42ec828a2541e0b5e2e8a64df2e8bbaa1e898184` |
| `test_data.mbo.v3.dbn.zst` | `6b2fcfa985cc34f6b40470929035945ce9c131a2f97ffcb736cc284094218c47` |
| `test_data.mbp-10.v1.dbn.zst` | `e8203f620ad7f16586b022f8eb12ae0a7fa60a254e00c6016913421ec3258d4f` |
| `test_data.mbp-10.v2.dbn.zst` | `b782e3ae6acc925c75dadb51ee6fd936aea250281445d581f53e13ce4b1e3739` |
| `test_data.mbp-10.v3.dbn.zst` | `12cc762d20fb0190191f5f631cf229c5a2d0ad6fc52fb6a41346594c05e6284a` |
| `test_data.status.v2.dbn.zst` | `921c81f93f4de55d6f1d3b22ff3a3e477db632e2384729acb560605c1534474b` |
| `test_data.status.v3.dbn.zst` | `0e4dd0225645eecedcaf7f30eaac6928853d8d407da9e47aeef867fe3a7790d6` |
| `test_data.trades.v1.dbn.zst` | `743232d8cd463725ff389c263b832860450b75872712e946f9636b9e33d1a8fc` |
| `test_data.trades.v2.dbn.zst` | `4f7293b9a06c31c0e3f88e23b309fd25fe3df5664608aba322e50b24853abd15` |
| `test_data.trades.v3.dbn.zst` | `0036503b694f1334227f7042a0887d608d75a88dd041548da30203433c12f1e5` |
