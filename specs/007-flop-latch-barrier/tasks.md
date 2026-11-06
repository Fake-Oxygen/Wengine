# Tasks: Flop-Latch Opaque Barrier

**Input**: Design documents from `/specs/007-flop-latch-barrier/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/optimize-sequential.md, quickstart.md

**Tests**: Included — required by the feature spec (acceptance scenarios per
story), the contract's equivalence clause, and Constitution Principle III
(multi-boundary verification). Tests are written FIRST and must FAIL before
the implementation task(s) they cover.

**Organization**: Grouped by user story. All production edits land in
`include/wengine/logic/optimize.hpp` (no new files); tests join the existing
`tests/test_logic.cpp` / `tests/test_cli_args.cpp` targets.

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline build green before any change

- [X] T001 Configure dev preset, build, and record baseline `ctest` results from repo root (`cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON && cmake --build build && ctest --test-dir build --output-on-failure`)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Shared test helpers every story's tests use

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T002 Add sequential-net test helpers + gate-set-compare oracle in tests/test_logic.cpp (builders: closed DFF counter cluster, T divider + redundant glue, acyclic DLATCH/SRLATCH pipeline, JK flop with NQ consumer, full-control DFF with async pins; helper asserting sequential name/kind/logical-driver/polarity equality before vs after)

**Checkpoint**: Foundation ready — user story work can begin

---

## Phase 3: User Story 1 — Optimize a sequential net at O0 without losing state (Priority: P1) 🎯 MVP

**Goal**: O0 (and shared O0/O1 fixpoint passes) preserve all state elements;
DCE roots state; exact truth-table pass skips sequential nets

**Independent Test**: Build a DFF-counter + latch-glue net, optimize at O0,
assert the sequential set is identical (names, kinds, logical drivers,
polarity) and a 10-step fixed-step run matches the original every step;
unobserved flop and closed counter cluster survive; `optimize(optimize(x))
== optimize(x)`

### Tests for User Story 1 (write FIRST — must FAIL before T004–T006)

- [X] T003 [US1] Write O0 barrier tests in tests/test_logic.cpp (DFF counter + latch glue set-identity; unobserved-flop survival; closed-cluster DCE survival; 10-step run parity via seq_step reference; idempotence)

### Implementation for User Story 1

- [X] T004 [US1] DCE roots + sequential-aware fixpoint topology in include/wengine/logic/optimize.hpp (roots[] gains every live sequential gate; new detail::recompile dispatch: no seq gates -> ensure_topology as today, else validate_sequential + barrier order with seq gates as slot-order roots; route all ensure_topology calls inside optimize() through it; replace SEQ11 placeholder comment with barrier rationale)
- [X] T005 [US1] Freeze barrier pins in include/wengine/logic/optimize.hpp (bypass_pass and fusion double-negation branch skip removing a BUF/NOT consumed by any live sequential gate; replace SEQ11 comments)
- [X] T006 [US1] Skip exact_simplify_pass on nets with live sequential gates plus defensive sequential skip in const-fold collapse-roots loop in include/wengine/logic/optimize.hpp (replace SEQ11 comments)
- [X] T007 [US1] Build and run test_logic barrier tests plus full `ctest --output-on-failure` green with zero warnings

**Checkpoint**: US1 fully functional — O0 over sequential nets preserves state and run parity holds

---

## Phase 4: User Story 2 — Optimize around barriers at O1/O2 without touching state (Priority: P2→P1)

**Goal**: O1 bubble-push and O2 universal conversion simplify glue around
barriers while carrying state elements verbatim (O2 succeeds instead of
refusing loudly)

**Independent Test**: `NOT(Q)` consumer and `BUF`-feeding-flop untouched at
O1; NQ polarity + full-control DFF pins preserved at O1/O2; O2 output has
verbatim sequential gates, converted glue, and 10-step run parity

### Tests for User Story 2 (write FIRST — must FAIL before T009–T010)

- [X] T008 [US2] Write O1/O2 barrier tests in tests/test_logic.cpp (NOT-driven-by-comb feeding flop unchanged at O1; flop-output consumers never bypassed at O1/O2; constant-driven flop input never folded; NQ polarity preserved; O2 verbatim carry + run parity)

### Implementation for User Story 2

- [X] T009 [US2] Add O2 verbatim-emit branch for reached live sequential gates in include/wengine/logic/optimize.hpp (same kind/input_count/ignored_mask/ignore_output/ignore_nq_output, fresh slot via add_gate in slot order, one PendingWire per input resolved through emit_map, per-pin driver_inv copy, NQ fanout recount; replace SEQ11 emit_gate comment; keep try_minimized_o2 sequential refusal as the structural fallback)
- [X] T010 [US2] Mark fresh sequential gates in O2 post-cleanup post_roots[] in include/wengine/logic/optimize.hpp (dedup/double-inv need no change — universal-kind-only)
- [X] T011 [US2] Build and run test_logic O1/O2 tests plus full `ctest --output-on-failure` green with zero warnings

**Checkpoint**: US1 + US2 work — all three levels preserve state; O2 converts glue around verbatim barriers

---

## Phase 5: User Story 3 — Optimize a sequential net end-to-end with no loud refusal (Priority: P2)

**Goal**: `logic optimize` succeeds at O0/O1/O2 over sequential nets with a
well-formed net and full state-element set (no new failure modes)

**Independent Test**: CLI `logic optimize --level O0|O1|O2 [--to nand|nor]`
over a sequential workspace exits 0; combinational-only nets behave exactly
as before (same success/failure, same gate counts)

**Depends on**: US1 + US2 implementation (no new production change in this
phase — CLI forwards to `optimize()` unchanged per research R0)

- [X] T012 [P] [US3] Add sequential optimize CLI tests in tests/test_cli_args.cpp (O0/O1/O2 exit 0 with flat sequential counts; O2 --to without O2 still usage error; stray positionals still usage errors; regression: combinational optimize output unchanged)
- [X] T013 [US3] Build and run test_cli_args CLI tests plus full `ctest --output-on-failure` green

**Checkpoint**: US1 + US2 + US3 work — CLI contract holds end to end

---

## Phase 6: User Story 4 — Trust the before/after report on sequential nets (Priority: P3)

**Goal**: Report counts provably attribute all reduction to combinational
glue; user docs state the barrier contract

**Independent Test**: Net with known redundant glue shows total-count drop
with sequential census flat; redundancy-free net shows identical total
count; `docs/logic.md` §6 + README test list updated

- [X] T014 [US4] Add report-count assertions in tests/test_logic.cpp (glue-redundant net: total shrinks, sequential census flat; redundancy-free net: total unchanged)
- [X] T015 [P] [US4] Update optimizer barrier semantics in docs/logic.md (§6: opaque barriers, DCE roots, exact skip, O2 acceptance) and test list in README.md
- [X] T016 [US4] Final validation: full `ctest --output-on-failure` green on dev and test-opt presets with zero warnings; walk through quickstart.md in specs/007-flop-latch-barrier/quickstart.md

**Checkpoint**: All user stories independently functional

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final gates before handoff to implementation review

- [X] T017 Verify no stray SEQ11 placeholder comments remain in include/wengine/logic/optimize.hpp and run complete quickstart.md validation in specs/007-flop-latch-barrier/quickstart.md (REPL spot-checks + headless run parity + docs check per its troubleshooting table)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately (T001)
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all stories (T002)
- **User Stories (Phase 3–6)**: Depend on Foundational; test tasks precede their story's implementation tasks
  - US1 (T003–T007) first — MVP
  - US2 (T008–T011) after US1 (extends the same fixpoint; O2 carry assumes R1/R2 semantics)
  - US3 (T012–T013) after US1+US2 (asserts end-to-end success of their implementation; T012 test-writing may start in parallel with US1/US2 work — different file)
  - US4 (T014–T016) after US1+US2 (asserts counts their implementation produces; T015 docs may be drafted in parallel)
- **Polish (Phase 7)**: Depends on all stories complete (T017)

### Within Each User Story

- Test task FIRST, verify it FAILS, then implement, then build-and-run green
- Single-file production edits (`optimize.hpp`) run sequentially in listed order
- Story checkpoint green before next story's implementation begins

### Parallel Opportunities

- T012 (`tests/test_cli_args.cpp`) can be written in parallel with any `tests/test_logic.cpp` work (different files)
- T015 (`docs/logic.md`, `README.md`) can be drafted in parallel with implementation once research decisions are fixed
- No two tasks editing `include/wengine/logic/optimize.hpp` or `tests/test_logic.cpp` run in parallel (same-file conflicts)
- T001, T007, T011, T013, T016 validation runs are sequential gates, not parallel work

---

## Parallel Example: User Story 1

```bash
# Test-writing for US1 (tests/test_logic.cpp) can overlap with US3 test-writing
# (tests/test_cli_args.cpp — different file) and docs drafting:
Task: "T003 [US1] Write O0 barrier tests in tests/test_logic.cpp"
Task: "T012 [P] [US3] Add sequential optimize CLI tests in tests/test_cli_args.cpp"
Task: "T015 [P] [US4] Update optimizer barrier semantics in docs/logic.md and README.md"

