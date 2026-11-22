# Tasks: Data-Parallel Word Ops for Bit-Parallel Evaluator

**Input**: Design documents from `/specs/017-data-parallel-word-ops/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md
**Tests**: Parity test explicitly requested (spec FR-012, contracts/parity-matrix.md) — test tasks included.

**Organization**: Tasks grouped by user story; each story phase is an independently testable increment.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Pin the before-state so every later claim is comparable.

- [X] T001 Verify clean tree and green baseline in `build/` (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON`, `cmake --build build`, `ctest --test-dir build --output-on-failure`) and record HEAD commit hash in `specs/017-data-parallel-word-ops/working/before.md`
- [X] T002 [P] Capture before-numbers with `benchmarks/bench_logic.cpp` on the bench preset (`cmake --preset bench`, record per-gate-word figure, echoed `-O3 -DNDEBUG` flags, compiler, host) into `specs/017-data-parallel-word-ops/working/before.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Batched evaluation core in the logic domain — MUST complete before any story adoption.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T003 [P] Add `kWordBatchWidth = 4` and `batch_words()` size helper with `static_assert` coverage in `include/wengine/logic/block_interface.hpp`
- [X] T004 [P] Add `ws_wordblock` pool workspace plus amortized growth in `ensure_workspace` in `include/wengine/logic/types.hpp` (workspace never serialized, excluded from `Netlist::operator==`)
- [X] T005 Implement shared gate-op helper plus both `eval_block_words` overloads (caller-owned span + pool-backed convenience) in `include/wengine/logic/eval.hpp`, leaving `eval_block_word` body untouched; ship Doxygen `@brief`/`@param` comments per constitution
- [X] T006 Build all logic targets to prove the header-only engine compiles warning-clean on the baseline compiler (no intrinsics/target-attribute/`-march` flags introduced)

**Checkpoint**: Foundation ready — batched entry compiles; user story work can now begin.

---

## Phase 3: User Story 1 - Truth evaluation runs faster with identical tables (Priority: P1) 🎯 MVP

**Goal**: Batched passes produce words bit-identical to the single-slice path; hot loops adopt batching.

**Independent Test**: Full-slice sweep on every corpus net: `eval_block_words` batches + single-slice tail vs per-slice `eval_block_word` — every word `==`; plus one benchmark run showing lower per-gate-word cost.

### Tests for User Story 1

> Write the test FIRST; ensure it FAILS before T008/T009 land (single-slice reference exists, batch entry missing until Phase 2 — test fails at link/compile until T005, then fails on behavior until adoption is proven).

- [X] T007 [P] [US1] Write failing parity corpus in `tests/test_logic_word_batch.cpp` (axes: all 8 combinational kinds, fan-in 1..8, input counts 1..12 plus one ≥31-input spot-slice net, ties, ignored/pull-terminated dangling, sequential fallthrough, degenerate nets; checks: batch-vs-single `==`, scalar-vs-word `==`, repeat determinism) and register `add_executable` + `add_test` in `CMakeLists.txt`

### Implementation for User Story 1

- [X] T008 [US1] Adopt batched loops (full batches + single-slice tail) in both `eval_block_word` call sites in `src/cli/commands/logic/common.cpp` (truth streaming ~line 374, export ~line 489), keeping output logic byte-identical
- [X] T009 [US1] Add batch timing section (same net, same slices, opaque xorshift inputs, checksummed outputs) beside the word loop in `benchmarks/bench_logic.cpp`
- [X] T010 [US1] Run `test_logic_word_batch` plus all `test_logic*` binaries green and verify `truth` verb output bytes identical before/after on a sample net

**Checkpoint**: US1 fully functional and testable independently — faster truth, identical tables.

---

## Phase 4: User Story 2 - Every host gets correct words, accelerated or not (Priority: P2)

**Goal**: Portable correctness proven — no new dependency, no build break, automatic identical fallback.

**Independent Test**: Default and bench-preset builds succeed warning-clean with no new required dependency; tail/single paths reproduce batch words exactly on the whole corpus.

- [X] T011 [P] [US2] Verify warning-clean configure+build on default and bench presets and gate the diff (grep new code for `immintrin`/`arm_neon`/`__m128`/`__m256`/`target(`/`march=` — must be absent outside vendored `external/`); record results in `specs/017-data-parallel-word-ops/working/portability.md`
- [X] T012 [US2] Extend `tests/test_logic_word_batch.cpp` with non-multiple-of-4 slice counts and degenerate nets so the single-slice tail path is exercised on every run, proving tail words `==` batch words

**Checkpoint**: US1 AND US2 work — speed where available, identical words everywhere.

---

## Phase 5: User Story 3 - Nothing outside speed moves (Priority: P2)

**Goal**: Refusal classes, parity disciplines, and persisted artifacts all behave exactly as before.

**Independent Test**: Full pre-feature suite green; every refusal verdict identical batch-vs-single; replays/round-trips byte-identical.

- [X] T013 [P] [US3] Add refusal-verdict parity cases (shared ≥2 attachments, live `kTbuf`, X/Z block inputs, undersized scratch, output-size mismatch, out-of-range slice) asserting identical `kInvalidArgument` from batched and single entries in `tests/test_logic_word_batch.cpp`
- [X] T014 [US3] Run the full `ctest --test-dir build --output-on-failure` suite plus save/load round-trips and example replays, asserting byte-identical outputs and verdicts vs the Phase 1 baseline

**Checkpoint**: All three stories independently functional — only per-gate-word cost changed.

---

## Phase 6: User Story 4 - The win is measured and nothing else regresses (Priority: P2)

**Goal**: Committed before/after evidence; no collateral regression.

**Independent Test**: Committed record shows word-evaluation curve moved, every other stage within noise; vectorization claim inspected, not assumed.

- [X] T015 [US4] Re-run `bench_logic` on the pinned bench preset with repetitions, confirm packed 64-bit lane ops in the batch inner loop codegen (`-fopt-info-vec`/`objdump` note), and re-run the full baseline sweep proving no other stage regressed beyond stated noise
- [X] T016 [US4] Commit before/after per-gate-word numbers with environment pin, repetition/variance policy, and per-size footprint in `docs/benchmarks/logic-scale.md`, keeping `docs/audit_catalogs.py` at zero missing/zero ghost entries

**Checkpoint**: Evidence trail complete — the optimization is measured, not asserted.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Docs, catalog hygiene, final validation.

- [X] T017 Update README test list (and module table only if user-visible behavior changed — default: test list only) for `test_logic_word_batch` and the `bench_logic` batch section in `README.md`
- [X] T018 [P] Run `quickstart.md` validation end-to-end (parity → CLI bytes → measurement → vectorization sanity) and fix any drift between the guide and the implementation
- [X] T019 Final full-suite green (`ctest --test-dir build --output-on-failure`), zero new compiler warnings on baseline presets, working tree buildable with no unrelated files changed

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately.
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories.
- **User Stories (Phases 3–6)**: All depend on Foundational completion; then proceed in priority order (US1 → US2 → US3 → US4) or in parallel if staffed (US2/US3 touch mostly different files: portability notes vs refusal tests vs bench record).
- **Polish (Phase 7)**: Depends on all desired stories being complete.

### User Story Dependencies

- **US1 (P1)**: After Foundational only — delivers the MVP (batched engine + adoption + parity).
- **US2 (P1)**: After Foundational; integrates with US1's test file (T012 extends T007) but verifiable independently.
- **US3 (P2)**: After Foundational; refusal tests extend the same test file; suite check independent.
- **US4 (P2)**: After US1 (needs adopted loops + bench section for after-numbers).

### Within Each User Story

- Test written and FAILs before implementation (T007 before T008/T009).
- Engine core (Phase 2) before caller adoption (T008/T009).
- Adoption before measurement (T015 needs T009's bench section).

### Parallel Opportunities

- T001 + T002 (baseline build vs before-numbers on separate build dirs — use `build/` vs `build-bench/`).
- T003 + T004 (different headers: `block_interface.hpp` vs `types.hpp`).
- T007 (new test file) can be drafted in parallel with T005 once signatures are fixed by the contract.
- T011 + T013 (portability grep vs refusal tests — different files).
- T018 can run alongside T015/T016 (validation vs record-writing).

---

## Parallel Example: User Story 1

```bash
# Test file first (fails until engine lands):
Task: "Write failing parity corpus in tests/test_logic_word_batch.cpp + CMakeLists.txt registration"
# After Phase 2, adoption + bench in parallel (different files):
Task: "Adopt batched loops in src/cli/commands/logic/common.cpp"
Task: "Add batch timing section in benchmarks/bench_logic.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 (Setup — before-numbers pinned).
2. Complete Phase 2 (Foundational — batched core compiles).
3. Complete Phase 3 (US1 — parity + adoption + bench).
4. **STOP and VALIDATE**: parity test green, truth bytes identical, batch faster.
5. Demo: truth output unchanged, `bench_logic` batch line improved.

### Incremental Delivery

1. Setup + Foundational → batch engine ready.
2. US1 → identical-but-faster truth (MVP).
3. US2 → portability proven (no new deps, tail parity).
4. US3 → regression pin (refusals + full suite identical).
5. US4 → committed evidence (bench record updated).
6. Polish → docs/catalogs/final green.

---

## Notes

- [P] tasks = different files, no dependencies.
- [Story] labels (US1–US4) map tasks to spec user stories for traceability.
- Constraints quoted verbatim from design docs: scratch refuses loudly with `kInvalidArgument` "before any work"; words compare with bit-exact `==` (no tolerances); steady state "allocates nothing and throws nothing"; benchmark inputs stay "opaque" with "checksummed" outputs.
- Commit after each task or logical group; no commit/push unless explicitly requested (repo governance).
- Constitution Doxygen duty (new public APIs ship `@brief`/`@param`) is inside T005, not deferred to Polish.

---

## Phase 8: Convergence

**Purpose**: Close the remaining gaps between spec/plan/tasks and the implementation (converge run 2026-11-22; 19/19 prior tasks verified complete in code).

- [X] T020 Commit the 10k/100k/1M before/after ladder (per-size means, ranges, checksums, invocations, footprints) to the `## bench_logic` record in `docs/benchmarks/logic.md` per FR-011, SC-002 (partial)
- [X] T021 Reconcile the record path: add a cross-reference pointer under `## bench_logic_scale` in `docs/benchmarks/logic-scale.md` to the 017 record in `docs/benchmarks/logic.md` (the catalog-correct home) per plan structure, T016 (partial)
