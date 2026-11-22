# Quickstart: Validating the Scale-Profiling Baseline (015)

Runnable end-to-end validation scenarios. Each proves one spec
criterion; run them in order after implementation. No implementation
detail here — commands and expected outcomes only.

## Prerequisites

- GCC 13.3 baseline toolchain, CMake ≥ 3.20, Linux (WSL2 host class
  for comparing against committed numbers).
- ~1 GB free RAM for the ≥1M-ladder runs (netlist ~230 MB +
  scratch); a few hundred MB for the quick ladder.
- Reference artifacts: [spec.md](spec.md) (FR/SC numbering),
  [contracts/bench-invocation.md](contracts/bench-invocation.md),
  [contracts/baseline-report.md](contracts/baseline-report.md).

## Scenario 1 — alias behavior is untouched (SC-004, FR-009)

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

**Expected**: the whole pre-feature suite passes — goldens, example
replays, refusals, save/load round-trips — including the new
`test_logic_scale` (default 65,536 gates; seconds). No existing
test's output text changed.

## Scenario 2 — the alias instantiates identically (FR-009, FR-001)

```sh
ctest --test-dir build -R logic_scale --output-on-failure
```

**Expected**: `test_logic_scale: OK` — alias identity
`static_assert`s (sizes/sentinels), scale-instantiation scalar/word
bit-exact parity, prefix-slice agreement, bit-identical repeat
solves, loud-overflow/cycle/single-driver refusals at scale.
Optional: `WENGINE_LOGIC_SCALE_GATES=262144 ctest --test-dir build -R
logic_scale` for a bigger manual parity run.

## Scenario 3 — the harness runs and reads honestly (SC-002, FR-003/FR-006)

```sh
cmake --preset bench
cmake --build --preset bench
./build-bench/bench_logic_scale            # quick default ladder
```

**Expected**: per-stage, per-size lines (`growth-add`, `compile-rebuild`,
`solve`, `word`, …) each with median/min, footprint, checksum; warmup
never mixed into steady-state; any stage exceeding its budget prints
`UNMEASURED-AT-SIZE` loudly and the run continues; exit 0. Identical
args re-run ⇒ identical checksums (timings vary, shapes never).

## Scenario 4 — million-gate reach in one session (SC-001, FR-001/FR-010)

Run the committed full-ladder invocation exactly as printed in
`docs/benchmarks/logic-scale.md` (its verbatim form lives there; the
1M point takes wall time on the order of the very bottlenecks being
baselined — bring the memory budget).

**Expected**: the ladder reaches ≥1,000,000 gates: the mixed motif
constructs, solves, word-evaluates, and takes one budgeted step;
scalar and word results agree bit-exactly; repeats are bit-identical;
the compile/step/dedup stages show their measured (or
budget-exceeded) costs at the 1M end.

## Scenario 5 — the committed baseline reproduces (SC-002, FR-004/FR-008)

Re-run Scenario 4's invocation on the pinned environment and compare
against the Baseline section.

**Expected**: every measured stage lands within the page's stated
variance; unmeasured-at-size entries reproduce as unmeasured; the
page contains zero extrapolated numbers.

## Scenario 6 — attribution and the gate are on the page (SC-003, FR-005/FR-007)

Read `docs/benchmarks/logic-scale.md`.

**Expected**: all six named suspects carry measured verdicts (ratios
grounded in table rows — cleared verdicts are legitimate); the 1M-end
ranking splits mutation/compile/eval-side; the gate paragraph is
operational (a hotspot-less optimization proposal is rejectable by
citation alone); the fidelity note states the u32-instantiation
caveat with alias anchor points.

## Scenario 7 — the no-regression pin on measurement (FR-008/FR-009)

```sh
./build-bench/bench_logic 64 16 20000
```

**Expected**: `bench_logic`'s ns/gate-word and shared/rebuild lines
match pre-feature runs within the stated noise — the current scale
paid nothing measurable for the enablement; nothing in this feature
optimized anything.

## Scenario 8 — catalogs stay clean (constitution docs duty)

```sh
python3 docs/audit_catalogs.py
```

**Expected**: `benchmarks: N/N covered` and `tests: N/N covered`
(zero missing, zero ghost), including `## bench_logic_scale` and
`## test_logic_scale` anchors; README run-list, catalog paragraph,
and brace list mention the new bench; `docs/benchmarks/index.md`
carries the committed-numbers carve-out sentence.
