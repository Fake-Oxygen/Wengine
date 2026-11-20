# Quickstart: Enable-Gated Buffer plus Per-Net Pulls (012)

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

## Scenario 1 — A driver that disconnects

```sh
./build/PhysicsSimApp logic add TBUF
./build/PhysicsSimApp logic add AND --inputs 2
./build/PhysicsSimApp logic add AND --inputs 2
./build/PhysicsSimApp logic wire AND1.Y TBUF1.1
./build/PhysicsSimApp logic wire AND2.Y TBUF1.2
./build/PhysicsSimApp logic wire TBUF1.Y NOT1.1     # needs a NOT1: add NOT first
./build/PhysicsSimApp logic solve --in 111
./build/PhysicsSimApp logic solve --in 110
```

**Expected**: with enable high (`--in 111` → AND2 drives high) the
bus carries AND1's data through the buffer; with enable low
(`--in 110`) the buffer releases and the driven net floats to
high-Z — `solve` fails loudly at the consumer (state/timing
untouched), exactly as an unwired driver would. Toggling only the
enable between two solves changes nothing but the buffer's
contribution.

## Scenario 2 — The idle level (pull-up / pull-down / none)

```sh
./build/PhysicsSimApp logic pull NOT1.1 up
./build/PhysicsSimApp logic solve --in 110
./build/PhysicsSimApp logic pull NOT1.1 down
./build/PhysicsSimApp logic solve --in 110
./build/PhysicsSimApp logic unpull NOT1.1
./build/PhysicsSimApp logic solve --in 110
```

**Expected**: parked bus (buffer disabled) reads high under
pull-up, low under pull-down, and fails loudly (Z at the consumer)
with no pull. Enabling the buffer drives its level under every pull
setting — a driven net ignores the pull. `list` shows the pull
annotation while set, and its absence after `unpull`.

## Scenario 3 — Gated contention still contends

```sh
./build/PhysicsSimApp logic wire AND1.Y NOT1.1 --shared   # second driver, marked
./build/PhysicsSimApp logic solve --in 101                # buffer enabled, drivers disagree
```

**Expected**: the gated bus with two disagreeing active drivers
resolves to unknown; the per-net contention count increments (visible
in the status line); the level consumer rejects X loudly. A pull on
the same net changes nothing about the fight (still X, still counted).

## Scenario 4 — Loud seams (never silent)

```sh
./build/PhysicsSimApp logic solve --in 110     # enable dangling? wire it first, then:
./build/PhysicsSimApp logic unwire TBUF1.2
./build/PhysicsSimApp logic solve --in 11
./build/PhysicsSimApp logic pull TBUF1.1 up    # wired input: legal
./build/PhysicsSimApp logic tie TBUF1.1 AND1.1 # or any tied/pulled mix
```

**Expected**: solving with a dangling enable fails loudly (no silent
default on/off); setting a pull on a tied input (or tying a pulled
input) refuses loudly with the netlist untouched. The refusal
messages name the conflict (`tied`, `ignored`, `no pull set`).

## Scenario 5 — Save / reload round-trip

```sh
./build/PhysicsSimApp logic save /tmp/opencode/buffer_bus.yaml
./build/PhysicsSimApp logic load /tmp/opencode/buffer_bus.yaml
./build/PhysicsSimApp logic list
```

**Expected**: the file carries `tbuf` gate rows and the `pulls:`
section (see workspace-yaml.md); reload restores kinds, wirings,
marks, attachments, and pulls; replays produce identical outputs. A
pre-012 file (no `pulls:`, no `tbuf`) loads byte-identically to
before the feature.

## Scenario 6 — Optimizer soundness and truth refusal

```sh
./build/PhysicsSimApp logic optimize --level O0   # buffer netlist
./build/PhysicsSimApp logic optimize --level O2   # buffer netlist
./build/PhysicsSimApp logic truth                 # buffer netlist
```

**Expected**: O0/O1 succeed with buffer gates frozen and pulled
inputs skipped (resolution results unchanged;
optimize-of-optimize idempotent); O2 refuses loudly naming buffers /
pulls, workspace untouched; `truth` refuses loudly (four-value path
needed) while the same netlist solves scalar. A pull-only netlist
(no buffers) streams `truth` unchanged.

## Scenario 7 — Test suite and benchmarks

```sh
cmake --build build
ctest --test-dir build --output-on-failure
./build-bench/bench_logic 8 8 2000    # AFTER: ns/gate-word within noise
```

**Expected**: all tests green, including the new
`test_logic_buffer` binary (kind table → eval truth table → netlist
seam → verbs → YAML round-trip, reference-anchored) and the synced
registry pins (`LogicRegistry` size 25); `bench_logic` within
run-to-run noise of the recorded baseline (if measurable, apply the
research D11 contingency and re-measure).

## Failure signatures worth knowing

| Symptom | Meaning |
|---|---|
| `solve` prints `Z` on an output | A dangling disabled-buffer output — the net is genuinely undriven; terminate it with a pull or drive it |
| `truth` fails while `solve` works | Expected: word path refuses live-buffer netlists; pull-only netlists stream fine |
| O2 fails on a buffer/pull netlist | Expected: loud refusal per optimizer policy; O0/O1 available |
| `pull` refused on an input | The input is tied or ignored (untie/use first), or the pin is an output — check `logic list` |
| Second wire refused on a pulled net | Pulls are not marks: `wire --shared` (mark) is still required for a second driver — the pull only names the idle level |
