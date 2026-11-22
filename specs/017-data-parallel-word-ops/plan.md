# Implementation Plan: Data-Parallel Word Ops for Bit-Parallel Evaluator

**Branch**: `017-data-parallel-word-ops` | **Date**: 2026-11-22 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/017-data-parallel-word-ops/spec.md`

## Summary

The bit-parallel word evaluator (`eval_block_word` in
`include/wengine/logic/eval.hpp`) evaluates one 64-row truth slice per
traversal of the compiled level order. Every caller (the `truth` verb,
its export twin in `src/cli/commands/logic/common.cpp`, the exact/QM
equivalence checks, `bench_logic`) loops `for w: eval_block_word(w)`,
so a full truth table pays one level-order walk plus one span assembly
per gate-input per slice. This feature adds a batched entry that
evaluates `W` consecutive slices in a single traversal — one pass
produces `W` slices of outputs — with words bit-identical to the
standing single-slice path. The approach is slice-blocking with plain
scalar `uint64_t` ops in an inner batch loop the optimizer
auto-vectorizes at `-O3` (no intrinsics, no target attributes, no new
dependency); the existing single-slice function stays untouched as the
portable reference and the tail path. Research pins the batch width,
scratch layout, guard placement, caller adoption, and measurement
procedure (see research.md).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (engine headers must
stay warning-clean under `-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor` on `wengine_cli`; engine itself is a
header-only `INTERFACE` library).

**Primary Dependencies**: None new. Only existing logic-domain
headers (`block_interface.hpp`, `topo.hpp`, `types.hpp`,
`input_resolution.hpp`, `core/status`). No intrinsics headers, no
third-party SIMD library, no CPUID/dispatch library.

**Storage**: N/A (no serialization change; batch scratch is derived,
never checkpointed — same discipline as all pool workspace).

**Testing**: Plain-`assert` binaries printing `test_x: OK`,
registered with `add_executable` + `target_link_libraries` +
`add_test`; every new `test_*` target links `wengine_test_assert`.
Parity corpus: batch-vs-single agreement on every slice plus
scalar-vs-word agreement, across all combinational kinds, mixed
fan-in, ties, ignored dangling inputs, partial-word counts,
sequential fallthrough, refusal classes, degenerate nets.

**Target Platform**: Linux host, baseline GCC 13.3; accelerated and
portable paths both build everywhere (no `-march=native`, no
per-target TUs — the engine stays header-only and platform-free).

**Project Type**: Header-only simulation-engine domain library
(`wengine/logic`) + CLI callers + benchmarks.

**Performance Goals**: Lower per-gate-word cost on `bench_logic`'s
word-evaluation loop vs the committed 015/016 baseline, by a stated
committed factor; no other baseline stage regressed beyond noise.

**Constraints**: Steady state allocation-free and exception-free;
caller-owned scratch with loud `kInvalidArgument` refusal on
undersize; `core::Status` errors, never throws; bit-exact words
(no tolerance — equality, not `near()`); determinism intact
(slot-order traversal, identical words on repeat).

**Scale/Scope**: One new header-level entry beside `eval_block_word`
plus caller adoption in `common.cpp` truth/export loops and
`bench_logic`; one new parity test; benchmark-record update. No CLI
surface change, no serialization change, no GUI/TUI change.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- [x] **I. Correctness**: Gate ops unchanged (same `&|^|~` truth);
  batching only shares the traversal across slices. Expected values
  come from the independent reference (scalar `solve` + untouched
  single-slice path), never from the new code. Bit-exact `==`
  throughout — no tolerances involved.
- [x] **II. Determinism**: Traversal order (compiled level order),
  row/lane convention, output order, and masking are untouched;
  repeated runs bit-identical. Slice assembly order stays
  slot-order; batching adds no iteration-order variance. Snapshot
  CSV unaffected (word eval feeds truth display, not checkpoints).
- [x] **III. Testing**: Multi-boundary (solver result → CLI
  representation → re-read parity corpus); new `test_*` links
  `wengine_test_assert`; suite builds without GUI/ROS; no
  optional-dependency test.
- [x] **IV. Measured performance**: Cites the committed 015/016
  baseline's word-evaluation number as the hotspot; smallest change
  (one batched entry + caller loops); opaque bench inputs +
  checksums retained; re-run committed beside baseline.
- [x] **V. Minimal scope / dependencies**: Header-only preserved
  (no new TU); dependency direction untouched (`logic → core`,
  `cli → logic` consumption only); no new abstraction without need
  (one function + one size helper); no unrelated cleanup.

Post-design re-check (2026-11-22): design holds all gates — no new
TU, no new dependency, no signature change to existing entries,
guards fire before data-parallel work, measurement follows the
standing bench record. No violations to justify; Complexity Tracking
stays empty.

## Project Structure

### Documentation (this feature)

```text
specs/017-data-parallel-word-ops/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   ├── word-batch-api.md
│   └── parity-matrix.md
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── eval.hpp              # batched entry beside eval_block_word + shared gate-op helper
├── block_interface.hpp   # batch size helper (words-for-batch), unchanged single-slice geometry
└── types.hpp             # pool batch-word workspace (ws_wordblock) beside ws_words

src/cli/commands/logic/
└── common.cpp            # truth/export loops adopt the batched entry (tail via single path)

benchmarks/
└── bench_logic.cpp       # batch timing section beside the word loop (opaque inputs, checksums)

tests/
└── test_logic_word_batch.cpp  # parity corpus: batch-vs-single, scalar-vs-word, refusals

docs/benchmarks/
└── logic-scale.md        # before/after per-gate-word numbers + environment pin
```

**Structure Decision**: Single-project header-only layout — the
feature lives entirely in the existing logic-domain headers plus
their existing callers, following the established seam ownership
(`block_interface.hpp` owns word geometry, `input_resolution.hpp`
owns span assembly, `eval.hpp` owns the level-order walk). No new
directories in source.

## Complexity Tracking

> Fill ONLY if Constitution Check has violations that must be justified

(none — no violations)
