# Tasks: Cached Adjacency Lists and Maintained Indexes for Logic-Domain Hot Paths

**Input**: Design documents from `/specs/016-cached-adjacency-lists/`
**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md, contracts/
**Tests**: INCLUDED — explicitly required by spec FR-015 (parity corpus) and the parity/lifecycle/measurement contracts; every story's tests pin current behavior first and implementation must keep them green (this is a behavior-preserving change, so "FAIL before implementation" does not apply to parity pins — see each phase note)
**Organization**: Tasks grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1–US6)
- Include exact file paths in descriptions

## Path Conventions

- Engine headers (header-only, no new TUs): `include/wengine/logic/`
- Tests (plain-`assert` binaries, `test_` prefix inherits assertion policy): `tests/`
- Measurement harness (unchanged, reused): `benchmarks/bench_logic_scale.cpp`
- Records: `docs/benchmarks/logic-scale.md`, `docs/logic.md`, `docs/tests/logic.md`, `README.md`
- Working raw bench rows (consolidated by US4): `specs/016-cached-adjacency-lists/working/stage-deltas.md`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline environment and a verified-green tree before any change.

- [x] T001 Configure dev build: `cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON` then full `cmake --build build` in repo root
- [x] T002 [P] Configure bench preset: `cmake --preset bench`, capture the echoed `-O3 -DNDEBUG` flags for the delta environment pin in `docs/benchmarks/logic-scale.md`
- [x] T003 Run pre-change test gate: `ctest --test-dir build --output-on-failure` confirming a green tree (required baseline for every later gate)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The test oracle, the shared corpus, the derived cache members, and the incrementally maintained successor-adjacency writer that US1/US2/US5 all consume. No user story work begins until this phase is complete.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T004 Extract pre-change scan implementations verbatim into `tests/logic_scan_reference.hpp` (test-only, links `wengine_core`): Kahn drain scan, `reaches` successor discovery, tie member/group/re-star scans, `drives_shared` predicate, all-pairs dedup loop, lowest-free-slot scan, `rewire_users` sweep, `remove_gate` sink sweep — per `contracts/parity-harness.md` §1
- [x] T005 [P] Create `tests/test_logic_adjacency.cpp` with the shared deterministic corpus builders (layered combinational, shared/extra+NQ incl. mark/wire orderings, tie classes, flop feedback, dangling primaries/extras, extra-closing cycles) and register `add_executable` + `target_link_libraries` + `add_test` in `CMakeLists.txt` (inherits `wengine_test_assert` via the `test_` prefix)
- [x] T006 [P] Extend `NetlistT<kCap>` in `include/wengine/logic/types.hpp` with derived cache members per `data-model.md` (successor head/edge-pool/free-edge arrays, tie head/member chains, free-slot min-heap, `feeds_shared` counters, stale markers) plus `ensure_*` growers following the `ensure_workspace` pattern; Doxygen comments; no serialization fields touched (research D1/D13)
- [x] T007 Implement successor-adjacency writer maintenance across the mutation seam in `include/wengine/logic/netlist.hpp` (wire/unwire, extra append/erase/promote, `remove_gate` release, `rewire_users`) and `include/wengine/logic/shared_net.hpp` extra paths: ordered slot-position inserts (research D2), unlink on release, free-edge recycling; no consumer changed yet (scans still serve every query); add the content-invariant check to `tests/test_logic_adjacency.cpp` asserting chain multiset equals the oracle-scanned edge set after every corpus op; run green

**Checkpoint**: Foundation ready — oracle extracted, corpus built, cache members present, adjacency maintained exactly; US1/US2/US5 consumers can now be built (US3/US5/US6 maintain their own indexes inside their phases).

---

## Phase 3: User Story 1 - Topology compile reads cached adjacency (Priority: P1) 🎯 MVP

**Goal**: Kahn compile (rebuild/validate/stepper order, both filters) drains successors through the maintained adjacency; per-pop cost drops from pool-scan to out-degree; outputs bit-identical.

**Independent Test**: Corpus compile parity oracle-vs-engine (order/levels/counts/verdicts) + compile-rebuild stage bench showing the flattened cost.

