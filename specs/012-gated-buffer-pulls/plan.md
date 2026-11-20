# Implementation Plan: Enable-Gated Buffer plus Per-Net Pull-Up/Down (012)

**Branch**: `012-gated-buffer-pulls` | **Date**: 2026-11-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/012-gated-buffer-pulls/spec.md`

## Summary

One appended gate kind (`TBUF`, byte 8: data + active-high enable;
enabled drives data, released drives high-Z) plus one per-input pull
attribute (`none`/`up`/`down`, default none) that maps an all-floating
net to its idle level. Both ride the shipped single funnel
(`resolve_net`, specs 010/011): buffers choose their span contribution,
pulls map the all-Z outcome, nothing else changes. Storage is one
additive dense-flat array (`pull_cfg`, 8 KB) beside `shared_meta`;
pulled dangling inputs are terminated at the seam (excluded from the
derived block interface, resolved to idle in `fill_resolved`). The word
path refuses live-buffer netlists loudly; pulls alone never block
truth. The workspace schema grows additively (`pulls:` section, `tbuf`
kind rows) with no generation retirement. All decisions are anchored in
[research.md](research.md) (D1–D11).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning target
with the `wengine_cli` flag set).

**Primary Dependencies**: existing only — engine header-only
`INTERFACE` libraries (`wengine_core`), `wengine_cli` (Shell + ArgCursor),
`wengine_logic_yaml` (the single yaml-cpp TU, vendored 0.8.0). No new
external or vendored dependency.

**Storage**: in-memory `Netlist` (+8 KB `pull_cfg` dense-flat array per
the `shared_meta`/`driver_inv` precedent); versioned YAML workspace,
additive optional constructs, marker stays `version: 1` (research D6).
Pulls are persistent topology state; contention counts stay
CLI-session state, never serialized.

**Testing**: plain-`assert` binaries via ctest; new
`tests/test_logic_buffer.cpp` (kind table → eval truth table →
netlist seam → verbs → YAML round-trip); extensions to
`test_logic_seq_kinds.cpp` (numbering/table pins), `test_cli_args.cpp`
(registry 23→25, `pull`/`unpull`, kind want-strings, completion),
`test_logic_yaml.cpp` (pull schema + unchanged refusals);
`wengine_test_assert` policy inherited via the `test_` prefix.

**Target Platform**: Linux (WSL2 baseline), headless CLI + optional
TUI (no TUI change required — parity by construction).

**Project Type**: C++20 physics/logic simulation engine + CLI modules.

**Performance Goals**: `bench_logic` ns/gate-word unchanged within
noise for buffer/pull-free netlists (research D11 protocol: recorded
baseline, same-args re-run, named contingency). The scalar `n ≤ 1` path
gains one level-check branch; the word seam gains no per-word branch
(guards live at word-loop entries, research D4).

**Constraints**: hot paths exception-free/allocation-free (stack span
buffer, fixed cap 4, unchanged); determinism (order-independent spans,
pinned pull mapping, terminated-dangling interface derivation);
no serialization-format drift for existing content (byte-identical
loads/saves/goldens for buffer/pull-free workspaces); Z block-output
rendering changes output bytes only where Z is observable (impossible
pre-012).

**Scale/Scope**: `kMaxGates` 1024, `kMaxInputs` 8, `pull_cfg` 8 KB
(`sizeof(Netlist)` ~150 KB → ~158 KB); `LogicRegistry` 23 → 25 verbs
(`pull`, `unpull`); one new example YAML joins `examples/logic/`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Phase-0 | Post-Phase-1 (design) |
|---|---|---|
| I. Physical/numerical correctness | Buffer/pull semantics derive from the shipped 010/011 funnel; goldens from independent derivation | PASS — `eval_gate` TBUF truth table + pull idle-mapping verified against independently derived tables (engine-seam.md "Reference anchoring"); funnel itself untouched |
| II. Determinism & reproducibility | Spans order-independent (shipped); pulls serialized, counts not | PASS — pull mapping pinned post-fold; terminated-dangling derivation deterministic (slot order); `Netlist{}` zero-init default (no pulls); no RNG/alloc/exceptions on new paths; Z rendering deterministic |
| III. Independent-reference testing | Multi-boundary verification is the house norm | PASS — engine seam → CLI verbs → YAML round-trip; new binary + extended binaries; registry `static_assert`s enumerated for sync; bench excluded from assertion policy |
| IV. Measured performance only | Done-criterion demands an unchanged fast path in bench | PASS — D11 protocol: `bench_logic` baseline + re-run + named contingency; new branches measured, not assumed |
| V. Simplicity, minimal scope, dependency discipline | Header-only engine modules, single yaml-cpp TU, one command domain | PASS — additive flat array (no new TU); schema work inside the existing store seam; two verbs on the tie/untie precedent; no dependency-direction change; exactly one new `GateKind` byte under the append-only rule |
| Logic-domain constraints (append-only kinds, loud failures, optimizer opacity, loud YAML) | All named in the constitution | PASS — byte 8 appended, no renumber; Z/X at buffer seams fail loudly pre-existing-convention; TBUF/buffer-pull optimizer freeze follows the 011 `drives_shared`/skip pattern with O2 loud refusal; YAML additive with no retirement (the documented in-generation convention) |

**Gate result**: no violations; no Complexity Tracking entries
required.

## Project Structure

### Documentation (this feature)

```text
specs/012-gated-buffer-pulls/
├── spec.md              # The feature specification (/speckit.specify output)
├── plan.md              # This file (/speckit.plan output)
├── research.md          # Phase 0: decisions D1-D11, all clarifications resolved
├── data-model.md        # Phase 1: entities, storage, serialization model
├── quickstart.md        # Phase 1: runnable validation scenarios
├── contracts/
│   ├── engine-seam.md   # buffer eval, pull mapping, termination, optimizer policy
│   ├── cli-verbs.md     # pull / unpull / add TBUF / list / solve-step-Z / truth / docs duties
│   └── workspace-yaml.md# additive pulls schema, tbuf rows, canonical bytes, loader rules
├── checklists/
│   └── requirements.md  # spec quality validation (complete)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp             # GateKind::kTbuf=8; gate_kind_name arm; Netlist +pull_cfg; is/has pull helpers
├── pins.hpp              # kKinds +TBUF row (exactly 2, default 2); kind_table_valid 13->14;
|                         #   parse_gate_kind want-string +TBUF (numeric pins only: 1=data, 2=enable)
├── netlist.hpp           # add_gate guard admits byte 8; pull/unpull engine fns;
|                         #   remove_gate/reset_netlist clear pull_cfg; tie/untie/ignore <-> pull refusals
├── block_interface.hpp   # resolve_input pull mapping (shared + single-driver Z arms, loud level check);
|                         #   is_block_input excludes pulled dangling; fill_resolved terminates them;
|                         #   has_buffer_gates()/any_pull() predicates; shared word-block unchanged
├── eval.hpp              # eval_gate kTbuf arm (enabled?data:Z); single-driver path inherits loud Z;
|                         #   eval_block_word kTbuf arm = loud refusal (safety net, entries guard first)
├── seq_step.hpp          # unchanged (phases inherit resolve_input/eval_gate; EN-default arms untouched)
├── optimize_barrier.hpp  # has_pulled_input() predicate beside has_marked_input
├── optimize_passes.hpp   # const_fold/collapse skip TBUF gates + pulled inputs; classify TBUF->kV (default arm)
├── optimize_exact.hpp    # conservative whole-net skip when buffers or pulls exist (see barrier:357 pattern)
├── optimize_o2*.hpp      # O2 refusal on buffers/pulls (optimize.hpp:124); o2_emit kTbuf loud arm (unreachable)
└── yaml_store.hpp        # additive pulls-schema declarations (header stays platform-free)
src/logic/yaml_store.cpp   # pulls: section + tbuf kind rows; loud level/row validation
include/wengine/cli/logic_registry.hpp   # LogicPullSpec/LogicUnpullSpec; LogicAddSpec usage; registry 23->25
src/cli/commands/logic/pull.cpp          # NEW verb (set pull up/down)
src/cli/commands/logic/unpull.cpp        # NEW verb (clear pull)
src/cli/commands/logic/{add,wire}.cpp    # add: TBUF kind strings + completion; wire: unchanged
src/cli/commands/logic/{list,solve,step}.cpp  # list: pull annotations; solve/step: Z output rendering
src/cli/commands/logic/{truth,export}.cpp      # truth/export entry guards refuse live-buffer netlists
src/cli/logic_draw_{sdl,imgui}.cpp       # TBUF schematic symbol (BUF triangle + EN side-stub)
tests/test_logic_buffer.cpp              # NEW: end-to-end seam binary (wengine_cli + wengine_logic_yaml)
tests/test_logic_seq_kinds.cpp            # kind numbering/table pins (13->14, byte 8)
tests/test_cli_args.cpp                  # registry 23->25, pull/unpull goldens + completion, add TBUF strings
tests/test_logic_yaml.cpp                # pulls round-trip; unchanged refusals stay green
examples/logic/buffer_bus.yaml           # example golden (joins test_examples automatically)
benchmarks/bench_logic.cpp               # unchanged (kinds 0..7 only); measured before/after (research D11)
docs/logic.md, docs/tests/logic.md, README.md   # living-spec duties per contracts/cli-verbs.md
```

**Structure Decision**: single-project layout unchanged; all engine
edits are additive members/functions inside existing headers (modules
stay header-only `INTERFACE`); the YAML platform TU and CLI verb TUs
follow the existing per-file pattern; two new verb TUs + one new test
binary; no new directories.

## Complexity Tracking

No constitution violations to justify — the table is intentionally
empty. The one deliberate scope trade (O2 loud refusal instead of a
verbatim-carry emitter for buffers/pulls) is a documented
conservative-refusal decision (research D7), not a complexity
exception; its follow-on is named in docs/logic.md §12.
