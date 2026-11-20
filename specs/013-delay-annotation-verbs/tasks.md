# Tasks: Propagation-Delay Annotation Verbs (013)

**Input**: Design documents from `/specs/013-delay-annotation-verbs/`
(plan.md, spec.md, research.md D1–D9, data-model.md, contracts/
engine-seam.md, cli-verbs.md, workspace-yaml.md, quickstart.md)

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: Included — the spec demands reference-anchored verification
(FR-014: "independently derived value conversions (never the
implementation's own readback), CLI representation, saved artifact, and
re-read artifact, including every refusal class and the zero-value/
cleared equivalence") and the constitution requires multi-boundary
coverage. New binary `tests/test_logic_delay.cpp` carries the story
tests; `tests/test_cli_args.cpp` gains registry/completion pins
incrementally (each verb task keeps the size asserts green so the
tree never stops building).

**Organization**: Tasks grouped by user story; each story phase is an
independently testable increment on top of the foundation.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1…US4)
- Exact file paths in every description

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Baseline confirmation and the new test-binary scaffold
before any behavior changes. No benchmark baseline is recorded —
research D9: no hot path is touched (the evaluator never reads the
field), and the constitution forbids unmeasured claims, so the feature
makes none.

- [X] T001 Confirm the pre-change baseline: `cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON && cmake --build build && ctest --test-dir build --output-on-failure` green; record the no-bench decision from specs/013-delay-annotation-verbs/research.md (D9) in the working notes
- [X] T002 Create the tests/test_logic_delay.cpp scaffold (empty-OK `test_delay: OK` binary) and register it in CMakeLists.txt (`add_executable` + `target_link_libraries` with `wengine_cli` + `wengine_logic_yaml` + `add_test`; inherits `wengine_test_assert` via the `test_` prefix)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: The two engine seams every story stands on — the exact
value parse and the field write/clear functions. No user-story work
begins until this is complete.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T003 [P] Add `parse_time_ps(text) -> core::Result<std::uint64_t>` to include/wengine/cli/args.hpp per contracts/engine-seam.md "Exactness contract": bare token `^[0-9]+$` → exact u64 picoseconds via `std::from_chars` (full field range); suffixed token `^[0-9]+(\.[0-9]+)?$` + one owned `scales_for<Time>` spelling (`s sec ms us ns min h ps`, case-sensitive) with the matched scale classified into {1, 60, 3600, 10⁻³, 10⁻⁶, 10⁻⁹, 10⁻¹²} (double-literal comparison; unclassifiable suffix refuses loudly); value = integer arithmetic `num × factor × 10^(12+exp10)` over the implied fraction divisor in unsigned `__int128` with an explicit u64 overflow check. Constraint quoted verbatim from data-model.md: "No floating point participates anywhere in the parse." Loud refusals: negative ("delay must be non-negative"), sub-picosecond remainder, field overflow, unknown/cross-dimension suffix, malformed grammar. Thin forwarder over units/scales.hpp (no suffix knowledge here); Doxygen block per the constitution
- [X] T004 [P] Add `set_gate_delay(Netlist&, slot, value_ps)` and `clear_gate_delay(Netlist&, slot)` to include/wengine/logic/netlist.hpp beside `set_pull` (:361) / `clear_pull` (:393) per contracts/engine-seam.md: live-slot validation (`kInvalidArgument`, netlist untouched on failure); "clear refuses loudly when the field is zero" (the `clear_pull` precedent); "Legal on **every live gate kind** — combinational and sequential alike" (no kind check; per-kind semantics belong to the future delay evaluator, FR-008); idempotent last-write-wins; zero is a legal write equal to clearing. `[[nodiscard]]`, `noexcept`, Doxygen blocks citing docs/logic.md §12

**Checkpoint**: Foundation ready — `cmake --build build` green (engine
headers extended additively; existing `test_logic*` binaries compile);
user story implementation can now begin.

---

## Phase 3: User Story 1 — Annotate a gate with a propagation delay (Priority: P1) 🎯 MVP

**Goal**: `set-delay <NAME> <VALUE>` writes exact whole-picosecond
delays on named live gates; the annotation is visible in `list`.

**Independent Test**: Annotate with a bare and a unit-suffixed value,
assert the stored value and listing readback match hand-derived
conversions (spec US1) — runnable via quickstart Scenario 1 plus the
refusal half of Scenario 3.

### Tests for User Story 1

> **NOTE: Write these tests FIRST, ensure they FAIL before implementation
> (T005 compiles against T003 but its verb halves fail until T006/T007
> land — engine tasks first, same convention as 012)**

