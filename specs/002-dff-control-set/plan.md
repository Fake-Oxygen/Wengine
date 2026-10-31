# Implementation Plan: DFF Control Set

**Branch**: `002-dff-control-set` | **Date**: 2026-10-31 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/002-dff-control-set/spec.md`

## Summary

Extend the DFF flop from bare D-only (`D`/`CLK`) to the minimum viable
pipeline flop: async set/reset plus clock enable, under one total priority
(reset > set > enable-hold > D capture, reset wins). DFF fan-in grows from
exactly-2 to 2-or-5 (`D`=0, `CLK`=1, `RST`=2, `SET`=3, `EN`=4); new pins
are dangling-inactive by default so existing netlists replay bit-identically.
Stepping gains a priority-resolving snapshot plus an immediate async-apply
path so `RST`/`SET` take effect without a clock advance.

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline, CMake ≥ 3.20, out-of-source
builds only (`cmake --preset dev`, `test-opt`, `bench`)

**Primary Dependencies**: header-only `wengine_logic` (`logic/netlist.hpp`,
`logic/seq_state.hpp`, `logic/seq_step.hpp`, `logic/types.hpp`,
`logic/eval.hpp`); `wengine_cli` session verbs (`src/cli/commands/logic/`);
single TU `src/logic/yaml_store.cpp` (yaml-cpp); no new dependencies

**Storage**: YAML workspace schema `version: 2` — no bump. DFF `inputs`
field accepts 2 (legacy, new pins dangling-inactive) or 5 (full control
set); loader fallback synthesizes inactive controls for `inputs: 2` files

**Testing**: plain-`assert` binaries printing `test_x: OK`, registered via
`add_executable` + `target_link_libraries` + `add_test`, every `test_*`
linking `wengine_test_assert`; registry-size `static_assert`s updated;
goldens derived independently (hand-computed truth tables), `==` on
Low/High values; full `ctest --output-on-failure` gate

**Target Platform**: Linux headless (REPL, one-shot, pipes, script files);
GUI explicitly out of scope

**Project Type**: header-only simulator engine library + CLI

**Performance Goals**: CLI-paced stepping; per-flop per-step cost adds two
pin resolves (RST/SET) + one enable check — linear slot-order scans, no
benchmark needed (no hot-path claim; SEQ4 stepper is not a measured hot
path)

**Constraints**: zero warnings (`-Wall -Wextra -Wpedantic -Wconversion
-Wshadow -Wnon-virtual-dtor` on `wengine_cli`); engine headers: no
exceptions, no allocation, no strings, no iostream, `noexcept` throughout;
sinks/CLI own all I/O; determinism (slot-order snapshot/commit, stable
serials) preserved

**Scale/Scope**: `kMaxGates` 1024 slots, `kMaxInputs` 8 (fan-in 5 fits);
flop counts small; one kind (DFF) touched, DLATCH untouched

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Gate | Status | Evidence |
|------|--------|----------|
| I. Correctness first, independent goldens | PASS | Priority truth table hand-derived in research.md; tests assert engine truth table ≠ implementation echo |
| II. Determinism preserved | PASS | Slot-order snapshot/commit retained; async-apply is slot-order too; zero-migration (dangling-inactive) keeps old outputs identical; serials untouched |
| III. Multi-boundary testing | PASS | engine unit → CLI poke/step/peek → YAML round-trip → re-read; registry `static_assert`s updated |
| IV. No unmeasured perf claims | PASS | No optimization claimed; added resolves are O(flops), CLI-paced |
| V. Minimal scope, dependency discipline | PASS | DFF-only; vocabulary stays in `netlist.hpp`/`types.hpp`, state in `seq_state.hpp`, stepping in `seq_step.hpp`; no new modules, no dependency reversal (cli → solvers) |
| Workflow (gates, docs, no git actions) | PASS | Narrowest-tests-first then full ctest; README + docs/logic.md updated; no commits/branches |

Post-Phase-1 re-check: no new violations introduced (design adds one
engine helper + fan-in extension, no new deps, no I/O in engine).

## Project Structure

### Documentation (this feature)

```text
specs/002-dff-control-set/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp        # Gate struct unchanged (kMaxInputs=8 already fits 5)
├── netlist.hpp      # valid_fanin(DFF) {2,5}; parse_pin RST/SET/EN; pin names
├── seq_state.hpp    # unchanged (Q pool, serials untouched)
└── seq_step.hpp     # priority snapshot + apply_async_controls helper

src/logic/
└── yaml_store.cpp   # writer canonical names; loader accepts inputs 2|5

src/cli/commands/logic/
├── common.cpp/hpp   # interface derivation, compose_block_inputs, help text
├── poke.cpp         # async immediate-apply after poke
├── step.cpp         # async-apply + priority step + transition report
├── peek.cpp         # unchanged form (Q/NQ already shown)
├── wire.cpp / tie / ignore / use  # new pin names flow through parse_pin
└── logic_registry.hpp (include/wengine/cli/)  # help + completion

tests/
├── test_logic_seq_controls.cpp  # new: truth table, priority, enable, async
├── test_logic_seq_cli_step.cpp / poke / yaml (extended)
└── CMakeLists.txt (new target + static_assert updates)

docs/logic.md  # priority statement + pin vocabulary + SEQ section
README.md      # Run examples, module table, test list
```

**Structure Decision**: Single-project header-only engine + CLI layout as
it exists; no new directories. The one engine addition is a free function
beside `seq_step_filtered` (same header), not a new module.

## Complexity Tracking

No constitution violations to justify — table intentionally empty.
