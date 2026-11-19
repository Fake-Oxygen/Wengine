# Quickstart: Allowlisted Multi-Driver Nets (011)

Runnable validation scenarios proving the feature end-to-end. Each
scenario is independently executable; expected outputs are behavioral
(contracts: [engine-seam](contracts/engine-seam.md),
[cli-verbs](contracts/cli-verbs.md),
[workspace-yaml](contracts/workspace-yaml.md); storage:
[data-model](data-model.md)).

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure   # baseline green
```

Baseline for the fast-path claim (constitution IV — measured only):

```sh
cmake --preset bench && cmake --build build-bench -j$(nproc)
./build-bench/bench_logic 8 8 2000    # record ns/gate-word BEFORE the change
```

## Scenario 1 — The opt-in gate (never silent)

```sh
printf 'logic add AND --inputs 2\nlogic add AND --inputs 2\nlogic add OR --inputs 2\n' \
  '; ' 2>/dev/null
./build/PhysicsSimApp logic add AND --inputs 2
./build/PhysicsSimApp logic add AND --inputs 2
./build/PhysicsSimApp logic add OR --inputs 2
./build/PhysicsSimApp logic wire AND1.Y OR1.1
./build/PhysicsSimApp logic wire AND2.Y OR1.1
```

**Expected**: the second `wire` refuses loudly — exit 2, "input
already wired (unwire it first)" — and the workspace is unchanged
(`logic list` shows one driver on `OR1.1`). This is today's behavior,
byte-identical: the default single-driver rule never weakens.

## Scenario 2 — Wired-OR consensus (the point of the feature)

Continue the session above (REPL) or one-shot with `--shared`:

```sh
./build/PhysicsSimApp logic wire AND2.Y OR1.1 --shared
./build/PhysicsSimApp logic list          # OR1.1 marked, 2 drivers
./build/PhysicsSimApp logic solve --in 11
./build/PhysicsSimApp logic solve --in 00
```

**Expected**: the shared wire lands (success line names the shared
state, e.g. `wired AND2.Y -> OR1.1 (shared: 2 drivers)`); with both
drivers agreeing (`--in 11` → both ANDs drive high; `--in 00` → both
low) the shared net resolves to the agreed level and `OR1.Y` evaluates
normally — the wired-OR/wired-AND case with no contention line beyond
`total 0`.

## Scenario 3 — Contention: X, counted, visible

```sh
./build/PhysicsSimApp logic solve --in 10
```

**Expected**: drivers disagree (one AND high, one low) → the shared
net resolves to unknown; `solve` fails loudly (the consumer rejects X,
state/timing untouched) **and** the status carries the contention
count, e.g. `contention: OR1.1=1 (total 1)`. Repeating the solve
increments per resolution. `logic reset` clears the counts; the saved
workspace is byte-identical with or without contention history
(FR-009).

## Scenario 4 — Unshare lifecycle

```sh
./build/PhysicsSimApp logic unshare OR1.1        # 2 drivers attached
./build/PhysicsSimApp logic unwire AND2.Y OR1.1
./build/PhysicsSimApp logic unshare OR1.1        # now 1 driver
./build/PhysicsSimApp logic wire AND2.Y OR1.1    # occupied again: refuses
```

**Expected**: `unshare` with two attachments refuses loudly (exit 2,
"unwire extras first"); after unwiring down to one, `unshare`
succeeds; the next plain wire hits the single-driver refusal again —
the mark was the only thing lifting it.

## Scenario 5 — Save / reload round-trip

```sh
./build/PhysicsSimApp logic save /tmp/opencode/bus.yaml     # while shared
./build/PhysicsSimApp logic load /tmp/opencode/bus.yaml
./build/PhysicsSimApp logic list
```

**Expected**: the file carries the `shared:` section and repeated
wire rows (see workspace-yaml.md); reload restores marks and both
attachments; replays produce identical outputs. A pre-011 file (no
`shared:` section, no repeated wires) loads byte-identically to
before the feature; a duplicate `to:` row without a mark still
refuses with today's message.

## Scenario 6 — Optimizer soundness and truth refusal

```sh
./build/PhysicsSimApp logic optimize --level O0   # shared netlist
./build/PhysicsSimApp logic optimize --level O2   # shared netlist
./build/PhysicsSimApp logic truth                 # shared netlist, 2+ drivers
```

**Expected**: O0/O1 succeed with the shared-feeding gates frozen
(resolution results unchanged; optimize-of-optimize idempotent); O2
refuses loudly naming shared nets, workspace untouched; `truth`
refuses loudly (four-value path needed) while the same netlist
solves scalar.

## Scenario 7 — Test suite and benchmarks

```sh
cmake --build build
ctest --test-dir build --output-on-failure
./build-bench/bench_logic 8 8 2000    # AFTER: ns/gate-word within noise
```

**Expected**: all tests green, including the new
`test_logic_shared` binary (engine seam → verbs → YAML round-trip,
reference-anchored span table) and the synced registry pins
(`LogicRegistry` size 23); `bench_logic` within run-to-run noise of
the recorded baseline (if measurable, apply the research D11
contingency and re-measure).

## Failure signatures worth knowing

| Symptom | Meaning |
|---|---|
| Second wire refused on a net you marked | The mark was on a different input index — check `logic list` |
| `contention:` line absent on a shared netlist | Marks exist but solve ran on a netlist where none are marked — verify with `list` |
| `truth` fails while `solve` works | Expected: word path refuses ≥2-attachment shared nets |
| O2 fails on a shared netlist | Expected: loud refusal per optimizer policy; O0/O1 available |