- [X] T005 [US1] Value-exactness goldens in tests/test_logic_delay.cpp against the hand-derived table in contracts/engine-seam.md (never the implementation's readback): `1500` → 1500; `1.5ns` → 1500; `250ps` → 250; `0.001ns` → 1; `2us` → 2,000,000; `5ms` → 5,000,000,000; `1.5h` → 5,400,000,000,000,000; bare `18446744073709551615` (u64 max) exact; refusals: `-1`, `1.5ps` (sub-ps), `6000h` (field overflow), bare `1.5` (grammar), `1500V` (cross-dimension), `1e3ns` (grammar), unknown suffix
- [X] T008 [US1] Verb-lifecycle + display pins in tests/test_logic_delay.cpp: `set-delay` success echo `set AND1 delay=1500ps` (canonical ps regardless of token form); `list` shows `delay=1500ps` on exactly the annotated gate; re-set replaces (last write wins, FR-011); `set-delay X 0` leaves the listing clean (zero is the un-annotated state); unknown gate → usage error exit 2; stray/missing operands → exit 2; un-annotated `list` output byte-identical to pre-feature

### Implementation for User Story 1

- [X] T006 [US1] Declare `LogicSetDelaySpec` in include/wengine/cli/logic_registry.hpp (inserted after `LogicUnpullSpec` in `LogicRegistry`) and implement `set-delay <NAME> <VALUE>` in src/cli/commands/logic/set_delay.cpp (new file, added to `wengine_cli` sources in CMakeLists.txt): `parse_time_ps` (T003) + `resolve_gate` (include/wengine/logic/pins.hpp:364) + `set_gate_delay` (T004); "bare integers mean picoseconds, suffixed forms convert exactly through the owned time table" in the usage text plus the zero-delay inertness note (docs/logic.md §12); completion = `--help`/`-h` + gate names via `collect_gate_names` (src/cli/commands/logic/common.hpp:48), nothing in the value position (research D7); Doxygen per struct. Keep the tree green in the same change: sync tests/test_cli_args.cpp pins to 26 — `:689` size assert, `:816` `verbs.size()`, `:822` help-completion count, `find("set-delay")` block, `:1762` `want_s` gains `"set-delay"` before `"solve"` — and add `set-delay` to the `LogicSpec::kUsage` session-verb list in include/wengine/cli/commands.hpp (:644-645)
- [X] T007 [P] [US1] Annotate the gate line in src/cli/commands/logic/list.cpp: ` delay=<N>ps` "iff the stored value is nonzero" (contracts/cli-verbs.md), beside `inputs=`/`fanout=` on the same line (:46); un-annotated workspaces list byte-identically (present-only, the 012 pull-display precedent)

**Checkpoint**: US1 fully functional and independently testable —
quickstart Scenario 1 passes; every un-annotated flow byte-identical.

---

## Phase 4: User Story 2 — The annotation is inert: stored, never interpreted (Priority: P1)

**Goal**: Annotated netlists evaluate identically to un-annotated ones;
zero-delay evaluation stands; no code path reads the field to alter
behavior.

**Independent Test**: Evaluate the same netlist twice — un-annotated,
then with every gate heavily annotated — and assert solve/step/truth/
export outputs are byte-identical, plus every pre-existing golden green
(spec US2) — runnable via quickstart Scenario 2.

### Tests for User Story 2

- [X] T009 [US2] Inertness tests in tests/test_logic_delay.cpp: same netlist with and with-out annotations → byte-identical solve, step, truth, and export outputs; annotation on gates feeding shared/pulled/tied nets resolves exactly as before (no topology interplay); record the reader inventory re-run (research.md "Facts that bind": the only field readers are save/check/replay, the O2 reset, display, tests) in the test comments as the no-hot-path proof

### Implementation for User Story 2

- [X] T010 [US2] Verification-only story — no implementation lands. Run the full pre-existing suite (`ctest --test-dir build --output-on-failure`) plus a zero-warning check (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on `wengine_cli`) and confirm every pre-feature golden, saved-workspace behavior, and refusal message is unchanged (spec US2 acceptance 3)

**Checkpoint**: The zero-delay rule proven — quickstart Scenario 2
passes with zero regressions anywhere.

---

## Phase 5: User Story 3 — Clear the annotation, and know exactly where it persists (Priority: P2)

**Goal**: `unset-delay <NAME>`; zero ≡ cleared everywhere;
save/reload round-trips annotations; O0/O1 preserve in place, O2's
documented reset is reported; removal clears with the gate.

**Independent Test**: Annotate, clear, assert byte-indistinguishable
from never-annotated; save/reload identical; pre-feature workspace
loads byte-identically; every optimizer level behaves per its standing
contract with the reset visible (spec US3) — runnable via quickstart
Scenarios 4–6.

### Tests for User Story 3

- [X] T011 [US3] Lifecycle + persistence + optimizer tests in tests/test_logic_delay.cpp: `unset-delay` on an un-annotated gate refused ("no delay set on NOT1", exit 2) and `set-delay X 0` ≡ cleared; remove the annotated gate → recycled slot starts un-annotated (netlist.hpp:103/:764 pins); save → `delay_ps:` row carries the verb-written value → load → identical field → replay byte-identical (contracts/workspace-yaml.md "New test pins"); examples/logic/tieoff.yaml (pre-feature, already carries `delay_ps: 1500`/`250`) loads byte-identically; O0/O1 preserve in place; O2 resets with the note printed iff ≥1 annotation existed; optimize-of-optimize idempotent; world binary checkpoint round-trip (include/wengine/world/serialize.hpp u64 column)

### Implementation for User Story 3

- [X] T012 [US3] Declare `LogicUnsetDelaySpec` in include/wengine/cli/logic_registry.hpp (inserted after `LogicSetDelaySpec`) and implement `unset-delay <NAME>` in src/cli/commands/logic/unset_delay.cpp (new file, added to `wengine_cli` sources in CMakeLists.txt): `resolve_gate` + `clear_gate_delay` (T004); map the engine's zero-field refusal to `usage_error("no delay set on <NAME>")` exit 2 (the src/cli/commands/logic/unpull.cpp:35-37 pattern); success line `cleared <NAME> delay`; completion = gate names + `--help`/`-h`; Doxygen per struct. Keep the tree green in the same change: sync tests/test_cli_args.cpp pins 26 → 27 (`:689` size, `:816`, `:822`, `find("unset-delay")` block, `:1766` `want_u` gains `"unset-delay"` after `"unpull"`) and add `unset-delay` to the `LogicSpec::kUsage` list (include/wengine/cli/commands.hpp)
- [X] T013 [US3] Add the conditional O2 reset note to src/cli/commands/logic/optimize.cpp: pre-scan live gates for nonzero delays; when level==O2 and count ≥1 print `note: O2 reshaped the network; reset the propagation delay on N gate(s) (docs/logic.md §12)` (contracts/cli-verbs.md, verbatim); "Zero-annotation runs print nothing extra (byte-identical)" — mirror of the interface-changed note at optimize.cpp:110

**Checkpoint**: US3 complete — quickstart Scenarios 4–6 pass; the
schema is untouched (`version: 1`, no new section).

---

## Phase 6: User Story 4 — One vocabulary across every driving surface (Priority: P2)

**Goal**: Both verbs covered by completion, byte-identical across
headless/interactive front ends; registry pins complete.

**Independent Test**: Drive set/clear through the session verbs with
the engine seam's exact acceptance/refusals, assert completion offers
gate names (and nothing for values), and assert interactive output
byte-identical to headless (spec US4) — runnable via quickstart
Scenario 7.

### Tests for User Story 4

- [X] T014 [US4] Full-surface pins in tests/test_cli_args.cpp: `logic set-delay ` completes gate names; `logic set-delay AND1 ` completes nothing (values are free-form); `logic unset-delay ` completes gate names; both complete `--help`/`-h` in flag position; one-shot (`logic <verb>`) and in-session forms accept/refuse identically; interactive parity pins byte-identical to headless by construction

### Implementation for User Story 4

- [X] T015 [US4] Parity verification only — no new surface lands (completion shipped with T006/T012, parity is by construction): configure `-DBUILD_TUI=ON`, build, and confirm the new verbs flow through `TuiExecutor` → `cli::Shell` with no TUI source change (no logic form exists; forms compose argv only — constitution III's front-end parity rule)

**Checkpoint**: All user stories independently functional —
quickstart Scenario 7 passes end-to-end.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Living-spec duties, catalog hygiene, final validation.

- [X] T016 [P] Documentation updates in docs/logic.md (§10 verb grammar gains `set-delay`/`unset-delay` with the exactness rules and the zero-≡-cleared equivalence; §12 Timing slot ships the annotation verbs and keeps the delay-honoring evaluator as the named follow-on) and docs/tests/logic.md (page for tests/test_logic_delay.cpp)
- [X] T017 [P] README.md updates: verb sentence gains `set-delay`/`unset-delay`; a runnable annotate→list→solve-unchanged example block; test list gains `test_logic_delay`; module table's `logic` row mentions delay annotation
- [X] T018 Run specs/013-delay-annotation-verbs/quickstart.md Scenarios 1–7 end-to-end, then the full `ctest --test-dir build --output-on-failure`, zero-warning check, and `docs/audit_catalogs.py` at zero-missing/zero-ghost (new binary + verbs)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately (T001 baseline before any code change).
- **Foundational (Phase 2)**: Depends on Setup — BLOCKS all user stories (the exact parse and the field write/clear are the floor every story stands on).
- **User Stories (Phases 3–6)**: All depend on Foundational completion; US1 → US2 → US3 → US4 in priority order, each independently testable at its checkpoint. US3/US4 are P2 and may lag the P1 cut.
- **Polish (Phase 7)**: Depends on all desired user stories being complete.

### User Story Dependencies

- **US1 (P1, annotate)**: After Foundational — no story dependencies. **MVP scope**: Setup + Foundational + US1 (+ the US2 inertness test before any demo — it is the safety net proving the write changed nothing observable).
- **US2 (P1, inertness)**: After US1's verbs exist — verification-only; no implementation of its own (the constraint is preserved, not built).
- **US3 (P2, clear + persistence)**: After Foundational — uses `clear_gate_delay` (T004) and the verbs' registry slot from T006; independently testable on set-delay alone.
- **US4 (P2, one vocabulary)**: After US1+US3 verbs exist — pins the complete surface; no new surface lands.

### Within Each User Story

- Tests written FIRST and observed to FAIL before the story's implementation tasks.
- Engine seam before CLI verbs before readback/docs within a story.
- The registry size asserts move 25 → 26 (T006) → 27 (T012) inside the verb tasks — the tree stays buildable at every checkpoint; no task leaves a failing `static_assert`.
- Story checkpoint (quickstart scenario) green before moving to the next priority.

### Parallel Opportunities

- T003 ‖ T004 (different files: args.hpp vs netlist.hpp, no shared edits).
- T006 verb TU ‖ T007 list annotation — different files, but T006 also touches tests/test_cli_args.cpp; T007 touches only list.cpp → parallel-safe.
- T012 ‖ T013 (unset_delay.cpp vs optimize.cpp, different files; both after T011's tests are written).
- T016 ‖ T017 (docs/logic.md + docs/tests/logic.md vs README.md).
- Once Foundational completes, US1 and US3's engine halves can be staffed in parallel (US3's verb depends only on T004, not on US1's verb TU); US2/US4 are verification phases that naturally run last among their priority group.

---

## Parallel Example: User Story 1

```bash
# Tests first (same binary file — sequential):
Task: "Value-exactness goldens in tests/test_logic_delay.cpp"       # T005
Task: "Verb-lifecycle + display pins in tests/test_logic_delay.cpp" # T008

# Then implementation in parallel (different files):
Task: "LogicSetDelaySpec + set_delay.cpp + registry pins to 26"     # T006
Task: "list delay annotation in src/cli/commands/logic/list.cpp"   # T007 [P]
```

## Parallel Example: User Story 3

```bash
# Test first:
Task: "Lifecycle + persistence + optimizer tests in tests/test_logic_delay.cpp" # T011

# Then implementation in parallel (different files):
Task: "LogicUnsetDelaySpec + unset_delay.cpp + registry pins to 27" # T012
Task: "Conditional O2 reset note in src/cli/commands/logic/optimize.cpp" # T013
```

---

## Implementation Strategy

### MVP First (P1 annotation core)

1. Complete Phase 1 (Setup — baseline green, scaffold registered).
2. Complete Phase 2 (Foundational — exact parse + engine write/clear).
3. Complete Phase 3 (US1 — `set-delay` + `list` readback).
4. **STOP and VALIDATE**: quickstart Scenarios 1 + 3-refusals + `ctest`;
   run the US2 inertness test before demoing — it proves the write is
   observable only as stored data.
5. Demo: annotate a gate, show the listing, show solve unchanged.

### Incremental Delivery

1. Setup + Foundational → engine floor ready.
2. + US1 → MVP: writable, inspectable, exact annotations.
3. + US2 → proven inert: zero-delay stands, goldens green.
4. + US3 → full lifecycle: clear verb, round-trip persistence,
   visible O2 reset.
5. + US4 → coherent surface: completion, parity pins.
6. + Polish → living spec + catalog hygiene + final validation.

### Parallel Team Strategy

With multiple developers: team completes Setup + Foundational
together; then Developer A takes US1+US3 (verbs, list, optimize note),
Developer B takes US2+US4 (verification phases), Developer C takes
Polish (docs, README, final runs) — stories merge in priority order at
their checkpoints.

---

## Notes

- [P] tasks = different files, no dependencies; same-file tasks stay sequential.
- [Story] label maps each story-phase task to its spec user story for traceability.
- FR-014 constraint quoted into tasks: T005/T008/T011 verify against hand-derived conversions and pinned artifacts, never the implementation's readback.
- Data-model/contract constraints quoted verbatim: "No floating point participates anywhere in the parse" (T003); "clear refuses loudly when the field is zero" and "Legal on every live gate kind" (T004); " delay=<N>ps iff the stored value is nonzero" (T007); "no delay set on <NAME>" (T012); the O2 note text (T013).
- Registry-size discipline: `LogicRegistry` 25 → 26 (T006) → 27 (T012), each move inside the verb task that causes it — the tree never stops building.
- Stop at any checkpoint to validate the story independently; never commit/push/branch unless explicitly requested.
