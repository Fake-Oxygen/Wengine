# Quickstart: DFF Control Set

**Feature**: `specs/002-dff-control-set/spec.md` | **Contracts**: [session-verbs.md](contracts/session-verbs.md) | **Date**: 2026-10-31

Validates the feature end-to-end in a headless session. Each scenario is
runnable as typed (one-shot `logic` lines via pipe or REPL) and states
its expected outcome. Full truth-table goldens live in the test plan
(`tasks.md`); this guide covers the user-visible loop.

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
./build/PhysicsSimApp --help   # headless binary works, no GUI needed
```

## 1. Reset a running counter (Story 1)

```text
logic
add DFF --inputs 5
add DFF --inputs 5
ignore DFF1.CLK
ignore DFF2.CLK
poke DFF1.D=1
poke DFF2.D=1
step 1                 # both capture 1: Q=1 Q=1
poke DFF1.RST=1
poke DFF2.RST=1
peek                   # expect DFF1.Q=0 NQ=1, DFF2.Q=0 NQ=1, no step needed
poke DFF1.RST=release
poke DFF2.RST=release
step 1                 # counts from zero again
```

Expected: `peek` after assert shows `Q=0` on both flops before any
further `step`; the following `step 1` advances from the reset state.

## 2. Preset a seed (Story 2)

```text
logic
add DFF --inputs 5
ignore DFF1.CLK
poke DFF1.SET=1
peek                   # expect DFF1.Q=1 NQ=0 with D still 0, no step
poke DFF1.SET=release
step 1                 # D=0 captured: expect Q back to 0
```

Expected: preset appears with zero advances and does not latch after
release.

## 3. Stall one stage (Story 3)

```text
logic
add DFF --inputs 5
add DFF --inputs 5
ignore DFF1.CLK
ignore DFF2.CLK
poke DFF1.EN=0
poke DFF1.D=1
poke DFF2.D=1
step 3                 # DFF1 holds Q=0; DFF2 captures Q=1
peek                   # expect DFF1.Q=0, DFF2.Q=1
```

Expected: held flop reports no-change lines across all 3 edges; sibling
captures normally.

## 4. Priority check (Story 4)

```text
logic
add DFF --inputs 5
ignore DFF1.CLK
poke DFF1.RST=1
poke DFF1.SET=1
peek                   # expect Q=0 (reset wins)
poke DFF1.RST=release
peek                   # expect Q=1 (set now unopposed)
```

Expected: simultaneous assertion reads `Q=0`; releasing reset flips the
same flop to `Q=1` with no clock advance.

## 5. Zero-migration replay (SC-004)

Replay any pre-feature script (D-only netlist, no new pins) and diff
against its pinned output — outputs must be byte-identical.

## Validation checklist

- [x] Reset forces Q=0 with no `step`, holds while asserted (scenario 1)
- [x] Preset forces Q=1 with no `step`, releases cleanly (scenario 2)
- [x] `EN=0` holds across 3 edges while sibling captures (scenario 3)
- [x] RST+SET reads Q=0 everywhere (REPL, script, pipe) (scenario 4)
- [x] Old D-only scripts replay byte-identically (scenario 5)

Validated 2026-10-31 headlessly (pipe spelling; scenarios 1–4 also
covered by `test_logic_seq_controls` Shell goldens, scenario 5 by the
unchanged pre-existing suites).
