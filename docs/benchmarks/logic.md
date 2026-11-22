# Logic benchmarks

Bit-parallel gate-eval cost, isolated from solvers and transport.

## bench_logic

**Measured path**: `eval_block_word` over `words` consecutive 64-row slices
of a live Netlist: `width` gates per level by `depth` levels (product capped
at `kMaxGates`), caller-owned scratch, allocation-free steady state. A
second timed section (017) runs the same net and slices through
`eval_block_words` batches (full batches share one traversal, tail stays
single-slice), so the per-gate-word delta is measured under identical
conditions in one invocation.

**Why it matters**: floors word-level truth-eval throughput for the logic
domain; the per-gate-word rate sizes batch simulation budgets.

**How to run**:

```sh
./build-bench/bench_logic 8 8 2000
```

Positional args: width, depth, words (defaults 16 16 20000). Setup rejects
`width*depth` outside 1..`kMaxGates` with exit 1. The trailing
shared-solve and rebuild sections run after the word sections (the
checksum fold is bounded by the live channel size, so the full sweep
exits 0).

**Interpretation**: `bench_logic: <gates> gates (<w>w x <d>d) x <words>
words, <x> ns/gate-word (checksum=<c>)`; compare `ns/gate-word`.
`checksum` mixes final outputs with the gate count. The batch twin
prints `bench_logic_batch: ... ns/gate-word (checksum=<c2>)` — same
net, same slices; the two checksums differ by construction (different
folding), so cross-path equality is proven by `test_logic_word_batch`
and CLI byte-identity, never by comparing `c` to `c2`.

**Before/after record (017, pinned environment)**: Arch Linux x86_64,
GCC 16.2.1, `bench` preset (`-O3 -DNDEBUG`), default args
(16w x 16d x 20000 words, 256 gates). Five consecutive same-binary runs:

- Before (`feca3c1`, pre-017 binary): single-slice 18.1062
  ns/gate-word (single sample; `checksum=8274861028103430378`).
- After (this tree): single-slice mean 17.34 ns/gate-word (range
  16.95–17.92, n=5, same checksum — bit-identical outputs, single path
  untouched); batch mean 12.20 ns/gate-word (range 11.52–12.96, n=5,
  `checksum=853082083353332480`).
- Speedup (within-binary, identical conditions): **1.42x**
  (17.34/12.20). The before/after single-slice delta sits inside run
  variance — no other stage regressed beyond noise (the single-slice
  body is byte-untouched; the trailing shared/rebuild sections are
  unaffected apart from the checksum bound fix).
- Vectorization sanity (before claiming the win):
  `-fopt-info-vec-optimized` on the `eval.hpp` instantiation confirms
  the batch inner loops vectorize at `-O3` (16-byte packed vectors —
  baseline SSE2 codegen, no `-march`, no intrinsics). The win rests on
  traversal/span-assembly amortization plus auto-vectorization.

**Size ladder (017, same pinned environment, 3 consecutive same-binary
runs per point)**. Before binary: HEAD timed code (single-slice body
identical — verified by diff) plus only the `uint16_t`→`Slot` setup
widening so large nets construct; same `argc`-seeded nets and
checksums as the after binary. Invocations:
`./build-bench/bench_logic 100 100 2000` (10k),
`./build-bench/bench_logic 100 1000 500` (100k),
`./build-bench/bench_logic 1024 1024 100` (1M):

| gates | before single (mean of 3) | after single (mean of 3) | after batch (mean of 3) | speedup |
|---|---|---|---|---|
| 10000 (100w x 100d x 2000 words) | 34.50 (34.25–34.81, cksum `9847405648277703516`) | 34.78 (34.50–35.03, same cksum) | 14.29 (14.12–14.56, cksum `15346078436693247128`) | **2.43x** |
| 100000 (100w x 1000d x 500 words) | 35.35 (35.21–35.51, cksum `7942124226255947182`) | 36.04 (35.79–36.20, same cksum) | 15.92 (15.74–16.20, cksum `10694024693493938936`) | **2.26x** |
| 1048576 (1024w x 1024d x 100 words) | 66.88 (66.31–67.32, cksum `8146893803483361050`) | 66.36 (66.03–66.92, same cksum) | 25.75 (25.53–26.06, cksum `4348210232056982300`) | **2.58x** |

Footprint per point: caller-owned scratch is fixed-size
(single 8 MB, batch 32 MB — `kMaxGates` words × 1 ×/× 4 lanes);
the pool itself grows geometrically with live gates (64-slot seed),
so the 1M point is pool-dominated and still fits the shared host
(the run completes in minutes: ~7.0 s single-slice vs ~2.7 s batch
over 1.05e8 gate-words). Larger nets gain more (2.3–2.6x vs 1.4x at
256 gates): traversal/span-assembly amortization dominates further
once the working set exceeds cache.

**Pitfalls**: gate kinds cycle all 8 combinational ops via an `argc`-seeded
xorshift with opaque fan-in (1 for NOT/BUF, else 2–8) and forward-only wiring
(DAG by construction), so neither build nor eval can fold; one warmup slice
precedes timing and `do_not_optimize` holds the checksum. Tiny widths wrap
the word index modulo enumerated slices; huge nets (`n_inputs >= 31`)
accept every slice, so a failure there is a real error. Timings vary run
to run (shared interactive host) — compare within-binary ratios across
repetitions, never single samples across binaries.

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
