# Quickstart: Sequential Nets Runner

**Feature**: `spec.md` | **Design**: `research.md`, `data-model.md`, `contracts/`

## Prerequisites

- Out-of-source build: `cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build`
- A sequential workspace, e.g. `examples/logic/<counter>.yaml` (any saved
  `logic` workspace with flops; see `docs/logic.md` §9 for runnable examples).

## 1. Run a counter for 4 fixed steps (US1)

```sh
./build/PhysicsSimApp sim run --solver logic --seq-file examples/logic/counter.yaml \
  --steps 4 --emit-csv /tmp/seq.csv
```

Expected: exit 0; readback shows the count sequence `00 → 01 → 10 → 11 → 00`;
re-running the identical command produces byte-identical `/tmp/seq.csv`.

## 2. Step one clock domain only (US2)

```sh
./build/PhysicsSimApp sim run --solver logic --seq-file examples/logic/two_clock.yaml \
  --seq-clocks BUF1.Y --steps 3 --emit-csv /tmp/seq_one.csv
```

Expected: exit 0; flops under `BUF1.Y` advanced 3 times, all other flops held.
`--seq-clocks nope` → exit 2 naming the unknown domain, no state change.

## 3. Refusal cases (US4)

```sh
./build/PhysicsSimApp sim run --solver logic --seq-file <sr_illegal.yaml> --steps 1
./build/PhysicsSimApp sim run --solver projectile --seq-file examples/logic/counter.yaml --steps 1
```

Expected: both exit 2 — the first names the illegal S=R=1 combination, the
second rejects `--seq-file` for a non-logic solver.

## 4. Verify (developer)

```sh
cmake --build build && ctest --test-dir build --output-on-failure -R 'logic_seq|run_config|solver_registry|frame_source|snapshot|sim'
```

Expected: all selected suites green; full `ctest` green before finishing.
See `tasks.md` (next command) for the implementation order.
