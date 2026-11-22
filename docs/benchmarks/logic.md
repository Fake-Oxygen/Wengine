# Logic benchmarks

Bit-parallel gate-eval cost, isolated from solvers and transport.

## bench_logic

**Measured path**: `eval_block_word` over `words` consecutive 64-row slices
of a live Netlist: `width` gates per level by `depth` levels (product capped
at `kMaxGates`), caller-owned scratch, allocation-free steady state.

**Why it matters**: floors word-level truth-eval throughput for the logic
domain; the per-gate-word rate sizes batch simulation budgets.

**How to run**:

```sh
./build-bench/bench_logic 8 8 2000
```

Positional args: width, depth, words (defaults 16 16 20000). Setup rejects
`width*depth` outside 1..`kMaxGates` with exit 1.

**Interpretation**: `bench_logic: <gates> gates (<w>w x <d>d) x <words>
words, <x> ns/gate-word (checksum=<c>)`; compare `ns/gate-word`.
`checksum` mixes final outputs with the gate count.

**Pitfalls**: gate kinds cycle all 8 combinational ops via an `argc`-seeded
xorshift with opaque fan-in (1 for NOT/BUF, else 2–8) and forward-only wiring
(DAG by construction), so neither build nor eval can fold; one warmup slice
precedes timing and `do_not_optimize` holds the checksum. Tiny widths wrap
the word index modulo enumerated slices; huge nets (`n_inputs >= 31`)
accept every slice, so a failure there is a real error.

## bench_logic_yaml

**Measured path**: canonical workspace save (`save_text`) and load
(`load_text`) over a size ladder to 1M gates: layered bulk-built net
(first 6 gates dangling-input NOTs, every later gate fan-in 2 from
xorshift-earlier slots), one-shot save + load per size with a
save/load/save byte-identity check.

**Why it matters**: floors workspace I/O budgets for large nets; the
100k-gate load must stay under 5 s with bounded (linear) RAM and no
format change.

**How to run**:

```sh
./build-bench/bench_logic_yaml
./build-bench/bench_logic_yaml 10000 100000 1048576
```

Positional args: sizes (defaults `1024 10000 100000 1048576`); each
clamps to 1..`kMaxGates`.

**Interpretation**: `bench_logic_yaml save/load: <size> gates, <bytes>
B, <s> s`; compare wall seconds across commits. `checksum` pins the
bytes; `roundtrip=identical` pins the format. Timings vary run to run;
bytes and the identical verdict never do.

**Pitfalls**: construction is bulk slot-direct setup, never timed; only
the save/load stages are measured. The 1M point moves ~150 MB of
canonical text through the yaml-cpp DOM, so it takes ~1 min — that is
the parser floor, not a regression.
