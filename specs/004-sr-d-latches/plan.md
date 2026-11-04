# Implementation Plan: SR/D Latch Pair

**Branch**: `004-sr-d-latches` | **Date**: 2026-11-01 | **Spec**: `specs/004-sr-d-latches/spec.md`

**Input**: Feature specification from `/specs/004-sr-d-latches/spec.md`

## Summary

Add one new level-sensitive kind — SRLATCH (`S`/`R`, set/reset/hold plus
loud `S=R=1` refusal) — and refine DLATCH (`D`/`EN`) from the documented
SEQ4 edge-capture simplification to true transparent/frozen level
behavior. Both kinds resolve outside clock advances via a shared
slot-order level-settle pass (reusing the phase-1 comb settle), are
exempt from edge snapshotting and clock domains, and ride the existing
pin/wire/poke/peek/save-load machinery via fixed input indices. Approach
(from research): append-only kind code 20, fixed fan-in 2/2, level pass
beside (not inside) `apply_async_controls`, probe-then-revert illegal
refusal on every verb path, no YAML schema bump, no new CLI commands.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (repo standard)

**Primary Dependencies**: None new — header-only `wengine::logic` engine,
existing CLI stack, yaml-cpp for the file store (unchanged usage)

**Storage**: YAML `version: 2` workspace files (schema unchanged; loader
accepts the new SR kind row by fan-in, writer emits canonical names)

**Testing**: Plain-`assert` binaries printing `test_x: OK`, registered with
`add_executable` + `target_link_libraries` + `add_test` and linked against
`wengine_test_assert` (the `test_` prefix policy); full suite via
`ctest --output-on-failure`; PTY script only if TTY behavior changes (it
does not — no line-editing changes)

**Target Platform**: Linux headless CLI (REPL / one-shot / pipe / script);
GUI unaffected (no GUI changes in this feature)

**Project Type**: CLI + header-only engine library (`INTERFACE` targets)

**Performance Goals**: CLI-paced paths only (poke/step/peek are
human/script paced); stepper stays allocation-free `noexcept` as today —
no benchmark required (no hot-path change, per constitution IV)

**Constraints**: Zero warnings under
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on
`wengine_cli`; out-of-source builds only; `cmake --preset dev`
reproducible config

**Scale/Scope**: +1 gate kind (SR=20); DLATCH semantic refinement (no
vocabulary change); ~12 files touched (engine headers, YAML store,
CLI help/completion/session, docs, tests). No new commands, no new verbs,
no serialization version bump.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|-----------|------|--------|
| I. Correctness | Level tables tested against hand-derived truth tables (research R3), never against implementation output; DLATCH goldens updated explicitly, not widened | PASS — plan requires independent-reference tables in `data-model.md` + tests |
| II. Determinism | No change to op order, accumulation, snapshot timing, or serial assignment for existing kinds; level pass uses the same slot-order settle/commit; edge kinds keep their snapshot timing | PASS — shared phase-1 helper + slot-order level commit; one documented DLATCH behavior refinement |
| III. Independent-reference testing | Engine → CLI → YAML round-trip coverage; `test_` prefix (assert policy); registry-size `static_assert`s updated | PASS — contracts + quickstart define the three levels |
| IV. Measured performance | No hot-path change (CLI-paced level pass already allocation-free); no optimization proposed | PASS — no benchmark needed |
| V. Minimal scope / dependencies | Headers-only engine change; `cli -> solvers`, `signal` untouched; no new abstractions (pins, not sidecars); no unrelated cleanup | PASS |
| Docs (README living spec) | User-visible kinds + DLATCH refinement → update Run examples, module table, test list | PASS — scoped in contracts/Phase 1 |

No violations — Complexity Tracking stays empty.

*Post-design re-check (Phase 1, 2026-11-01): design keeps all gates green —
no new structs except the one enum code, no Netlist/SeqState reshape,
YAML schema untouched, CLI surface extended by table entries + one filter
predicate + one level-pass call.*

## Project Structure

### Documentation (this feature)

```text
specs/004-sr-d-latches/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   └── session-verbs.md # User-observable kind/pin/verb contracts
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp        # GateKind += kSrlatch=20; gate_kind_name; next_counter 20->21
├── netlist.hpp      # valid_fanin (SR==2), default_fanin (SR→2),
│                    #   is_sequential_kind += SR, parse_gate_kind "SRLATCH",
│                    #   resolve_gate kAllKinds += SR, parse_pin SR rows (S→0, R→1)
├── eval.hpp         # SR joins the sequential fallthrough (comb eval never sees latches)
├── seq_state.hpp    # unchanged (is_sequential_kind covers SR)
├── seq_step.hpp     # level-settle helper (R4) + edge-exempt DLATCH/SR + comment update
└── optimize.hpp     # audit: extend every kind switch + sequential guard to SR

src/logic/
└── yaml_store.cpp   # pin_text canonical SRn.S|R + kind want-strings;
                     #   fan-in accepted via valid_fanin (no schema bump)

src/cli/commands/logic/
├── common.cpp       # pin_text SR rows + clock_domains edge-only filter +
│                    #   level-pass call in apply path + help/completion strings
├── step.cpp         # pre-edge level pass + level-exempt member stepping + reports
└── *.cpp            # poke/wire/tie/load gain probe-then-revert level check (R5);
                     #   otherwise kind-table-driven (no logic-path changes)

tests/
├── test_logic_seq_kinds.cpp    # ordinals/names/parse/pins for SR (extend)
├── test_logic_seq_step.cpp     # level tables vs hand reference + illegal + replay (extend/new)
└── test_logic_seq_cli_*.cpp / test_logic_yaml.cpp  # session goldens + round-trip

docs/logic.md  # kind table (§12) + level tables + domain-exclusion note
README.md      # logic module-table row + run examples + test list
```

**Structure Decision**: Single-project layout preserved — engine stays
header-only `INTERFACE` (platform-TU exception not needed), CLI owns the
session surface, tests stay plain-assert binaries. No new directories.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — (none) | — | — |