# Implementation within a story is sequential (one shared header):
Task: "T004 [US1] DCE roots in include/wengine/logic/optimize.hpp"
# ...then T005, then T006, then T007 validation gate
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 (T001) + Phase 2 (T002)
2. Complete Phase 3: write T003, verify FAIL, implement T004→T006, verify T007 green
3. **STOP and VALIDATE**: O0 barrier holds, run parity holds, full suite green
4. MVP delivers the non-negotiable safety floor (Story 1)

### Incremental Delivery

1. Setup + Foundational → helpers ready
2. + US1 → O0 safe (MVP)
3. + US2 → O1/O2 safe, O2 refusal gone
4. + US3 → CLI contract proven end to end
5. + US4 → counts provable, docs current
6. Each increment keeps all previous stories green (full `ctest` at every checkpoint)

---

## Notes

- [P] = different files, no dependencies (T012, T015 only)
- [USn] maps each story-phase task to its spec story for traceability
- File paths: production = `include/wengine/logic/optimize.hpp` only; tests = `tests/test_logic.cpp`, `tests/test_cli_args.cpp`; docs = `docs/logic.md`, `README.md`
- Constitution gates enforced per task: plain-`assert` style, `test_` targets keep the assert policy, zero warnings, no new commands/kinds/formats, determinism + run-parity oracles, README/docs updated in-story (T015–T016)

---

## Phase 8: Convergence

**Purpose**: Close traceability gaps found by the converge assessment (all functional behavior verified in code; no missing/contradicts findings)

- [X] T018 Add explicit identical-wiring flop test in tests/test_logic.cpp per FR-006/spec edge case (partial)
- [X] T019 Add wired-constant flop-input test in tests/test_logic.cpp per US2/AC3/spec edge case (partial)
- [X] T020 Fold the O2 toggle-convergence guarantee (research R8) into spec.md edge cases per FR-007/FR-008 (unrequested)
