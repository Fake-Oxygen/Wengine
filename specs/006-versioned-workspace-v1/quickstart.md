# Quickstart: Versioned Workspace v1 Validation

**Feature**: specs/006-versioned-workspace-v1/spec.md | **Date**: 2026-11-04
(Run guide only — no implementation. Proves the feature end-to-end. Assumes a built tree.)

## Prerequisites

```sh
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
```

## 1. Round-trip: save → reload → byte-identical re-save (US1)

```sh
printf 'logic add DFF\nlogic add DFF\nlogic wire DFF1.Q DFF2.D\nlogic save /tmp/opencode/c1.yaml\nlogic load /tmp/opencode/c1.yaml\nlogic save /tmp/opencode/c2.yaml\nexit\n' | ./build/PhysicsSimApp
cmp /tmp/opencode/c1.yaml /tmp/opencode/c2.yaml && echo ROUNDTRIP-OK
grep -c '^version: 1$' /tmp/opencode/c1.yaml   # want 1
grep -c 'clocks:' /tmp/opencode/c1.yaml        # want 1
```

Expect: `loaded …` succeeds, `cmp` silent (identical), both markers present.

## 2. Inits take effect on power-on and survive stepping (US2)

```sh
printf 'logic load examples/logic/two_clock_counter.yaml\nlogic peek\nlogic step 3\nlogic peek\nexit\n' | ./build/PhysicsSimApp
```

Expect: first `peek` shows the file's stored (mixed, nonzero) Q values before any advance; after `step 3` the sequence matches the same 3-step run performed pre-save. Then:

```sh
./build/PhysicsSimApp <<'EOF'
logic load examples/logic/two_clock_counter.yaml
logic save /tmp/opencode/live.yaml
exit
EOF
```

saves post-step Q as the new inits (save captures live state).

## 3. Named domains survive reload; selective stepping identical (US3)

```sh
printf 'logic load examples/logic/two_clock_counter.yaml\nlogic step BUF1.Y 3\nlogic peek\nexit\n' | ./build/PhysicsSimApp
```

Expect: domain A advanced 3 captures, domain B held — identical to pre-save behavior. Repeat with no domain (all domains advance).

## 4. Legacy files refused loudly, workspace kept (US4)

```sh
printf 'version: 2\ngates:\nwires:\nties:\nmeta:\n  tool: x\n' > /tmp/opencode/legacy2.yaml
printf 'logic add AND\nlogic load /tmp/opencode/legacy2.yaml\nlogic list\nexit\n' | ./build/PhysicsSimApp
```

Expect: load fails naming version 2 and retirement; `list` still shows the AND gate (workspace untouched). Repeat with a legacy `version: 1` (no `clocks:`) file — refused as legacy, not misloaded.

## 5. Example library + full suite (US5, FR-013–015)

```sh
for f in examples/logic/*.yaml; do
  printf "logic load $f\nlogic list\nexit\n" | ./build/PhysicsSimApp > /dev/null || echo "FAIL: $f"
done
echo EXAMPLES-OK
ctest --test-dir build --output-on-failure
```

Expect: all eight files (five converted + three new) load; every shipped file carries `version: 1`; full `ctest` green with zero warnings.

## 6. Engine-path parity (FR-008, R8)

```sh
./build/PhysicsSimApp sim run --solver logic --seq-file examples/logic/two_clock_counter.yaml --steps 3 --emit-csv /tmp/opencode/eng.csv
```

Expect: per-step stored values evolve from the file's inits identically to the session `step 3` trajectory from §2.
