# Implementation Plan: Propagation-Delay Annotation Verbs (013)

**Branch**: `013-delay-annotation-verbs` | **Date**: 2026-11-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/013-delay-annotation-verbs/spec.md`

## Summary

Two module verbs — `set-delay <NAME> <VALUE>` and `unset-delay <NAME>` —
make the reserved per-gate `propagation_delay_ps` field (u64, default 0,
already persisted by every save format and already covered by documented
optimizer rules) user-writable and visible. Values are exact: bare
integers mean whole picoseconds (strict u64 parse); unit-suffixed forms
(`1.5ns`, `250ps`, `30min`) parse through the units-owned time-suffix
table with **pure integer arithmetic** (plain-decimal grammar; sub-ps
remainders, overflow, negative, and cross-dimension text refuse loudly —
no floating point anywhere, so exactness holds to the full field range).
The engine seam gains two Doxygen'd functions (`set_gate_delay` /
`clear_gate_delay`, mirroring `set_pull`/`clear_pull`); `list` annotates
gates with nonzero delays; the O2 optimizer report gains one conditional
note when it resets annotations (its documented standing behavior — no
pass changes). Zero evaluation, schema, struct, checkpoint, or benchmark
change: annotation is inert by construction. All decisions are anchored
in [research.md](research.md) (D1–D9).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning target
with the `wengine_cli` flag set).

**Primary Dependencies**: existing only — engine header-only
`INTERFACE` libraries (`wengine_core`, `wengine_logic`),
`wengine_cli` (Shell + ArgCursor + the units-owned suffix tables via
`cli/args.hpp`), `wengine_logic_yaml` (the single yaml-cpp TU). No new
external or vendored dependency.

**Storage**: in-memory `Netlist` — **no struct change**: the u64 field
already lives on `Gate` (types.hpp:333), is zeroed by `add_gate` and by
slot clear/reset (netlist.hpp:103/:764), and is persisted by the YAML
store (`delay_ps:` required key on every gate row, u64-validated at
yaml_store_check.cpp:184) and by the world binary checkpoint
(serialize.hpp:352/:791). This feature adds no storage work.

**Testing**: plain-`assert` binaries via ctest; new
`tests/test_logic_delay.cpp` (exactness goldens → verbs → list →
optimizer → save/reload round-trip, reference-anchored); syncs to
`tests/test_cli_args.cpp` (registry 25 → 27, `want_s`/`want_u`
completion vectors, `find` blocks); `wengine_test_assert` policy
inherited via the `test_` prefix.

**Target Platform**: Linux (WSL2 baseline), headless CLI + optional
TUI (no TUI change — parity by construction, no logic form exists).

**Project Type**: C++20 physics/logic simulation engine + CLI modules.

**Performance Goals**: none claimed. No hot path is touched: the
evaluator never reads the field, and verbs/list/completion are cold
(research D9). Constitution IV is satisfied by the absence of a claim
plus the pinned inertness test, not by a benchmark.

**Constraints**: exact-or-loud value contract (integer arithmetic only —
no FP recovery tricks, research D2); determinism (last-write-wins,
pure functions of the token); zero-warning; byte-identical output for
every un-annotated flow (list prints the annotation only when nonzero;
the O2 note only when ≥1 annotation was reset); no serialization-format
drift (the schema is untouched — `delay_ps` is already canonical).

**Scale/Scope**: `LogicRegistry` 25 → 27 verbs (`set-delay`,
`unset-delay` inserted after `LogicUnpullSpec`); two new verb TUs; two
engine functions in `netlist.hpp`; one parse helper in `cli/args.hpp`;
one new test binary; no new example file (tieoff.yaml already carries
nonzero `delay_ps` rows — nothing new to fixture).

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Phase-0 | Post-Phase-1 (design) |
|---|---|---|
| I. Physical/numerical correctness | Values must be exact whole picoseconds or loud refusals — no rounding | PASS — pure integer arithmetic to the full u64 range (engine-seam.md "Exactness contract"); goldens derived by hand decimal arithmetic (1.5ns → 1500), never from implementation readback |
| II. Determinism & reproducibility | No evaluation, ordering, or serialization change is requested | PASS — the field is inert (no eval path reads it — pinned by test); parse is a pure function of the token (no FP, no locale); last-write-wins; `delay_ps` row bytes already canonical so saved files are byte-identical for equal annotations |
| III. Independent-reference testing | Multi-boundary verification is the house norm | PASS — token → stored field → `list` line → saved YAML artifact → re-read/replay (workspace-yaml.md "Test pins"); new binary + registry pins enumerated (cli-verbs.md); no PTY need (completion covered by the existing completion harness style) |
| IV. Measured performance only | Feature must not pretend hot-path relevance | PASS — no benchmark touched, no claim made (research D9); the only engine additions are two cold-path functions; the field's non-read by eval is asserted, not assumed |
| V. Simplicity, minimal scope, dependency discipline | Header-only engine modules; one command domain; no ad-hoc unit tables in cli/ | PASS — two functions in an existing header (no new TU); suffix spellings/scales read from `units/scales.hpp` via the `cli/args.hpp` forwarder discipline (research D2); no struct/schema/checkpoint/Snapshot change; two verbs on the `pull`/`unpull` precedent |
| Logic-domain constraints (append-only kinds, loud failures, optimizer opacity, loud YAML) | All named in the constitution | PASS — no `GateKind` change; unknown names/values/refusals all loud with state untouched; optimizer passes unchanged with one conditional report note (documented O2 reset, research D6); YAML store untouched — its loud required-key/u64-validation conventions already govern the field |

**Gate result**: no violations; no Complexity Tracking entries
required.

## Project Structure

### Documentation (this feature)

```text
specs/013-delay-annotation-verbs/
├── spec.md              # The feature specification (/speckit.specify output)
├── plan.md              # This file (/speckit.plan output)
├── research.md          # Phase 0: decisions D1-D9, all clarifications resolved
├── data-model.md        # Phase 1: annotation entity, value grammar, persistence model
├── quickstart.md        # Phase 1: runnable validation scenarios
├── contracts/
│   ├── engine-seam.md   # set/clear functions, exactness contract, inertness, optimizer policy
│   ├── cli-verbs.md     # set-delay / unset-delay / list / optimize note / registry sync / docs duties
│   └── workspace-yaml.md# pins the already-shipped delay_ps schema (no change) + round-trip rules
└── checklists/
    └── requirements.md  # spec quality validation (complete)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp            # UNTOUCHED — Gate::propagation_delay_ps already exists (u64, default 0)
