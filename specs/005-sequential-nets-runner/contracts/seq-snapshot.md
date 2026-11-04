# Contract: sequential snapshot rows, CSV log, Frame channels

## Snapshot arrays (`world::Snapshot`)

- `seq_serial`: `u64` stable flop serials, dense slot order.
- `seq_value`: `u8` Q values (0/1), same order, same length.
- `seq_output`: `u8` settled block outputs in `block_output_count` order (0/1).
- Serials never reused across slot recycle (pool rule); not ascending after
  recycle — consumers validate uniqueness within a step, preserving order.

## CSV log (`LogicCsvSink`, `--emit-csv` with solver `logic`)

```text
step,time,seq_id,value
0,0.01,0,1
...
```

- One row per live flop per snapshot, header once, `max_digits10` doubles.
- `seq_id` = stable serial in snapshot slot order. Re-read `==` round-trips.
- Best-effort observer: sticky status, never throws (same as `SignalsCsvSink`).

## Frame projection (`frame_from_logic_snapshot`)

- Channels: `seq.count` + one `seq.<serial>.Q` per live flop (slot order).
- No items (scalar probes, not glyphs). Over-bound nets (1 + N > 64 channels)
  refused loudly at publish time with per-cause counters (same rule as signals).
- Registry: `kFrameSources` gains `{"logic", &frame_from_logic_snapshot}`.
