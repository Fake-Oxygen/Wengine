# Implementation Plan: Flop-Latch Opaque Barrier

**Branch**: `007-flop-latch-barrier` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/007-flop-latch-barrier/spec.md`

## Summary

Today's logic optimizer (`include/wengine/logic/optimize.hpp`, all levels
O0/O1/O2) is unsafe on sequential nets: `eval_gate` reads every flop/latch
output as `kLow`, so `exact_simplify_pass` builds a bogus truth table for
acyclic sequential nets and can collapse state elements into constants;
`dce_pass` roots only fanout-0 gates, so closed state clusters with no
block-output path are deleted; and the O2 fresh-build path (`emit_gate`)
loudly fails on any sequential gate. Several SEQ11 placeholder guards already
skip sequential kinds in classify/const-fold/fusion, but the passes above
reach through or delete state anyway.

The fix teaches every pass the same barrier contract: no pass reads through,
folds through, rewrites, or deletes a sequential gate (`DFF/DLATCH/JK/T/
SRLATCH` per `is_sequential_kind`), DCE roots every live state element, the
exact truth-table pass skips sequential nets entirely, and O2 carries state
elements through its fresh build verbatim. No new gate kinds, flags, commands,
file formats, or report fields. Per-gate kind dispatch only — no asymptotic
cost, no benchmark needed.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source builds only

**Primary Dependencies**: None new. Work is confined to header-only
`wengine/logic` (`optimize.hpp`, using `netlist.hpp`/`eval.hpp` vocabulary);
the `logic optimize` CLI verb (`src/cli/commands/logic/optimize.cpp`)
forwards to `optimize()` unchanged.

**Storage**: N/A (in-memory `Netlist`; YAML v1 persistence format unchanged,
no migration)

**Testing**: Plain-`assert` binaries via `ctest --output-on-failure`:
`tests/test_logic.cpp` (optimizer + run-parity tests) and
`tests/test_cli_args.cpp` (CLI success/report on sequential nets). New tests
join the existing targets (no new binaries, no registry changes); every
`test_*` target links `wengine_test_assert` per policy.

**Target Platform**: Linux (POSIX), reproducible presets
`dev` / `test-opt` / `bench`

**Project Type**: Header-only engine library + CLI (no new commands)

**Performance Goals**: No performance change. Barrier checks ride the
existing per-gate kind dispatch (O(1) per gate, no new passes, no added
iteration bounds). The existing optimizer test suite passing is the proof
(no benchmark required for a non-hot-path semantic fix).

**Constraints**: Zero-warning target (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor`); `noexcept`, allocation-free, header-only
`INTERFACE` modules preserved; determinism preserved (slot-order processing,
fresh per-kind counters, no FP/RNG/order changes); serialization untouched.

**Scale/Scope**: One header (`optimize.hpp`, ~2726 lines, surgical
per-pass edits) + 2 test files + `docs/logic.md` (§6) + README test-list
touch-up. 11 functional requirements, 0 new user-facing surfaces.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Check | Result |
|-----------|-------|--------|
| I. Physical/numerical correctness | Optimized sequential nets must run step-for-step identically to originals (FR-011); goldens come from fixed-step runs of hand-built nets, never from the optimizer output. Async priority, transparent/frozen rules, illegal-combination refusal verified through `seq_step`, not rederived. | PASS — run-parity tests required (see contracts) |
| II. Determinism | Slot-order sweeps unchanged; O2 verbatim emit in slot order with `add_gate` fresh counters; no FP/RNG/accumulation-order changes; serialization untouched. Idempotence (SC-005) asserted by test. | PASS |
| III. Independent-reference testing | Multi-boundary: gate-set compare (optimizer output vs original) → CLI report (before/after counts) → fixed-step run parity (original vs optimized). Tests join existing `test_logic` / `test_cli_args` targets (`test_` prefix keeps assert policy). No registry-size asserts affected (no new commands/kinds). | PASS |
| IV. Measured performance | No performance claim made; assumption of negligible impact recorded in spec. No new hot-path cost by construction (kind-dispatch guards only). No benchmark needed. | PASS |
| V. Simplicity / minimal scope / dependencies | Smallest coherent diff: per-pass guards + DCE roots + O2 verbatim carry + tests + docs. No new abstractions, kinds, flags, or commands. Dependency direction preserved (`optimize.hpp` uses only `netlist.hpp`/`eval.hpp` vocabulary). | PASS |
| Docs gate (README living spec) | User-visible change (O2 no longer refuses sequential nets; report counts on sequential nets) → `docs/logic.md` §6 + README test list updated in the same diff. | PASS — tracked as task |

No violations → Complexity Tracking intentionally empty.

**Post-design re-check (Phase 1 complete)**: design introduces no new
modules, no I/O, no serialization or RunConfig changes, and no new
user-facing surface — all six gates still PASS. No complexity exceptions.

## Project Structure

### Documentation (this feature)

```text
specs/007-flop-latch-barrier/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   └── optimize-sequential.md
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── optimize.hpp     # barrier guards (DCE roots, rewire freeze, exact skip, O2 verbatim carry)
├── netlist.hpp      # UNCHANGED (vocabulary only; no shape change)
├── eval.hpp         # UNCHANGED (solve() keeps reading seq outputs as kLow; optimizer stops calling it on seq nets)
└── seq_step.hpp     # UNCHANGED (reference semantics for run-parity tests)

src/cli/commands/logic/
└── optimize.cpp     # UNCHANGED (forwards to optimize(); report covers sequential nets as-is)

tests/
├── test_logic.cpp   # new barrier tests (O0/O1/O2 preservation, DCE roots, exact skip, O2 carry, run parity)
└── test_cli_args.cpp# optimize CLI over sequential nets succeeds at all levels; report shape

docs/
└── logic.md         # §6 optimizer levels: barrier semantics; O2 no longer refuses sequential nets

README.md            # test-list touch-up only
```

**Structure Decision**: Single-project header-only layout (existing repo
shape). The feature adds no new files to production code — only guards
inside `optimize.hpp` — plus tests and docs.

## Complexity Tracking

> No Constitution Check violations. Nothing to justify.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — | — | — |
