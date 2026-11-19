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
