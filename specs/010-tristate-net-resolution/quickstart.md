# Quickstart: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Feature**: [spec.md](spec.md) | **Date**: 2026-11-19

Runnable validation for the funnel contract
([contracts/resolve-net.md](contracts/resolve-net.md),
[data-model.md](data-model.md)). This feature has no new CLI surface:
it is an engine-contract change, so validation = the re-pinned test
binary + regression smokes proving v1 behavior is untouched.

## Prerequisites

- CMake ≥ 3.20, GCC 13.3 baseline, out-of-source builds only.

## Setup

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
```

(An existing configured `build/` is fine; rebuild only.)

## Validation

### 1. Narrowest: the funnel's new resolution table

```sh
ctest --test-dir build -R '^logic$' --output-on-failure
```

**Expected**: the `logic` test binary passes, printing `test_x: OK`
lines. Its funnel region now pins, against the IEEE 1164-anchored
reference (not the implementation's own fold):

- every driver multiset of size 0..4 → the table result
  (empty/all-Z → `kHighZ`; single active → level; consensus →
  level; low+high → `kUnknown`; any X → `kUnknown`)
- permutation invariance: all orderings of representative spans
  resolve identically
- v1 passthrough: `{L}→L`, `{H}→H`
- ill-formed span (`nullptr`, n>0) → `false`

### 2. Regression: every live v1 path unchanged

```sh
ctest --test-dir build --output-on-failure
```

**Expected**: full suite green. Every test except the funnel's
superseded placeholder assertions (re-pinned in the same change)
passes without modification — including all consumer-rejection tests
(gate inputs, sequential control nets, Q pool) and all
logic-solver/runner tests.

### 3. Smoke: CLI logic behavior byte-identical

```sh
printf 'logic add AND --inputs 2\nlogic solve --in 01\nexit\n' | ./build/PhysicsSimApp
```

**Expected**: `added AND1 kind=AND inputs=2`, then the single-AND
solve echo with `AND1.Y=0` — identical to pre-change output. No CLI
verb, error text, or exit code changes anywhere.

```sh
./build/PhysicsSimApp sim run --solver logic --seq-file examples/logic/nand_pair.yaml --steps 4
```

**Expected**:

```text
sim run: solver=logic
steps: 4 x 0.01s -> t=0.04s
particles: 0
```

identical to pre-change output (verified against the current build).

```sh
./build/PhysicsSimApp sim run --steps 100 --particles 1
```

**Expected**: standard `sim run` output unchanged (engine-level
regression sentinel).

### 4. Docs build (API coverage gate)

```sh
docs/build.sh
```

**Expected**: the generated site builds cleanly — the updated
`resolve_net` Doxygen contract (total resolution, `n==0`, `false`
meaning) is picked up with zero Doxygen warnings, and the catalog
audit reports zero missing / zero ghost entries.

## What "done" looks like

| Criterion | Check |
|---|---|
| SC-001 exhaustive table | step 1 (69 multisets asserted) |
| SC-002 no regressions outside the superseded placeholder | step 2 (suite green) |
| SC-003 order-invariance | step 1 (permutation cases) |
| SC-004 three distinguishable scenarios | step 1: `{H,Z}→H`, `{L,H}→X`, `{Z,Z}→Z` all in one binary |
| Zero warnings | steps 1–2 build output |
| Consumer X/Z rejection preserved | step 2 (unmodified consumer tests) |
| v1 CLI byte-identical | step 3 |

## Out of scope (verified absent)

No changes to: netlist wiring rules, `wire`/`unwire` verbs, YAML
workspace schema, world serialization, snapshot/Frame channels, GUI,
optimizer. If any of those differ after the change, the diff has
escaped its scope — treat as a defect.