> NOTE: parity tests pin current behavior — they pass pre-change; the implementation task must not turn them red (no FAIL-first; this is a behavior-preserving change).

- [x] T008 [P] [US1] Write compile parity tests in `tests/test_logic_adjacency.cpp`: rebuild/validate/`compile_comb_order` over the corpus (kAll + kCombOnly, extras, tie classes, sequential nets, deliberate cycles incl. extra-closing, dangling drivers) comparing emission order, per-slot levels, emitted/live counts, verdicts; confirm green pre-change
- [x] T009 [US1] Replace `drain_successors` in `include/wengine/logic/topo.hpp` with the per-driver adjacency-chain drain (ascending slot order, identical indeg-decrement + queue-push semantics); seed/zero/`emit_depth`/early-out loops untouched
- [x] T010 [US1] Run the compile-parity battery plus the full `test_logic*` gate green; bench the compile-rebuild stage before/after at the pinned ladder sizes and append raw rows to `specs/016-cached-adjacency-lists/working/stage-deltas.md` for US4 consolidation

**Checkpoint**: At this point, User Story 1 should be fully functional and testable independently

---

## Phase 4: User Story 2 - Wire-time cycle probes follow the adjacency (Priority: P2→P1 realized)

**Goal**: `reaches` discovers successors through the adjacency; per-probe cost tracks the reachable cone, not pool size; accept/reject verdicts identical everywhere.

**Independent Test**: Wire-attempt corpus replay (accepted, cycle-refused, flop-feedback, extra-closing) oracle-vs-engine verdict equality + growth-wire stage bench.

> NOTE: parity pins apply as in US1 (green before, stay green).

*(Priority note: spec marks US2 P1; it runs here solely so the adjacency writer (Phase 2) and drain (US1) land first — US2 starts after Foundational independently and shares no files with US1's consumer.)*

- [x] T011 [P] [US2] Write probe parity tests in `tests/test_logic_adjacency.cpp`: wire-attempt corpus replay comparing every verdict (accept/refuse), plus spot reachability pairs (sink→driver, flop-breaking, dangling endpoints) oracle-vs-engine; confirm green pre-change
- [x] T012 [US2] Rewrite successor discovery inside `reaches` in `include/wengine/logic/topo.hpp` as an adjacency walk with filter-at-read (`is_sequential_kind` per visited edge, research D3); traversal touches only the reachable cone
- [x] T013 [US2] Run probe-parity battery plus full `test_logic*` gate green; bench the growth-wire stage before/after and append raw rows to `specs/016-cached-adjacency-lists/working/stage-deltas.md`

**Checkpoint**: At this point, User Stories 1 AND 2 should both work independently

---

## Phase 5: User Story 5 - Authoring verbs read maintained indexes, not the pool (Priority: P1)

**Goal**: Gate addition resolves slots via the free-slot min-heap (identical assignment); gate removal releases driven edges by walking the adjacency (fanout-proportional); no slot-death state survives into recycled slots.

**Independent Test**: Slot-assignment replay scripts (add/remove/recycle sequences) + remove-release parity + growth-add/remove stage benches.

> NOTE: parity pins apply as in US1.

- [x] T014 [P] [US5] Write add/remove parity tests in `tests/test_logic_adjacency.cpp`: identical operation sequences asserting the oracle's slot per add (FR-017), and remove-release parity (polarity-aware fanout release, extra promotion, tie detach, mark survival, recycled-slot cleanliness); confirm green pre-change
- [x] T015 [US5] Implement free-slot min-heap maintenance in `include/wengine/logic/netlist.hpp`: pop-min in `add_gate`, push in `remove_gate`, deterministic heapify on load/reset paths; add the identical-answers check (heap pop == oracle lowest-free after every corpus op) in `tests/test_logic_adjacency.cpp`
- [x] T016 [US5] Replace the full-pool sink sweep in `remove_gate` (`include/wengine/logic/netlist.hpp`) with a walk of the removed slot's adjacency chain, releasing each recorded edge through the existing per-edge `unwire`/`unwire_driver` semantics with identical effects (depends on T015's heap push and Phase-2 chain maintenance)
- [x] T017 [US5] Run add/remove-parity battery plus full `test_logic*` gate green; bench growth-add and remove stages before/after and append raw rows to `specs/016-cached-adjacency-lists/working/stage-deltas.md`

