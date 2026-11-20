# Quickstart: Propagation-Delay Annotation Verbs (013)

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

**Process note**: the logic workspace is process-wide. Multi-verb flows
below run as one process — piped session lines into `PhysicsSimApp
logic` (each `logic` line first enters the session, then the verbs
run in-session until `exit`). Single-verb one-shots
(`./build/PhysicsSimApp logic <verb> ...`) work only for the one
command.

## Scenario 1 — Annotate a gate and read it back

```sh
printf 'add AND\nadd NOT\nwire AND1.Y NOT1.1\nset-delay AND1 1.5ns\nset-delay NOT1 250\nlist\nexit\n' | ./build/PhysicsSimApp logic
```

**Expected**: both annotations succeed with canonical echoes
(`set AND1 delay=1500ps`, `set NOT1 delay=250ps` — the `1.5ns` token
lands as exactly 1500 whole picoseconds, no rounding anywhere); the
gate lines in `list` carry `delay=1500ps` / `delay=250ps`; every other
line of `list` is byte-identical to a pre-feature listing of the same
netlist. Re-annotating the same gate replaces the value (last write
wins): follow with `set-delay AND1 2ns` and `list` shows
`delay=2000ps`.

## Scenario 2 — Evaluation is untouched (inertness)

```sh
printf 'add AND\nadd NOT\nwire AND1.Y NOT1.1\nsolve --in 11\nset-delay AND1 1.5ns\nset-delay NOT1 42\nsolve --in 11\ntruth\nexit\n' | ./build/PhysicsSimApp logic
```

**Expected**: the two `solve` outputs are identical line for line, and
`truth` matches the un-annotated truth table — annotation is stored,
never interpreted (the zero-delay rule stands; honoring delays is the
named follow-on, docs/logic.md §12).

## Scenario 3 — Loud refusals, workspace untouched

```sh
printf 'add AND\nadd NOT\nset-delay AND1 -1\nset-delay AND1 1.5ps\nset-delay AND1 6000h\nset-delay AND1 1500V\nset-delay AND1 1e3ns\nset-delay AND9 5ns\nset-delay AND1\nunset-delay NOT1\nlist\nexit\n' | ./build/PhysicsSimApp logic
```

**Expected**: every failing verb exits 2 with a specific message —
non-negative, sub-picosecond (`1.5ps` is not whole ps), field overflow
(`6000h` exceeds the 64-bit ps field), unknown/cross-dimension suffix
(`1500V`), grammar (`1e3ns`), unknown gate (`AND9`), missing operand,
and `unset-delay` on an un-annotated gate ("no delay set on NOT1").
The final `list` shows AND1 with no annotation — no refusal changed
any state.

## Scenario 4 — Clear lifecycle and the zero equivalence

```sh
printf 'add AND\nset-delay AND1 1500\nlist\nunset-delay AND1\nlist\nset-delay AND1 0\nlist\nexit\n' | ./build/PhysicsSimApp logic
```

**Expected**: `list` shows `delay=1500ps`, then clean after
`unset-delay` (`cleared AND1 delay`), and `set-delay AND1 0` leaves it
clean — zero *is* the un-annotated state, so the two paths are
indistinguishable in every flow.

## Scenario 5 — Save / reload round-trip (schema already carried it)

```sh
printf 'add AND\nset-delay AND1 1.5ns\nsave /tmp/opencode/delay_demo.yaml\nexit\n' | ./build/PhysicsSimApp logic
printf 'load /tmp/opencode/delay_demo.yaml\nlist\nsolve --in 11\nexit\n' | ./build/PhysicsSimApp logic
./build/PhysicsSimApp logic load examples/logic/tieoff.yaml
```

**Expected**: the saved file's AND1 row reads `delay_ps: 1500` (the
canonical row shape — no new schema); the reload restores the
annotation (`list` shows `delay=1500ps`) and replays identically; the
pre-feature `tieoff.yaml` example — which already carries stored
delays (`delay_ps: 1500`, `delay_ps: 250`) — loads and solves exactly
as before the feature.

## Scenario 6 — Optimizer: preserved in place, reset reported at O2

```sh
printf 'add AND\nadd NOT\nwire AND1.Y NOT1.1\nset-delay AND1 1.5ns\noptimize --level O0\nlist\noptimize --level O2\nlist\nexit\n' | ./build/PhysicsSimApp logic
```

**Expected**: O0 (and O1) keep `delay=1500ps` in place with no extra
report line; the O2 run resets the annotation per its documented
contract and prints the conditional note naming the reset (`reset the
propagation delay on 1 gate(s)`) — the resolution results never change
(solve outputs identical throughout), and a zero-annotation O2 run
prints no note at all.

## Scenario 7 — Test suite

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

**Expected**: all tests green, including the new `test_logic_delay`
binary (value goldens hand-derived — `1.5ns` → 1500, `1.5h` →
5,400,000,000,000,000, u64-max bare accepted, `1.5ps` refused — never
the implementation's own readback; verb lifecycle; inertness;
optimizer preserve/reset; save/load round-trip; removal clears) and
the synced registry pins (`LogicRegistry` size 27, `want_s` gains
`set-delay`, `want_u` gains `unset-delay`).

## Failure signatures worth knowing

| Symptom | Meaning |
|---|---|
| `set-delay` refused with "not a whole number of picoseconds" | The value has sub-ps precision (`1.5ps`) — the field stores whole ps only; nothing was rounded |
| `set-delay` refused with "exceeds the 64-bit picosecond field" | Value overflows u64 ps (≈585 years); no silent clamp exists |
| `set-delay` refused on `1.5` (bare) | Bare form is integer picoseconds; write `1500` or `1.5ns` |
| `unset-delay` refused with "no delay set" | The gate is un-annotated — lifecycle refusal, mirror of `unpull` |
| O2 report prints the delay-reset note | Expected: O2 reshapes the network and its documented reset ran; O0/O1 preserve annotations in place |
| `list` shows no `delay=` on an annotated gate | The value is zero — zero is the un-annotated state; re-annotate and re-list |
| Annotations vanish after `logic load` of an old file | They were never in that file — pre-feature files store `delay_ps: 0`; the loader never invents values |
