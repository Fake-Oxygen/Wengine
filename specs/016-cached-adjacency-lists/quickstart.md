# Quickstart: Validate Cached Adjacency & Maintained Indexes (016)

Runnable validation scenarios proving the feature end-to-end. This is
a validation guide; implementation details live in the tasks phase.
Invariants being validated: [contracts/adjacency-contract.md](contracts/adjacency-contract.md)
(binds every index), comparison protocol: [contracts/parity-harness.md](contracts/parity-harness.md),
entities: [data-model.md](data-model.md), measurement evidence:
[contracts/measurement-delta.md](contracts/measurement-delta.md).

## Prerequisites

- GCC 13.3 (project baseline), CMake ≥ 3.20, out-of-source builds.
- The 015 baseline committed in `docs/benchmarks/logic-scale.md`
  (the before-numbers every scenario below compares against).

## Scenario 1 — Parity: every index answers like the scan it replaced

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build -R logic_adjacency --output-on-failure
```

Expected: `test_logic_adjacency: OK` — the deterministic corpus
(shared nets with NQ polarity, tie merges/re-elections/dissolves,
flop feedback, dangling drivers, extra-closing cycles, mark/wire
orderings) replayed through every changed operation agrees with the
pre-change scan oracle at all comparison levels (state, verdicts,
orders/levels, slot assignment per add, per-sweep dedup state).
Zero warnings in the build.

## Scenario 2 — The standing suite is untouched

```sh
ctest --test-dir build --output-on-failure
```

Expected: every pre-existing `test_logic*` binary (and the rest of
the suite) passes unmodified — SC-004's bit-identity claim at the
boundary the goldens already pin.

## Scenario 3 — Scale smoke: indexes hold at the top measured size

```sh
WENGINE_LOGIC_SCALE_GATES=262144 ctest --test-dir build -R logic_scale --output-on-failure
```

Expected: `test_logic_scale: OK` — cross-discipline parity
(scalar vs word), bit-identical repeats, and prefix-slice agreement
all still hold with the indexes live; loud refusal paths
(over-capacity, undersized scratch) refuse exactly as before.

## Scenario 4 — Determinism replay (authoring identity)

Run any corpus script twice with identical inputs (the parity binary
does this internally; this scenario is its observable).

Expected: bit-identical outputs on repeat, and the slot assigned to
every added gate equals the lowest-free-slot rule's answer (FR-017) —
allocation order matches pre-change replays exactly.

## Scenario 5 — The measured delta (the feature's reason)

```sh
cmake --preset bench
cmake --build build-bench --target bench_logic_scale
./build-bench/bench_logic_scale   # same ladder sizes/motifs as the baseline tables
```

Expected: the six targeted stages (growth-add, growth-wire,
compile-rebuild, remove, optimize, dedup) flatten per
[contracts/measurement-delta.md](contracts/measurement-delta.md) —
growth-add's per-gate slope stops rising, compile/remove/wire track
edges/cone/fanout instead of pool size, dedup stops rising with pair
count; solve/word/shared-solve/step/reset stay within the baseline
variance policy; footprint delta (including new cache members)
recorded. The dated delta section lands beside the baseline tables in
`docs/benchmarks/logic-scale.md`.

## Scenario 6 — Serialization byte-identity

Via the existing YAML/checkpoint round-trip tests (covered by
Scenario 2's suite): save + load a net with ties, marks, extras,
pulls, sequential state.

Expected: artifacts byte-identical to pre-change (caches never
serialized — FR-011); the loaded net's first query rebuilds indexes
lazily and all Scenario-1 guarantees apply to it.

## Scenario 7 — Documentation/catalog gates

```sh
python3 docs/audit_catalogs.py
```

Expected: zero missing entries, zero ghost anchors (the new test
binary's catalog rows ship with the change; README module table and
test list updated per the house rules).