**Checkpoint**: At this point, User Stories 1, 2 AND 5 should all work independently

---

## Phase 6: User Story 3 - Tie enumeration reads a cached member index (Priority: P2)

**Goal**: TieSet seam maintains per-leader ordered member chains; enumeration/size/survivor/re-star reads walk the chain (class-proportional), tables and verdicts identical.

**Independent Test**: Tie-operation corpus replay (merges, re-elections, dissolves, detach, reindex, counts) oracle-vs-engine + tie-op timings.

> NOTE: parity pins apply as in US1.

- [x] T018 [P] [US3] Write tie parity tests in `tests/test_logic_adjacency.cpp`: class merges (lowest-leader re-star), leader-removal re-election, non-leader untie, two-member dissolve, slot detach, compaction reindex, group sizes/counts/pair counts, member enumeration order; confirm green pre-change
- [x] T019 [US3] Implement tie-chain maintenance in the TieSet seam (`include/wengine/logic/tie_set.hpp`): ordered chain splices in `tie_pins`, unlink + re-head in `untie_pin`, per-leader rebuilds in `detach_slot_ties`/`reindex_kept_ties` per research D4
- [x] T020 [US3] Convert tie consumers to chains in `include/wengine/logic/tie_set.hpp`: `tie_class_members`, `tie_group_size`, untie survivor collection, re-star reads (`ignore_tie_class` inherits); block_interface/eval/seq consumers keep identical behavior via the same seam (depends on T018/T019)
- [x] T021 [US3] Run tie-parity battery plus full `test_logic*` gate green; record tie-op corpus timings (attribution for US4's remove-stage delta) in `specs/016-cached-adjacency-lists/working/stage-deltas.md`

**Checkpoint**: All user stories so far should now be independently functional

---

## Phase 7: User Story 6 - Optimizer pass scans are bounded by pass work (Priority: P2)

**Goal**: Feeder predicate answers O(1) from maintained counters; rewires touch only recorded users; dedup groups by structural key with exact in-bucket compare and identical merges; whole-net re-derivations happen at most once per pass.

**Independent Test**: Per-pass + fixpoint final-netlist structural equality over the corpus at every optimizer level + optimize/dedup stage benches.

> NOTE: parity pins apply as in US1.

- [x] T022 [P] [US6] Write optimizer parity tests in `tests/test_logic_adjacency.cpp`: full pipeline at every level over corpus (combinational, sequential, shared, pulled, tied) comparing per-pass state and fixpoint final netlists (slots, kinds, drivers, extras, ties, marks, pulls, fanout, per-kind counters), plus multi-sweep dedup state and opaque-barrier/feed-freeze holds; confirm green pre-change
- [x] T023 [US6] Maintain `feeds_shared` counters at the mark/wire-extra/unwire/promote/remove events in `include/wengine/logic/netlist.hpp` and consult them in the freeze predicates (`drives_shared` in `include/wengine/logic/optimize_barrier.hpp`, bypass skips in `include/wengine/logic/optimize_passes.hpp`, dedup skips in `include/wengine/logic/optimize_o2_dedup.hpp`) with boolean-identical answers
- [x] T024 [US6] Rewrite `rewire_users` in `include/wengine/logic/optimize_passes.hpp` as a walk of the replaced driver's adjacency chain (fanout-proportional), preserving ignored-bit exposure on newly dangling inputs
- [x] T025 [US6] Replace the all-pairs double loop in `include/wengine/logic/optimize_o2_dedup.hpp` with grouped-key bucketing (fixed deterministic hash, ascending in-bucket order, exact compare, collisions never merge on hash alone) per research D7; merge direction (higher into lower) and skips unchanged (depends on T022 pins)
- [x] T026 [US6] Hoist invocation-invariant guards (e.g. `has_sequential_gates`) to once per pipeline in `include/wengine/logic/optimize.hpp` and restrict `recount_fanout` in `include/wengine/logic/optimize_passes.hpp` + `include/wengine/logic/optimize_o2_dedup.hpp` to pass boundaries (never per gate/pair) per research D8
- [x] T027 [US6] Run optimizer-parity battery plus full `test_logic*` gate green; bench optimize and dedup stages before/after and append raw rows to `specs/016-cached-adjacency-lists/working/stage-deltas.md`

**Checkpoint**: All user stories should now be independently functional

---

## Phase 8: User Story 4 - The cache lifecycle is contracted and measured (Priority: P2)

**Goal**: Staleness discipline proven (no stale answers across interleaved sequences incl. load/reset), serialization byte-identical, loud scratch refusals, and the committed before/after delta record beside the baseline.

**Independent Test**: Interleaved mutation/query corpus with load/reset between queries (oracle-identical everywhere) + the dated delta section in `docs/benchmarks/logic-scale.md` with all rows cited.

> NOTE: T030's delta record requires all prior stories' bench rows; T028/T029 (the discipline pins) are independently testable right after Foundational.

- [x] T028 [P] [US4] Write lifecycle parity tests in `tests/test_logic_adjacency.cpp`: interleaved mutation/query scripts with checkpoint-load and reset between queries (every answer oracle-identical — the FR-009 stale-answer pin), plus loader stale-marking (world serialize, YAML store) and lazy-rebuild-once behavior
- [x] T029 [US4] Write loud-refusal tests in `tests/test_logic_adjacency.cpp`: undersized cache scratch refuses via `core::Status` (never partial), per `contracts/adjacency-contract.md` §6
- [x] T030 [US4] Re-run the full `bench_logic_scale` ladder and write the dated `### Delta 016` section in `docs/benchmarks/logic-scale.md` per `contracts/measurement-delta.md`: per-stage ratio tables for growth-add/growth-wire/compile-rebuild/remove/optimize/dedup, no-regression check on solve/word/shared-solve/step/reset within baseline variance, footprint delta incl. cache members, row-for-row baseline citations (depends on all prior stories' raw rows in `specs/016-cached-adjacency-lists/working/stage-deltas.md`)

**Checkpoint**: Lifecycle contracted, measured evidence committed, every story independently green

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, catalog, and final gates per the house rules.

- [x] T031 [P] Update `README.md` (Run examples, module table, test list) and `docs/tests/logic.md` (`test_logic_adjacency` CS1 entry) and `docs/logic.md` (derived-index contract note); add Doxygen `@brief`/params/`@file` role paragraphs to any new public helpers per `contracts/parity-harness.md` §5
- [x] T032 [P] Run `python3 docs/audit_catalogs.py` to zero missing entries / zero ghost anchors
- [x] T033 Final gate: dev + bench builds clean (zero warnings), full `ctest --test-dir build --output-on-failure` green, walk `quickstart.md` Scenarios 1–7 end-to-end, and record any remaining uncertainty in the session report

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion — BLOCKS all user stories
- **User Stories (Phases 3–8)**: All depend on Foundational phase completion
  - US1, US2, US3, US5, US6 consumer phases can then proceed in parallel (if staffed) — disjoint consumer files (`topo.hpp`, `tie_set.hpp`, `netlist.hpp` add/remove paths, optimizer headers)
  - US4's T028/T029 are independent after Foundational; T030 waits for all stories' bench rows
  - Sequencing recommendation: P1 order US1 → US2 → US5, then P2 US3 → US6 → US4
- **Polish (Phase 9)**: Depends on all desired user stories being complete

### User Story Dependencies

- **US1 (P1)**: After Foundational — no dependencies on other stories (adjacency writer is Foundational T007)
- **US2 (P1)**: After Foundational — shares the maintained adjacency, no code dependency on US1's consumer
- **US5 (P1)**: After Foundational — adjacency chain maintenance (T007) and free-slot heap (own T015) precede its sweep conversion (T016)
- **US3 (P2)**: After Foundational — maintains its own tie index (T019); benefits US5's detach path but requires no US5 code
- **US6 (P2)**: After Foundational — maintains its own feeder counters; consults Foundational adjacency (T007) in rewire and dedup guards
- **US4 (P2)**: T028/T029 after Foundational; T030 after all stories

### Within Each User Story

- Parity/pin tests FIRST (green pre-change), implementation keeps them green — no FAIL-first for this behavior-preserving change
- Maintenance writers before consumers within a story's files
- Consumers before measurement
- Story complete (suite green) before moving to next priority

### Parallel Opportunities

- T002 with T001; T004/T005/T006 all mutually parallel (disjoint files)
- Once Foundational completes: US1 (topo.hpp), US2 (topo.hpp reaches vs drain — same file, sequentialize), US3 (tie_set.hpp), US5 (netlist.hpp), US6 (optimizer headers) proceed in parallel across files by different workers
- T031 documentation with T032 catalog audit
- CAREFUL: US1-T009 and US2-T012 both edit `include/wengine/logic/topo.hpp` — sequence them (drain first, probe second), do not parallelize
- CAREFUL: US6-T023/T024/T025 overlap in `optimize_o2_dedup.hpp` and `optimize_passes.hpp` — strictly sequential
- CAREFUL: test additions across stories all append to `tests/test_logic_adjacency.cpp` — serialize appends per story phase

---

## Parallel Example: Foundational Phase

```bash
# Launch together (disjoint files):
Task: "Extract pre-change scans into tests/logic_scan_reference.hpp"
Task: "Create tests/test_logic_adjacency.cpp corpus builders + CMake registration"
Task: "Extend NetlistT<kCap> in include/wengine/logic/types.hpp with cache members + growers"
# Then, after all three complete:
Task: "Implement successor-adjacency writer in netlist.hpp + content-invariant check"
```

## Parallel Example: Cross-story (post-Foundational)

```bash
# Worker A: US1 drain consumer in include/wengine/logic/topo.hpp
# Worker B: US3 tie chains in include/wengine/logic/tie_set.hpp
# Worker C: US5 free-slot heap in include/wengine/logic/netlist.hpp (top regions, distinct from US1's none)
# Worker D: US6 feeder counters + guards in optimize headers
# (US2 probe waits for Worker A's topo.hpp edits to land; test appends to
#  tests/test_logic_adjacency.cpp serialize per story.)
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup (T001–T003)
2. Complete Phase 2: Foundational (T004–T007 — oracle, corpus, members, adjacency writer)
3. Complete Phase 3: User Story 1 (T008–T010 — compile parity + drain + compile-rebuild delta)
4. **STOP and VALIDATE**: parity battery + `test_logic*` gate green; compile stage flattened
5. Deploy/demo if ready (every consumer's observable output identical — safe to ship per-story)

### Incremental Delivery

1. Setup + Foundational → oracle and maintained adjacency live
2. + US1 → compile linear; test independently → ship
3. + US2 → probes cone-proportional → ship
4. + US5 → adds O(log N), removes fanout-proportional → ship
5. + US3 → tie ops class-proportional → ship
6. + US6 → optimizer bounded by pass work → ship
7. + US4 → lifecycle pins + committed delta record
8. Polish → docs, catalogs, final gates

### Parallel Team Strategy

With multiple developers:

1. Team completes Setup + Foundational together
2. Once Foundational is done:
   - Developer A: US1 (topo drain) then US2 (topo probe)
   - Developer B: US3 (tie_set) then US6 (optimizer)
   - Developer C: US5 (netlist add/remove) then US4 (lifecycle + delta)
3. Stories complete and integrate independently; T030 and T033 are the shared finish lines

---

## Notes

- [P] tasks = different files, no dependencies — the CAREFUL notes above override per-file intuition where stories share `topo.hpp`, `optimize_passes.hpp`, or the test binary
- [Story] label maps each task to its user story for traceability
- Each user story is independently completable and testable (Foundational is the only shared gate)
- No commits/PRs/branches unless explicitly requested (house git discipline)
- Verify zero new compiler warnings per task (zero-warning target)
- Stop at any checkpoint to validate the story independently
- Avoid: same-file parallel edits, cross-story dependencies that break independence, optimizing beyond the measured rows (research D11/D12 deferrals stay deferred)
