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
  'wire NOT1.Y DFF1.D' \
  'wire DFF1.Q NOT1.1' \
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
(NQ 1->0)`; after `step 2`, `clk: cycle 3 (2 edges)` with net-effect
line `DFF1.Q: 1 (no change)` (1→0→1 across the two edges) and
`DFF1.NQ=0`; final `peek DFF1` shows `DFF1.Q=1 NQ=0 (serial 0)`.
Re-running the whole pipe prints byte-identical output (determinism,
FR-008). Replacing `DFF1.Q` with `DFF1.Y` and `DFF1.D` with `DFF1.1`
in the script prints identical results (numeric aliases).

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
  'poke DFF1.D=release' \
  'poke' \
  'poke AND1.D=1' \
  'back' | ./build/PhysicsSimApp
```

Expected: map lists `DFF1.D=1`; `step 1` latches Q=1 (`DFF1.Q: 0->1`)
and the settled block output `AND1.Y=0` proves the wired NQ load reads
live NOT Q (Q=1, NQ=0); `peek` shows `DFF1.Q=1 NQ=0`; after release the
map reports `poke map: empty`. `poke AND1.D=1` prints
`poke: bad block input pin 'AND1.D'` with exit 2 and changes nothing
(named pins rejected on combinational gates; `solve` stays out of this
loop — it evaluates combinationally and never reads stepped Q).

## Scenario 3 — Error contract + one-shot parity (contracts: all)

```sh
printf '%s\n' \
  'logic' \
  'add DFF' \
  'ignore DFF1.CLK' \
  'ignore DFF1.D' \
  'step --cycles -1' \
  'step nosuchclk 3' \
  'peek NOPE' \
  'step 1' \
  'step --help' \
  'back' | ./build/PhysicsSimApp
echo "session exit=$?"
./build/PhysicsSimApp logic step; echo "one-shot exit=$?"
```

Expected: `step --cycles -1` prints `step: want N >= 0`,
`step nosuchclk 3` prints `step: unknown clock 'nosuchclk'`, and
`peek NOPE` prints `peek: unknown gate 'NOPE'` — each with exit 2 and
no state change (the following `step 1` still reports cycle 1). `step`
with no flops at all (fresh one-shot workspace) is a runtime error
(exit 1): `step: no flops in workspace`. `--help` prints the usage
block. The same verbs typed inside the `[logic]>` session produce
identical state transitions for identical input lines (FR-006 surface
parity).

## Scenario 4 — Determinism replay gate (acceptance for FR-008)

```sh
printf '%s\n' 'logic' 'add DFF' 'add NOT' 'wire NOT1.Y DFF1.D' \
  'wire DFF1.Q NOT1.1' 'ignore DFF1.CLK' 'step 100' 'peek' 'back' \
  | ./build/PhysicsSimApp > /tmp/seq_run1.txt
printf '%s\n' 'logic' 'add DFF' 'add NOT' 'wire NOT1.Y DFF1.D' \
  'wire DFF1.Q NOT1.1' 'ignore DFF1.CLK' 'step 100' 'peek' 'back' \
  | ./build/PhysicsSimApp > /tmp/seq_run2.txt
cmp /tmp/seq_run1.txt /tmp/seq_run2.txt && echo REPLAY-IDENTICAL
```

Expected: `cmp` silent, `REPLAY-IDENTICAL`; final cycle line reads
`clk: cycle 100 (100 edges)` in both files.
