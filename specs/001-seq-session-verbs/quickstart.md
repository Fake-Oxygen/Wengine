# Quickstart: Sequential Session Verbs

**Feature**: `specs/001-seq-session-verbs/spec.md` | **Date**: 2026-10-31

Validates the feature end-to-end from a shell with no GUI. Each scenario
links its contract ([step](./contracts/step.md), [poke](./contracts/poke.md),
[peek](./contracts/peek.md)) and names the expected outcome. Full test
suites (not pasted here) live in `tasks.md` / implementation phase.
Named pins are canonical (`DFF1.D`, `DFF1.CLK`, `DFF1.Q`, `DFF1.NQ`);
numeric aliases (`DFF1.1`, `DFF1.Y`) behave identically.

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
./build/PhysicsSimApp --help   # lists the `logic` command
```

## Scenario 1 — Toggle flop: poke → step → peek (contracts: poke, step, peek)

A DFF with its output inverted back to D toggles every edge; CLK ignored
(edge is the call).

```sh
printf '%s\n' \
  'logic' \
  'add DFF' \
  'add NOT' \
  'wire DFF1.Q NOT1.1' \
  'wire NOT1.Y DFF1.D' \
  'ignore DFF1.CLK' \
  'peek' \
  'step 1' \
  'peek' \
  'step 2' \
  'peek DFF1' \
  'back' | ./build/PhysicsSimApp
```

Expected: first `peek` reports `DFF1.Q=0 NQ=1 (serial 0)` (power-on Q=0,
NQ=1); after `step 1`, `clk: cycle 1 (1 edges)` plus `DFF1.Q: 0->1
(NQ 1->0)`; after `step 2`, Q toggles per edge; final `peek DFF1` shows
`DFF1.Q=1 NQ=0 (serial 0)`. Re-running the whole pipe prints
byte-identical output (determinism, FR-008). Replacing `DFF1.Q` with
`DFF1.Y` and `DFF1.D` with `DFF1.1` in the script prints identical
results (numeric aliases).

## Scenario 2 — Poke stimulus + NQ wiring (contracts: poke, step, peek)

Drive D, hold it across advances, observe NQ through a wired load, then
release.

```sh
printf '%s\n' \
  'logic' \
  'add DFF' \
  'add AND' \
  'ignore DFF1.CLK' \
  'wire DFF1.NQ AND1.1' \
  'wire DFF1.Q AND1.2' \
  'poke DFF1.D=1' \
  'poke' \
  'step 1' \
  'peek' \
  'solve' \
  'poke DFF1.D=release' \
  'poke' \
  'back' | ./build/PhysicsSimApp
```

Expected: map lists `DFF1.D=1`; `step 1` latches Q=1; `peek` shows
`DFF1.Q=1 NQ=0`; `solve` shows the AND of Q and NQ as 0 (NQ is live NOT
Q through the wired load); after release the map reports
`poke map: empty`. `poke AND1.D=1` prints
`poke: bad block input pin 'AND1.D'` with exit 2 and changes nothing
(named pins rejected on combinational gates).

## Scenario 3 — Error contract + one-shot parity (contracts: all)

```sh
./build/PhysicsSimApp logic step -1; echo "exit=$?"
./build/PhysicsSimApp logic step nosuchclk 3; echo "exit=$?"
./build/PhysicsSimApp logic peek NOPE; echo "exit=$?"
./build/PhysicsSimApp logic wire DFF1.NQ AND1.1 2>/dev/null; echo "exit=$?"
./build/PhysicsSimApp logic step --help | head -5
```

Expected: usage errors name the bad value (`want N >= 0`, unknown
clock, unknown gate), each with exit 2 and no state change; `--help`
prints the usage block. The same verbs typed inside the `[logic]>`
session produce identical state transitions for identical input lines
(FR-006 surface parity).

## Scenario 4 — Determinism replay gate (acceptance for FR-008)

```sh
printf '%s\n' 'logic' 'add DFF' 'add NOT' 'wire DFF1.Q NOT1.1' \
  'wire NOT1.Y DFF1.D' 'ignore DFF1.CLK' 'step 100' 'peek' 'back' \
  | ./build/PhysicsSimApp > /tmp/seq_run1.txt
printf '%s\n' 'logic' 'add DFF' 'add NOT' 'wire DFF1.Q NOT1.1' \
  'wire NOT1.Y DFF1.D' 'ignore DFF1.CLK' 'step 100' 'peek' 'back' \
  | ./build/PhysicsSimApp > /tmp/seq_run2.txt
cmp /tmp/seq_run1.txt /tmp/seq_run2.txt && echo REPLAY-IDENTICAL
```

Expected: `cmp` silent, `REPLAY-IDENTICAL`; final cycle line reads
`clk: cycle 100 (100 edges)` in both files.
