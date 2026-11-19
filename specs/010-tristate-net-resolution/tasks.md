# Tasks: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Input**: Design documents from `/specs/010-tristate-net-resolution/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/

**Tests**: INCLUDED — explicitly requested by the feature specification (FR-011, SC-001/SC-002/SC-003, and each story's Independent Test criteria require assertion coverage against the IEEE 1164-anchored reference).

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Engine headers: `include/wengine/logic/` (module `wengine_logic`, INTERFACE)
- Tests: `tests/test_logic.cpp` (binary `test_logic`, ctest name `logic`)
- Docs: `docs/logic.md`, `docs/api/logic.md`, `README.md`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish the green pre-change baseline this feature's
"no regressions" criterion is measured against.

- [X] T001 Configure and build the baseline (`cmake -S . -B build -DBUILD_TESTS=ON`, `cmake --build build`), run `ctest -R '^logic$' --test-dir build`, and record the current PASS state of the five funnel placeholder assertions in `tests/test_logic.cpp` (~lines 260-270)

**Checkpoint**: Baseline builds cleanly; the v1 placeholder refusals
pass pre-change — the "before" evidence for US4.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The single total-resolution funnel every story verifies
against. One indivisible diff (per constitution minimal-scope rule);
all stories share it.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T002 Implement total four-value resolution in `include/wengine/logic/types.hpp` (`resolve_net` body per research.md D1/D2/D3: high-Z transparent fold, unknown absorbing, low-vs-high conflict to `kUnknown`, consensus to the agreed level, `n==0` to `kHighZ` with `true`, ill-formed null-span to `false`) plus the compile-time vocabulary pinning `static_assert` (research.md D7)
- [X] T003 Update the `LogicValue` enum and `resolve_net` Doxygen contract in `include/wengine/logic/types.hpp` (research.md D6 item 1: live Z/X semantics, total-resolution contract table, `n==0` meaning, `false` meaning; keep the `docs/logic.md` §12 cross-reference accurate)

**Checkpoint**: Foundation ready — the funnel resolves the full
[contracts/resolve-net.md](contracts/resolve-net.md) table; user story
verification phases can now begin.

---

## Phase 3: User Story 1 - Detect contention on a shared net (Priority: P1) 🎯 MVP

**Goal**: Contending drivers (`{L,H}`) resolve to `kUnknown`, never a
silently picked winner; any `kUnknown` driver dominates levels; level
consumers keep rejecting X loudly.

**Independent Test**: Resolving a span of two opposite-level drivers
yields exactly `kUnknown` (never level/Z), and an existing consumer
given that result still rejects it loudly.

### Tests for User Story 1

> **NOTE**: The shared funnel (Phase 2) precedes story verification,
> so these tests extend the table on top of it; each story's group is
> independently runnable via the narrowest ctest filter.

- [X] T004 [US1] Add the contention assertion group to `tests/test_logic.cpp`: conflict rows (`{L,H}` → `kUnknown`), X-dominance rows (`{X}` → `kUnknown`, `{H,X}` → `kUnknown`, `{L,H,X}` → `kUnknown`), plus all distinct orderings of one representative conflict span (order-invariance spot check)
- [X] T005 [US1] Build and run `tests/test_logic.cpp` via `ctest -R '^logic$' --test-dir build --output-on-failure`; confirm the US1 acceptance scenarios and that existing consumer X-rejection tests still pass unchanged

**Checkpoint**: At this point, contention detection is fully
functional and testable independently (MVP: Setup + Foundational +
US1 — the correctness floor).

---

## Phase 4: User Story 2 - Model an enable-gated tri-state bus (Priority: P2→P1)

**Goal**: High-Z drivers are transparent; one enabled driver puts its
level on the bus; a fully disabled/empty bus reads `kHighZ`,
distinct from `kUnknown`.

**Independent Test**: `{high, high-Z}` → level, `{high-Z}` →
`kHighZ`, and adding/removing any number of high-Z drivers never
changes a span's resolved value.

> **Priority note**: spec.md ranks US1 and US2 both P1 (the minimum
> coherent bus model); this phase immediately follows the MVP.

### Tests for User Story 2

- [X] T006 [US2] Add the high-Z transparency assertion group to `tests/test_logic.cpp`: `{Z}` → `kHighZ`, `{H,Z}` → `kHigh`, `{L,Z,Z}` → `kLow`, `{Z,Z}` → `kHighZ`, empty span (`n==0`) → `kHighZ` with `true`, and add/remove-Z no-change checks
- [X] T007 [US2] Build and run `tests/test_logic.cpp` via `ctest -R '^logic$' --test-dir build --output-on-failure`; confirm the US2 acceptance scenarios

**Checkpoint**: At this point, User Stories 1 AND 2 are both
independently verifiable — the complete minimum coherent bus model.

---

## Phase 5: User Story 3 - Let agreeing drivers coexist (Priority: P2)

**Goal**: Multiple agreeing actives resolve to the agreed level (no
false conflict, no technology-specific reinterpretation).

**Independent Test**: `{low, low}`, `{low, low, low}` → low;
`{high, low, high-Z}`-style counter-case still → `kUnknown`.

### Tests for User Story 3

- [X] T008 [US3] Add the consensus assertion group to `tests/test_logic.cpp`: `{L,L}` → `kLow`, `{L,L,L}` → `kLow`, `{H,H}` → `kHigh`, `{H,H,Z}` → `kHigh`, plus the positive/negative pair `{L,L,L}` → `kLow` vs `{L,L,H}` → `kUnknown`
- [X] T009 [US3] Build and run `tests/test_logic.cpp` via `ctest -R '^logic$' --test-dir build --output-on-failure`; confirm the US3 acceptance scenarios

**Checkpoint**: All contention/Z/consensus behaviors are now
independently functional and verified.

---

## Phase 6: User Story 4 - Keep every v1 single-driver flow bit-identical (Priority: P2)

**Goal**: Every live v1 path (single low/high driver, all existing
workspaces/goldens) behaves identically; the superseded placeholder
refusals are re-pinned loudly, not silently dropped.

**Independent Test**: The pre-existing suite passes unchanged except
the funnel placeholder assertions, which now pin the new table; CLI
smokes are byte-identical.

### Tests for User Story 4

- [X] T010 [US4] Re-pin the five superseded placeholder assertions in `tests/test_logic.cpp` (~lines 260-270) to the new [contracts/resolve-net.md](contracts/resolve-net.md) table (`{Z}` → `kHighZ`, `{X}` → `kUnknown`, `{Z,Z}` → `kHighZ`, `{H,H}` → `kHigh`, `n==0` → `true`/`kHighZ`) — explicitly superseding the v1 refusal contract per spec US4 scenario 3
- [X] T011 [US4] Run the full regression `ctest --test-dir build --output-on-failure`; confirm zero failures and that the `git diff` on tests touches only the funnel region of `tests/test_logic.cpp`
- [X] T012 [US4] Run the CLI byte-identity smokes and confirm outputs identical to the Phase 1 baseline: `printf 'logic add AND --inputs 2\nlogic solve --in 01\nexit\n' | ./build/PhysicsSimApp`, `./build/PhysicsSimApp sim run --solver logic --seq-file examples/logic/nand_pair.yaml --steps 4`, `./build/PhysicsSimApp sim run --steps 100 --particles 1`

**Checkpoint**: All user stories are now independently functional and
the v1 contract preservation is proven.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Shipped-contract documentation and end-to-end validation.

- [X] T013 [P] Update `docs/logic.md` §12 per research.md D6 item 2: mark the Value-system slot shipped and note the Drivers slot now resolves four-value multi-driver spans while netlist-level multi-driver wiring remains the reserved follow-on
- [X] T014 [P] Update the funnel-contract paragraph in `docs/api/logic.md` per research.md D6 item 3 (funnel resolves Z/X; consumers still reject them)
- [X] T015 [P] Update the `logic` module table row in `README.md` per research.md D6 item 4 (four-value resolution funnel note)
- [X] T016 Run `docs/build.sh` and confirm zero Doxygen warnings and zero missing/ghost catalog entries (depends on T002, T003, T013–T015)
- [X] T017 Run the [quickstart.md](quickstart.md) validation end-to-end (steps 1–4) and confirm every row of its "what done looks like" table

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion — BLOCKS all user stories
- **User Stories (Phases 3–6)**: All depend on Foundational phase completion
  - Test-edit tasks (T004, T006, T008, T010) all touch `tests/test_logic.cpp` — sequence them in priority order (US1 → US2 → US3 → US4), do not edit concurrently
  - Story verification runs (T005, T007, T009, T011) follow their story's edits
  - T011 (full regression) runs after all funnel edits are in
- **Polish (Phase 7)**: T013–T015 depend on all story phases; T016 depends on T002, T003, T013–T015; T017 last

### User Story Dependencies

- **User Story 1 (P1)**: Starts after Foundational — the MVP correctness floor; no dependencies on other stories
- **User Story 2 (P1)**: Starts after Foundational — cumulative verification on the same funnel; recommended same increment as US1
- **User Story 3 (P2)**: Starts after Foundational — independently verifiable consensus slice
- **User Story 4 (P2)**: Starts after Foundational — preservation proof; its full-suite run (T011) is the gate for Polish
- Honest dependency note: US1–US4 share the single Phase-2 funnel body (indivisible coherent diff); story phases are cumulative, independently *checkable* verification slices, not independent implementations

### Within Each User Story

- Assertion group added first, then narrowest ctest run to confirm
- Task edit order within the shared test file follows story priority (US1 → US2 → US3 → US4)
- US4's placeholder re-pinning (T010) lands before the full regression (T011)

### Parallel Opportunities

- T013, T014, T015 (three doc files) can run in parallel
- Everything else is sequential by design: T002→T003 share `include/wengine/logic/types.hpp`; T004/T006/T008/T010 share `tests/test_logic.cpp`; verification runs need the build in their post-edit state

---

## Parallel Example: Phase 7 Documentation

```bash
# Launch all three doc updates together (different files, no dependencies):
Task: "Update docs/logic.md §12 per research.md D6 item 2"
Task: "Update the funnel-contract paragraph in docs/api/logic.md per research.md D6 item 3"
Task: "Update the logic module table row in README.md per research.md D6 item 4"
```

---

## Implementation Strategy

### MVP First (US1: contention detection)

1. Complete Phase 1: Setup (green baseline recorded)
2. Complete Phase 2: Foundational — total funnel body + Doxygen + vocabulary pinning (CRITICAL — blocks all stories)
3. Complete Phase 3: User Story 1 (contention: conflict → X, X dominance, consumer rejection preserved)
4. **STOP and VALIDATE**: `ctest -R '^logic$' --test-dir build` green; US1 acceptance scenarios hold
5. US1 alone is the correctness floor — shippable as the MVP increment

### Incremental Delivery

1. Setup + Foundational → funnel resolves the full contract table
2. Add US1 → verify independently → MVP (contention detected, never silent)
3. Add US2 → verify independently → expressible enable-gated bus + undriven net
4. Add US3 → verify independently → wired consensus without false conflicts
5. Add US4 → full regression + CLI byte-identity → v1 preservation proven
6. Polish → shipped-contract docs + site build gate + quickstart sign-off

---

## Notes

- [P] tasks = different files, no dependencies (here: T013–T015 only)
- [Story] label maps task to its user story for traceability (US1–US4)
- Test tasks included because the spec explicitly requires them (FR-011, SC-001/002/003); all test goldens come from the IEEE 1164-anchored reference in research.md D1, never from the implementation
- Story test-edits share `tests/test_logic.cpp` — sequence, do not parallelize
- Stop at any checkpoint to validate the story independently via `ctest -R '^logic$' --test-dir build`
- Commit/branch/PR only if explicitly requested (project git discipline)

## Execution notes (implementation session 2026-11-19)

- Execution adjustment: the four stale placeholder assertions (T010's
  conversion content) landed inside the T004 edit as a green-binary
  prerequisite — the old `assert(!...)` lines abort against the new
  funnel, so no story verification run could pass before conversion.
  T010 then verified the supersession complete (no stale text remains;
  all conversions carry explicit `(was: refused)` markers).
- T011 result (updated): full suite is now **74/74 green**. The 5
  earlier failures (`cli_args`, `tui_exec`, `tui_batches`,
  `tui_async`, `tui_views`) were proven environmental on the
  pristine tree (root cause: `/tmp/opencode` root-owned and not
  writable by the build user); after the tmp-permissions fix the
  entire suite passes with this feature's changes applied — zero
  regressions from the funnel change.
- T016/T017 closed: with doxygen 1.18.0 installed and the pinned
  sphinx toolchain in a venv, `docs/build.sh` exits 0; the Doxygen
  stage parses `include/wengine/logic/types.hpp` with zero warnings
  and no undocumented entities (the single Sphinx highlighting notice
  on first run was the pre-existing `docs/ros2.md:162` shell-snippet
  quirk in an untouched file); `docs/audit_catalogs.py` passes
  (76/76 tests, 12/12 benchmarks, 26/26 examples — zero missing,
  zero ghost). Quickstart steps 1–4 all green, including the step-4
  docs-build gate.

---

## Phase 8: Convergence

**Purpose**: Close the one remaining gap found by `/speckit.converge`
(2026-11-19 assessment): the funnel implementation is correct and
fully green, but the test table does not yet pin the entire SC-001
exhaustive matrix the spec promises.

- [X] T018 Extend the funnel assertions in `tests/test_logic.cpp` to the full SC-001 matrix per SC-001 (partial): assert every non-empty driver multiset of size 1..4 (the 4 singles already covered plus the remaining pairs/triples/quads), all distinct orderings of representative spans (`{L,H,Z}`, `{L,H,X}`, `{H,X}`, `{Z,X}`), and large-span spot checks (e.g. 8/16 drivers, all-Z and mixed), with expected values taken from the research.md D1 reference table — then rebuild and run `ctest -R '^logic$' --test-dir build --output-on-failure` green
