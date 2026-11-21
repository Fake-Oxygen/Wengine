# Quickstart: Validating the Delay-Honoring Evaluator (014)

Runnable validation scenarios proving the feature end-to-end. Each
maps to a spec success criterion; exact grammars live in
[contracts/cli-verbs.md](contracts/cli-verbs.md), semantics in
[contracts/timing-semantics.md](contracts/timing-semantics.md),
entities in [data-model.md](data-model.md).

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
```

Engine-only build (`wengine_logic` header-only; no optional deps
needed). The `logic` sub-shell is reachable headless:
`./build/PhysicsSimApp logic <verb> ...` (one-shot) or bare
`logic` in the REPL (session; identical results by construction).

## SC-001 — See the hidden pulse (glitch)

```sh
./build/PhysicsSimApp logic load examples/logic/glitch_chain.yaml
./build/PhysicsSimApp logic solve --in 1
./build/PhysicsSimApp logic solve --in 1 --with-delays
```

Expected:

- Flag-off: settled values only, byte-identical to pre-feature
  (no transient, no new text).
- Flag-on: the `timing: unit 1ps, <K> annotated` header, event
  lines, and the pulse — the gate output going `0->1` then
  `1->0` with the window width equal to the hand-derived
  path-delay difference (3 inverters → 3 ps, placement at the
  gate's own delay; annotated variants scale by the annotations).
- Settled values identical in both invocations (combinational
  parity, FR-005).

## SC-003 — See the race (sequential window)

```sh
./build/PhysicsSimApp logic load examples/logic/race_window.yaml
./build/PhysicsSimApp logic poke DFF1.D=1
./build/PhysicsSimApp logic step --with-delays
./build/PhysicsSimApp logic step            # compare
./build/PhysicsSimApp logic peek
```

Expected:

- Flag-on: the edge's window — `DFF1.Q` (and `NQ`, same
  timestamp) at its annotated clock-to-Q, then the downstream
  D-arrival at the hand-derived offset (clock-to-Q + path
  delays); `settled in <D>ps`.
- Flag-off: the ordinary step report; `peek` identical after
  either invocation (trajectory parity, FR-006).

## SC-002 — The default never moves

```sh
./build/PhysicsSimApp logic solve --in 1 > /tmp/a.txt   # post-feature
git stash && cmake --build build && ./build/PhysicsSimApp logic \
  solve --in 1 > /tmp/b.txt && git stash pop && cmake --build build
diff /tmp/a.txt /tmp/b.txt                                # empty
ctest --test-dir build -R bench_logic --output-on-failure  # unchanged
```

Expected: every flag-off flow byte-identical; the bench suite
measures unchanged (the fast path gained nothing). The full
golden suite covers this without the stash dance —
`ctest --test-dir build --output-on-failure` passes with zero
flag-aware changes to any pre-feature test.

## SC-004 — Determinism and one vocabulary

```sh
./build/PhysicsSimApp logic solve --in 1 --with-delays > /tmp/t1.txt
./build/PhysicsSimApp logic solve --in 1 --with-delays > /tmp/t2.txt
diff /tmp/t1.txt /tmp/t2.txt                              # empty
./build/PhysicsSimApp logic solve --in 1 --with-del 2>&1 | tail -1
                                                          # usage error, exit 2
```

Expected: repeated invocations byte-identical; a misspelled flag
refuses loudly with the workspace untouched; completion offers the
flag (pinned by `test_cli_args.cpp` vectors, registry size 27
unchanged).

## Truth settle times (US5)

```sh
./build/PhysicsSimApp logic truth --with-delays
```

Expected: identical row values plus the `settle` column — each
row's drain time equals its hand-derived longest activated path;
flag-off truth output byte-identical.

## Full verification

```sh
ctest --test-dir build --output-on-failure     # includes test_logic_timing
./build/PhysicsSimApp logic load examples/logic/glitch_chain.yaml   # replay pins
```

`test_logic_timing` covers the levels beyond these demos:
1364-anchored 4-state windows, budget/overflow refusals (a
reconvergent annotated bomb), settled X/Z and S=R=1 refusals, and
the parity/determinism pins. Catalog audit:
`python3 docs/audit_catalogs.py` stays zero-missing/zero-ghost.
