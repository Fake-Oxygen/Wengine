# Implementation Plan: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Branch**: `010-tristate-net-resolution` | **Date**: 2026-11-19 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/010-tristate-net-resolution/spec.md`

## Summary

Cash in the v1-reserved four-value slot: `resolve_net`
(include/wengine/logic/types.hpp:148) — the single funnel every input
resolves through — gains real resolution semantics for
`kHighZ`/`kUnknown`. Today it enforces exactly one `kLow`/`kHigh`
driver and refuses everything else; after this feature it is a total,
order-independent, allocation-free combination over any driver span:
high-Z drivers are transparent, any `kUnknown` driver dominates
low/high, active low+high contention resolves to `kUnknown`, active
consensus resolves to the agreed level, and an empty or all-Z span
resolves to `kHighZ`. The signature and every existing call site stay
unchanged (the docs/logic.md §12 promise: "replaces one function
body; call sites unchanged"); consumers keep rejecting X/Z loudly at
their own seams. The v1 placeholder-refusal assertions in
tests/test_logic.cpp are re-pinned to the new resolution table against
an IEEE 1164-anchored independent reference, and the shipped-contract
docs (types.hpp Doxygen, docs/logic.md §12, docs/api/logic.md, README
module table) are updated in the same change.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline compiler.

**Primary Dependencies**: `wengine_logic` INTERFACE library
(header-only, allocation-free, exception-free engine module); the
funnel lives in `include/wengine/logic/types.hpp`, a leaf beside the
`LogicValue`/`GateKind` vocabulary. Sole production caller today:
`block_interface.hpp:401` (span of one gate-output value). No new
dependencies; no CLI, netlist, serialization, or GUI surface changes.

**Storage**: N/A — pure `constexpr` function; no state, no I/O, no
schema changes (the YAML workspace and world checkpoints are
untouched; Q values stay 0/1 because consumers keep rejecting X/Z).

**Testing**: plain-`assert` binaries printing `test_x: OK`,
registered with `add_executable`/`add_test` and linked against
`wengine_test_assert`. Re-pin and extend the funnel tests in
`tests/test_logic.cpp` (links `wengine_core`); the full resolution
table is validated against an independently derived reference (IEEE
1164 wired resolution plus a hand-derived semilattice), never against
the implementation's own output.

**Target Platform**: Linux (GCC 13.3 baseline); engine header usable
wherever `wengine_core` is linked (CLI, GUI models, tests).

**Project Type**: high-performance C++20 physics/logic simulation
engine — this feature is an engine-module contract change.

**Performance Goals**: none new — the funnel is `constexpr`,
`noexcept`, single-pass O(n) over the span, no allocation, no
branching beyond value classification; the n==1 live path keeps the
same shape as v1. No benchmark introduced (no measured hot-path
regression is possible without new callers; per constitution IV no
optimization is claimed).

**Constraints**: zero-warning build (`-Wall -Wextra -Wpedantic
-Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`;
equivalent strictness expected in engine headers via consumers);
deterministic output for identical inputs (the resolution result
depends only on the driver-value multiset, never on order);
append-only value vocabulary (`kHighZ`=2, `kUnknown`=3 never
renumber); call sites unchanged; consumers' loud X/Z rejection
preserved.

**Scale/Scope**: one function body + its Doxygen contract, one test
region re-pinned/extended, three documentation surfaces updated
(types.hpp comments, docs/logic.md §12, docs/api/logic.md, README
module row). No new files, no new targets, no registry changes.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | How this plan satisfies it |
|---|---|---|
| I. Physical/Numerical Correctness | PASS | The four-value table is anchored to IEEE Std 1164 wired resolution and independently hand-derived as a semilattice (Z identity, X absorbing, 0∧1=X); tests assert exact enum results, no tolerances involved. Contention is represented, never silently picked (FR-005). |
| II. Determinism & Reproducibility | PASS | Resolution is pure and multiset-determined (FR-009): any permutation yields the identical result, so no evaluation-order, FP, RNG, or snapshot-timing surface exists. No serialization, CLI output, or snapshot change. |
| III. Independent-Reference Testing | PASS | Goldens come from IEEE 1164-1993 `resolution` + a hand-derived table, not from the implementation (FR-011, SC-001). Existing suite must keep passing outside the intentionally superseded placeholder (SC-002). |
| IV. Measured Performance Only | PASS | No optimization claimed; no new benchmark introduced; the function stays single-pass, allocation-free, `constexpr`/`noexcept`. |
| V. Simplicity, Minimal Scope, Dependency Discipline | PASS | Smallest coherent diff: one function body + docs + test re-pins. No new headers, TUs, targets, registries, or dependencies; `types.hpp` stays a leaf. Existing documented API shape preserved (bool + out-param, call sites unchanged per docs/logic.md §12). |
| Logic-domain constraints | PASS | `GateKind` untouched; `LogicValue` members never renumbered (append-only vocabulary); netlist single-driver rule, wire/unwire verbs, YAML workspace, and world serialization untouched; the optimizer and all consumers unchanged. |
| Development Workflow | PASS | Doxygen comments updated in the same change as the API semantics; docs/logic.md + docs/api/logic.md + README module row updated; full `ctest` run; zero warnings; no git actions. |

**Gate evaluation**: no violations — Phase 0 may proceed.

**Post-Phase-1 re-check (after research.md, data-model.md,
contracts/resolve-net.md, quickstart.md)**: still no violations. The
design stays inside the audited blast radius (research.md "Caller
inventory": one function body, one test region, four doc surfaces);
determinism is enforced by the multiset-determined semilattice
(research.md D1); goldens stay IEEE 1164-anchored (D5); no new
entities, targets, dependencies, or persisted state (data-model.md);
the contract pins the unchanged signature and append-only vocabulary
(contracts/resolve-net.md); the quickstart gates on the full suite,
consumer-rejection tests, and the docs API-coverage build.

## Project Structure

### Documentation (this feature)

```text
specs/010-tristate-net-resolution/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/
│   └── resolve-net.md   # Phase 1 output: the funnel's public API contract
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/types.hpp   # resolve_net body + LogicValue/funnel Doxygen (wengine_logic)
tests/test_logic.cpp              # funnel tests re-pinned + table extended (links wengine_core)
docs/logic.md                      # §12 value-system + drivers slots move to "shipped"
docs/api/logic.md                  # funnel contract section (API reference)
README.md                          # `logic` module table row (four-value resolution noted)
```

**Structure Decision**: no new directories or targets — the feature is
a contract change inside the existing `wengine_logic` INTERFACE
module, its existing test binary, and the existing documentation
surfaces. This matches the house rule that engine modules stay
header-only unless a concrete platform/library boundary demands a TU.

## Complexity Tracking

No constitution violations to justify — table intentionally empty.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — | — | — |
