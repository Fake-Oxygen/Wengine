# Implementation Plan: Allowlisted Multi-Driver Nets with Loud Contention Counting

**Branch**: `011-multi-driver-nets` | **Date**: 2026-11-19 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/011-multi-driver-nets/spec.md`

## Summary

Per-input opt-in "shared" marks lift the single-driver rule for exactly
one destination input at a time (`wire --shared` marks, new `unshare`
verb unmarks); N:1 nets resolve through the shipped four-value funnel
(`resolve_net`, spec 010) with per-attachment polarity, and every
low-vs-high contended resolution is counted loudly in a per-net
diagnostic channel surfaced by `solve`/`step` and cleared by `reset`.
Storage is three additive dense-flat arrays in `Netlist` (primary
driver slot unchanged); the workspace schema grows additively
(optional `shared:` section, repeated wire rows) with no generation
retirement; the optimizer freezes shared-feeding gates (O0/O1) and
refuses O2 loudly; the word-parallel path (`truth`) refuses ≥2-
attachment shared nets. All decisions are anchored in
[research.md](research.md) (D1–D11).

## Technical Context

**Language/Version**: C++20, GCC 13.3 baseline (zero-warning target
with the `wengine_cli` flag set).

**Primary Dependencies**: existing only — engine header-only
`INTERFACE` libraries (`wengine_core`), `wengine_cli` (Shell + ArgCursor),
`wengine_logic_yaml` (the single yaml-cpp TU, vendored 0.8.0). No new
external or vendored dependency.

**Storage**: in-memory `Netlist` (+~80 KB fixed arrays: `shared_meta`,
`extra_drivers`, `extra_inv` — dense-flat SoA per the `tie_rep`/
`driver_inv` precedent); versioned YAML workspace, additive optional
constructs, marker stays `version: 1` (research D5). Contention
counters are CLI-session state (`SolveDiag`), never serialized.

**Testing**: plain-`assert` binaries via ctest; new
`tests/test_logic_shared.cpp` (engine seam → verbs → YAML round-trip);
extensions to `test_logic.cpp` (span resolution near the funnel
region), `test_cli_args.cpp` (registry 22→23, `wire --shared`,
`unshare`, completion), `test_logic_yaml.cpp` (shared schema + unchanged
refusals); registry/`static_assert` syncs as listed in
contracts/cli-verbs.md; `wengine_test_assert` policy inherited via the
`test_` prefix.

**Target Platform**: Linux (WSL2 baseline), headless CLI + optional
TUI (no TUI change required — parity by construction).

**Project Type**: C++20 physics/logic simulation engine + CLI modules.

**Performance Goals**: `bench_logic` ns/gate-word unchanged within
noise for unmarked netlists (research D11 protocol: recorded baseline,
same-args re-run, D11 contingency if measurable). Scalar `n ≤ 1` path
structurally untouched.

**Constraints**: hot paths exception-free/allocation-free (stack span
buffer, fixed cap 4); determinism (order-independent spans, pinned
promotion/compaction, observation-only counting); no
serialization-format drift for existing content (byte-identical
loads/saves/goldens for mark-free workspaces).

**Scale/Scope**: `kMaxGates` 1024, `kMaxInputs` 8, `kMaxSharedExtra` 3
(4 attachments per shared input, loud overflow); `LogicRegistry`
22 → 23 verbs; one new example YAML joins `examples/logic/`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Pre-Phase-0 | Post-Phase-1 (design) |
|---|---|---|
| I. Physical/numerical correctness | Resolution semantics are spec 010's shipped, IEEE-1164-anchored table; this feature adds authoring + observability, no new value vocabulary | PASS — span tests extend the same independent reference through the netlist seam (engine-seam.md "Reference anchoring"); counting is defined at span assembly, not inferred from X |
| II. Determinism & reproducibility | `resolve_net` is order-independent (shipped); counters must not touch state/serialization | PASS — pinned span assembly + promotion/compaction orders; `SolveDiag` is observation-only (null channel = identical bytes); counters never serialized; single-driver path byte-identical |
| III. Independent-reference testing | Multi-boundary verification is the house norm | PASS — engine seam → CLI verbs → YAML round-trip; new binary + extended binaries; registry `static_assert`s enumerated for sync; benchmark excluded from assertion policy |
| IV. Measured performance only | Done-criterion demands an unchanged fast path in bench | PASS — D11 protocol: `bench_logic` baseline + re-run + named contingency; claims stay measured |
| V. Simplicity, minimal scope, dependency discipline | Header-only engine modules, single yaml-cpp TU, one command domain | PASS — additive flat arrays (no new TU); schema work inside the existing store seam; one verb + one option; no dependency-direction change; no new `GateKind` |
| Logic-domain constraints (append-only kinds, single-driver rule, optimizer opacity, loud YAML) | All named in the constitution | PASS — default single-driver refusal preserved byte-identically; optimizer freeze follows the `feeds_seq` pattern with O2 loud refusal; the YAML decision (no retirement) is the documented in-generation additive convention (`ignore_nq` precedent), recorded loudly in docs — no fallback loader question arises because nothing is retired |

**Gate result**: no violations; no Complexity Tracking entries
required.

## Project Structure

### Documentation (this feature)

```text
specs/011-multi-driver-nets/
├── spec.md              # The feature specification (/speckit.specify output)
├── plan.md              # This file (/speckit.plan output)
├── research.md          # Phase 0: decisions D1-D11, all clarifications resolved
├── data-model.md        # Phase 1: entities, storage, serialization model
├── quickstart.md        # Phase 1: runnable validation scenarios
├── contracts/
│   ├── engine-seam.md   # mark lifecycle, span resolution, diagnostics, optimizer policy
│   ├── cli-verbs.md     # wire --shared / unshare / solve+step readback / completion / docs duties
│   └── workspace-yaml.md# additive schema, canonical bytes, loader rules, test pins
├── checklists/
│   └── requirements.md  # spec quality validation (complete)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
include/wengine/logic/
├── types.hpp             # Netlist +shared_meta/+extra_drivers/+extra_inv, kMaxSharedExtra=3
├── netlist.hpp           # wire()/unwire()/remove_gate() shared branches; share/unshare engine fns
├── block_interface.hpp   # resolve_input span assembly + SolveDiag (the single funnel)
├── eval.hpp              # solve(..., SolveDiag* = nullptr)
├── seq_step.hpp          # seq_step_filtered(..., SolveDiag* = nullptr)
├── optimize*.hpp         # drives_shared freeze predicate; extras-aware recount/reachability;
|                         #   exact conservative skip; O2/O2-min loud refusal
└── yaml_store.hpp        # additive shared-schema declarations (header stays platform-free)
src/logic/yaml_store.cpp   # optional shared: section, repeated to: rows, loud validation
include/wengine/cli/logic_registry.hpp   # LogicWireSpec usage/opts; LogicUnshareSpec; registry 22->23
src/cli/commands/logic.cpp             # SolveDiag session singleton + reset clears it
src/cli/commands/logic/wire.cpp         # --shared flag, completion, probe/revert extension
src/cli/commands/logic/unshare.cpp      # new verb
src/cli/commands/logic/{solve,step,list}.cpp  # contention readback + mark inspectability
tests/test_logic_shared.cpp              # NEW: end-to-end seam binary (wengine_cli + wengine_logic_yaml)
tests/test_logic.cpp                     # span-resolution unit tests beside the funnel region
tests/test_cli_args.cpp                  # registry 22->23, wire --shared/unshare goldens, completion
tests/test_logic_yaml.cpp                # shared round-trip; unchanged refusals stay green
examples/logic/shared_bus.yaml           # example golden (joins test_examples automatically)
benchmarks/bench_logic.cpp               # unchanged; measured before/after (research D11)
docs/logic.md, docs/tests/logic.md, README.md   # living-spec duties per contracts/cli-verbs.md
```

**Structure Decision**: single-project layout unchanged; all engine
edits are additive members/functions inside existing headers (modules
stay header-only `INTERFACE`); the YAML platform TU and CLI verb TUs
follow the existing per-file pattern; one new test binary and one new
verb TU; no new directories.

## Complexity Tracking

No constitution violations to justify — the table is intentionally
empty. The one deliberate scope trade (O2 loud refusal instead of a
verbatim-carry emitter) is a documented conservative-refusal decision
(research D6), not a complexity exception; its follow-on is named in
docs/logic.md §12.