├── netlist.hpp           # +set_gate_delay / +clear_gate_delay beside set_pull:361 / clear_pull:393
|                        #   (live-slot validation, Doxygen, [[nodiscard]] Status returns)
|                        #   add_gate:103 and slot-clear:764 already zero the field — unchanged
└── yaml_store.hpp       # UNTOUCHED — delay_ps already a required, u64-validated key (check.cpp:22/:184)
include/wengine/cli/
├── args.hpp             # +parse_time_ps — exact whole-picosecond time parse (thin forwarder over
|                        #   scales_for<Time> + split_number_suffix; integer arithmetic, loud refusals)
├── commands.hpp         # LogicSpec::kUsage session-verb list gains set-delay unset-delay
└── logic_registry.hpp   # LogicSetDelaySpec / LogicUnsetDelaySpec structs; LogicRegistry 25 -> 27
                         #   (inserted after LogicUnpullSpec, before LogicRemoveSpec)
src/cli/commands/logic/
├── set_delay.cpp        # NEW verb — write (parse_time_ps + resolve_gate + set_gate_delay)
├── unset_delay.cpp      # NEW verb — clear (resolve_gate + clear_gate_delay; "no delay set" refusal)
├── list.cpp             # gate line gains ` delay=<N>ps` iff nonzero (byte-identical otherwise)
└── optimize.cpp         # conditional note after O2: annotations reset on N gate(s) — printed iff
                         #   level==O2 && pre-scan found >=1 nonzero delay (mirror of the
                         #   interface-changed note at optimize.cpp:110)
src/logic/               # NO yaml-store change (delay_ps already canonical + validated + replayed)
tests/
├── test_logic_delay.cpp # NEW binary: value goldens -> verbs -> list -> optimizer -> round-trip
└── test_cli_args.cpp    # registry pins: :689 size 27, :816/:822 counts, :1762 want_s gains
                         #   "set-delay" (registry order: before solve), :1766 want_u gains
                         #   "unset-delay" (after unpull), find blocks for both verbs
CMakeLists.txt           # wengine_cli sources += set_delay.cpp, unset_delay.cpp; test_logic_delay
                         #   target (add_executable + link + add_test; test_assert policy inherited)
examples/logic/          # UNTOUCHED — tieoff.yaml already carries nonzero delay_ps rows
benchmarks/              # UNTOUCHED — eval never reads the field (research D9)
docs/logic.md            # §10 verb grammar gains set-delay/unset-delay; §12 Timing slot ships the
                         #   annotation verbs (the delay-honoring evaluator stays the follow-on)
docs/tests/logic.md      # page for the new binary; catalog audit stays zero-missing/zero-ghost
README.md                # verb sentence, runnable set-delay example, test list, module-table row
```

**Structure Decision**: single-project layout unchanged. All engine
edits are additive functions inside existing headers (modules stay
header-only `INTERFACE`); the two verb TUs follow the existing
per-file pattern (`pull.cpp`/`unpull.cpp`); one new test binary; no new
directories, no new platform TU.

## Complexity Tracking

No constitution violations to justify — the table is intentionally
empty. The one deliberate design weight is the integer-exactness parse
(research D2): it exists because the constitution forbids silent
rounding anywhere a stored physical value is produced, and its rejected
alternative (floating-point recovery with a magnitude cap) is recorded
in research.md rather than here, since it was rejected, not traded.
