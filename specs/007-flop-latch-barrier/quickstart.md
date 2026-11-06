# Quickstart: Validating the Flop-Latch Opaque Barrier

**Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md) | **Contract**: [contracts/optimize-sequential.md](./contracts/optimize-sequential.md)

How to prove the feature works end-to-end. All commands run from the repo
root with an out-of-source build. No new tooling required.

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
```

Baseline (must be green before and after the change):

```sh
ctest --test-dir build --output-on-failure
```

## 1. Unit/integration validation (primary gate)

```sh
ctest --test-dir build -R 'test_logic|test_cli_args' --output-on-failure
```

Expected: pass. Coverage added by this feature (in `tests/test_logic.cpp`
unless noted):

- O0/O1 over a DFF counter + latch glue: sequential set bit-identical,
  run parity over 10 fixed steps (`seq_step` reference).
- Closed state cluster (free-running counter, no block outputs): survives
  DCE at all levels (the former wholesale-deletion case).
- Unobserved flop (no fanout): survives at all levels.
- Acyclic pipeline (flop → comb → flop): `exact_simplify` skips the net;
  no constant-collapse of state, no tied-off flop inputs.
- `NOT(Q)` consumer + `BUF`-feeding-flop: O1 leaves the barrier pins
  untouched (no fusion/bypass through state).
- NQ consumer + full-control DFF (async pins): polarity, pin count, and
  control wiring preserved; O2 carries `driver_inv` through the rebuild.
- O2 over a sequential net: exits success (former loud failure), glue
  converted, sequential gates verbatim, run parity holds.
- Idempotence: `optimize(optimize(x)) == optimize(x)` on a sequential net.
- `tests/test_cli_args.cpp`: `logic optimize --level O0|O1|O2` over a
  sequential workspace exits 0 with flat sequential counts.

## 2. Interactive spot-check (REPL)

```sh
./build/PhysicsSimApp
```

Suggested session (DFF counter + redundant glue):

```text
logic add DFF --inputs 2
logic add AND
<wire counter + glue>
logic optimize --level O0
# expect: success, sequential count flat, total possibly reduced
logic optimize --level O2 --to nand
# expect: success (previously: loud failure), state elements verbatim
```

## 3. Headless run-parity check

```sh
./build/PhysicsSimApp sim run --steps 10 --particles 1
```

For sequential nets, compare the per-step stored-value readback before and
after `logic optimize` at each level (see contract § Equivalence): every
step identical, or the feature failed.

## 4. Docs check

- `docs/logic.md` §6 describes barrier semantics and O2 acceptance of
  sequential nets.
- README test list accounts for the new barrier tests.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| O2 still fails on sequential nets | Verbatim-emit branch missed a sequential kind (check all five + full-control DFF shape). |
| Flop input driver changed at O0/O1 | A pass wrote a sequential `drivers[]` entry — the freeze (research R2) was bypassed; check `rewire_users` call sites. |
| Acyclic sequential net collapsed to constants | `exact_simplify_pass` early-out missing or ordered after mutation. |
| Closed counter cluster disappears | DCE roots missing live sequential gates (research R1). |
| NQ consumer reads wrong polarity after O2 | `driver_inv` not copied per pin in the verbatim path (research R0). |
