# Implementation Plan: JK/T Flop Kinds

**Branch**: `003-jk-t-flops` | **Date**: 2026-11-01 | **Spec**: `specs/003-jk-t-flops/spec.md`

**Input**: Feature specification from `/specs/003-jk-t-flops/spec.md`

## Summary

Add two synchronous flop kinds — JK (`J`/`K`/`CLK`, hold/reset/set/toggle)
and T (`T`/`CLK`, hold/toggle) — to the existing logic-gate netlist as a
small additive kind extension. Both kinds ride the existing single-edge
stepper (`seq_step`/`seq_step_filtered`), reuse the pin/wire/poke/peek/
save-load machinery via fixed input indices, and expose their excitation
tables in help. Approach (from research): append-only kind codes 18/19,
fixed fan-in (JK=3, T=2), snapshot-phase excitation in the stepper, no
async path, no YAML schema bump, no new CLI commands.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (repo standard)

**Primary Dependencies**: None new — header-only `wengine::logic` engine,
existing CLI stack, yaml-cpp for the file store (unchanged usage)

**Storage**: YAML `version: 2` workspace files (schema unchanged; loader
accepts the two new kind rows by fan-in, writer emits canonical names)

**Testing**: Plain-`assert` binaries printing `test_x: OK`, registered with
`add_executable` + `target_link_libraries` + `add_test` and linked against
`wengine_test_assert` (the `test_` prefix policy); full suite via
`ctest --output-on-failure`; PTY script only if TTY behavior changes (it
does not — no line-editing changes)

**Target Platform**: Linux headless CLI (REPL / one-shot / pipe / script);
GUI unaffected (no GUI changes in this feature)

**Project Type**: CLI + header-only engine library (`INTERFACE` targets)

**Performance Goals**: CLI-paced paths only (step/poke/peek are
human/script paced); stepper stays allocation-free `noexcept` as today —
no benchmark required (no hot-path change, per constitution IV)

**Constraints**: Zero warnings under
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` on
`wengine_cli`; out-of-source builds only; `cmake --preset dev`
reproducible config

**Scale/Scope**: +2 gate kinds; ~10 files touched (engine headers, YAML
store, CLI help/completion, docs, tests). No new commands, no new verbs,
no serialization version bump.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|-----------|------|--------|
| I. Correctness | Excitation tables tested against hand-derived truth tables (research R3), never against implementation output | PASS — plan requires independent-reference tables in `data-model.md` + tests |
| II. Determinism | No change to op order, accumulation, snapshot timing, or serial assignment for existing kinds; new kinds use the same slot-order snapshot/commit | PASS — additive snapshot branch only |
| III. Independent-reference testing | Engine → CLI → YAML round-trip coverage; `test_` prefix (assert policy); registry-size `static_assert`s updated | PASS — contracts + quickstart define the three levels |
| IV. Measured performance | No hot-path change (CLI-paced stepper already allocation-free); no optimization proposed | PASS — no benchmark needed |
| V. Minimal scope / dependencies | Headers-only engine change; `cli -> solvers`, `signal` untouched; no new abstractions (pins, not sidecars); no unrelated cleanup | PASS |
| Docs (README living spec) | User-visible kinds → update Run examples, module table, test list | PASS — scoped in contracts/Phase 1 |

No violations — Complexity Tracking stays empty.

*Post-design re-check (Phase 1, 2026-11-01): design keeps all gates green —
no new structs except the two enum codes, no Netlist/SeqState reshape,
YAML schema untouched, CLI surface extended by table entries only.*

## Project Structure

### Documentation (this feature)

```text
specs/003-jk-t-flops/
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
├── types.hpp        # GateKind += kJk=18, kT=19; gate_kind_name; next_counter 18->20
├── netlist.hpp      # valid_fanin (JK=3, T=2), default_fanin (JK=3, T=2),
│                    #   is_sequential_kind += JK/T, parse_gate_kind "JK"|"T",
│                    #   resolve_gate kind table, parse_pin J/K/T/CLK rows
├── eval.hpp         # eval_gate + bit-parallel eval: JK/T join the sequential
│                    #   fallthrough (combinational eval never sees them)
├── seq_state.hpp    # unchanged (is_sequential_kind covers the new kinds)
├── seq_step.hpp     # snapshot-phase JK/T excitation; no async-path change
└── optimize.hpp     # audit: extend every kind switch / sequential guard

src/logic/
└── yaml_store.cpp   # pin_text canonical J/K/CLK + T/CLK; kind-name error
                      #   strings; fan-in accepted via valid_fanin (no schema bump)

src/cli/commands/logic/
├── common.cpp       # help text (kind list + excitation tables) + pin completion
└── *.cpp            # no logic changes (add/poke/step/peek/wire/save ride the
                      #   engine tables generically); only help/completion strings

tests/
├── test_logic_seq_kinds.cpp    # ordinals/names/parse for JK/T (extend)
├── test_logic_seq_step.cpp     # excitation truth tables vs hand reference (extend)
└── test_logic_seq_cli_*.cpp / test_logic_yaml.cpp  # session + round-trip goldens

docs/logic.md  # kind table (§12) + excitation tables
README.md      # logic module-table row + test list
```

**Structure Decision**: Single-project layout preserved — engine stays
header-only `INTERFACE` (platform-TU exception not needed), CLI owns the
session surface, tests stay plain-assert binaries. No new directories.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| — (none) | — | — |
