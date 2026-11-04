# Research: Versioned Workspace v1 (Flop Inits + Clock-Domain Metadata)

**Feature**: specs/006-versioned-workspace-v1/spec.md
**Date**: 2026-11-04
**Method**: Codebase inspection (no external research needed — all decisions are internal consistency questions).

## R1 — Schema version marker: new-generation `version: 1`

- **Decision**: The new schema generation uses the literal marker `version: 1`. Acceptance is marker **plus** required-section presence: a `version: 1` file without the new required sections (`clocks:` at minimum) is refused as a legacy file, never misloaded.
- **Rationale**: The request states "Versioned workspace v1" and "legacy versions v1/v2 dropped completely" literally, and the requester attests legacy will never load again. Reusing the numeral with a section-presence gate is unambiguous in practice (legacy v1 bytes lack `ties:`/`clocks:`; legacy v2 bytes lack `clocks:`), keeps diagnostics simple ("want 1"), and needs no migration tooling.
- **Alternatives considered**: Bumping to `version: 3` (next integer after current 2) — rejected: contradicts the explicit "v1" instruction and gains nothing once legacy support is fully removed. A distinct root key (e.g. `workspace: 1`) — rejected: gratuitous schema churn; the section gate already disambiguates.

## R2 — Init representation: per-gate optional `init:` key on sequential rows

- **Decision**: Each sequential gate row may carry `init: 0|1` (default `0` when absent). Combinational rows carrying `init` are a loud load error. The saver always writes the key explicitly on every sequential row.
- **Rationale**: Direct precedent — `ignore_nq` is already a sequential-only optional 7th gate key defaulting when absent, keeping combinational rows at their canonical 6-field bytes. Inits are per-element attributes, so they belong on the row; validation stays local in `check_gate_map`. Asymmetric read/write (tolerant read, explicit write) mirrors the existing `ignore_nq` and comment-superset precedents.
- **Alternatives considered**: Top-level `inits:` section (`- {gate: DFF1, init: 1}`) — rejected: indirection for per-element data, ordering/duplication questions, no precedent (relational sections like `ties:` exist precisely for cross-gate facts, which inits are not).

## R3 — Clock metadata: top-level `clocks:` section, name-only entries, set-equality validation

- **Decision**: `clocks:` is a required top-level section (may be empty for combinational nets) with one entry per derived domain: `- {name: BUF1.Y}` (or `- {name: clk}` for the implicit dangling-clock domain). The loader re-derives domains from the stored topology and requires exact set-equality with the stored names; any mismatch is a loud load error naming the domain.
- **Rationale**: Precedent — relational facts (`ties:`) live in top-level sections. Derived names already equal the source-pin spelling (`BUF1.Y`, `DFF2.NQ`) or `clk`, so the name *is* the source binding (a name without a dot can only be the implicit domain, since pin spellings always contain a dot). Membership stays derived (single source of truth, FR-007); persisting the set pins it, so hand-edit drift between topology and domains fails loudly instead of silently re-deriving. Required-even-when-empty gives a uniform acceptance gate that also disambiguates new-v1 from legacy bytes (R1).
- **Alternatives considered**: Persisting members per domain — rejected: redundant with topology, invites divergence, doubles validation surface. User-renameable aliases (`sysclk` → source) — rejected: needs alias resolution in `step`, `--seq-clocks`, completion, and cycle-count keys; the spec asks for stable names bound to sources, which derivation already provides.

## R4 — Save semantics: saver persists live pool Q as the file's inits

- **Decision**: `save` writes each sequential element's current live pool Q as its `init:` value. No new verb is introduced for setting inits; the flow is poke/step to the desired state, then save.
- **Rationale**: Minimal scope (constitution V — no new commands, no registry-size assertion churn). Save/load/save stays byte-identical (load powers on to inits, immediate re-save writes the same values). Matches checkpoint intuition: saving captures the workspace *state*, and matches the session model where the pool is the live truth beside the workspace. Hand-built never-stepped nets resync at Low, so their files carry all-zero inits — the documented default.
- **Alternatives considered**: Saving the *loaded* inits (design intent) while ignoring live Q — rejected: needs a separate remembered-inits store plus a new verb to author inits interactively, otherwise mixed-init files could only be hand-written. New `init PIN=0|1` verb — rejected as unnecessary once save-captures-Q is chosen.

## R5 — Load pool handling: load starts a new pool lifetime

- **Decision**: Successful load resets the session pool (serials restart from 0), resyncs to the new topology, applies the file's inits to every sequential slot, then runs the existing async-control/level-settle application. Failure (including the post-swap illegal-SR check) restores both workspace *and* pool, preserving atomicity.
- **Rationale**: A load replaces the workspace; carrying old Q/serials into overlapping slots of a different net is unsound identity-wise. Reset+apply is deterministic (serials 0-based in slot order) and makes "power-on to inits" (FR-008) exact. The existing `apply_async_and_report` already snapshots/restores the pool around its own work; load extends the snapshot to cover the reset so FR-011 atomicity spans workspace and pool.
- **Alternatives considered**: Idempotent resync preserving still-valid slots' Q, then overriding all sequential Q with inits — same observable Q but stale serials from a previous workspace leak across loads; rejected as unsound identity.

## R6 — Domain derivation moves to the engine (new header-only header)

- **Decision**: Add header-only `include/wengine/logic/clock_domains.hpp` with a pure derive function (Netlist → ordered domain list with slot-ordered members, same vocabulary: source-pin spelling, `clk` implicit, edge kinds DFF/JK/T only, first-seen slot order). The session `step` verb, `sim --seq-clocks` lowering, completion, and the loader's `clocks:` validation all use it; the two existing CLI-local copies are replaced.
- **Rationale**: Concrete need, not abstraction for its own sake: the file pins what stepping uses, so derivation must have exactly one implementation or file and stepper can drift. Header-only keeps the engine INTERFACE-only rule; the function is session/file-path code (strings/vectors), never hot-path, so engine string/alloc avoidance rules don't apply (precedent: `yaml_store.hpp` already uses `std::string`). No yaml-cpp involvement — the seam stays intact.
- **Alternatives considered**: Keeping the two CLI copies and duplicating a third derivation inside the yaml TU — rejected: three copies of grouping semantics is exactly the drift hazard. Validating `clocks:` only for shape (non-empty/unique names) at the loader — rejected: shape-only checks make the section write-only decoration with no drift detection.

## R7 — Loader API: bundle struct, Netlist untouched

- **Decision**: `yaml_store.hpp` gains a plain-data `Workspace` bundle (`Netlist netlist` + per-slot init array, `kLow`-defaulted) with `load_workspace(text, bundle, err)` and `save_workspace(netlist, inits, out)`. The existing `load_text`/`save_text` Netlist-only signatures are removed (no legacy callers may remain per FR-015). `Gate`/`Netlist` gain no fields (docs/logic.md §12 "Netlist never reshapes").
- **Rationale**: Inits ride beside the topology exactly like `SeqState` rides beside `Netlist` at runtime (established pattern). One canonical writer preserves byte-pinning; session and sim drivers both consume the same bundle.
- **Alternatives considered**: Adding an `init_q` field to `Gate` — rejected: reshapes `Netlist`, touches every constructor/copy/golden, violates the §12 additive-only rule for file-only data.

## R8 — Engine (`sim run --seq-file`) path honors inits

- **Decision**: `SequentialRunConfig` gains one parsed-values init field (per-slot power-on values, same shape as the bundle array); `sim.cpp` fills it from the loaded bundle; the configured-run driver applies it to the pool right after `init_seq_state` (fresh serials, mirroring session load).
- **Rationale**: Same file must power on identically in session and engine runs (FR-008 + determinism principle); otherwise `--seq-file` silently diverges from `load`. The field lives inside the existing nested section (one-section-per-solver rule preserved — no flat fields). Default (direct-constructed configs in tests) is all-Low, so existing runner behavior is bit-identical.
- **Alternatives considered**: Session-only inits — rejected: observable session/engine divergence on identical files.

## R9 — Missing `init` defaults Low on load; saver always writes explicit

- **Decision**: Asymmetric (see R2): absent key reads as `0`, saver emits `0|1` on every sequential row. Non-binary values are loud errors.
- **Rationale**: Keeps hand-authored files small while canonical output stays fully explicit and byte-pinnable. A hand file missing some inits round-trips from the second save — the exact documented caveat of the existing comment-superset behavior.

## R10 — `clocks:` required even when empty; full required-section set

- **Decision**: New-schema acceptance requires `version: 1` + `gates:` + `wires:` + `ties:` + `clocks:` + `meta:`. Unknown keys/sections remain loud errors everywhere, including inside the two new surfaces.
- **Rationale**: Uniform gate (simplest loader invariant), and the presence of `clocks:` is what separates new-v1 from both legacy generations (R1/FR-004).

## R11 — Explicitly out of scope

- **Decision**: World binary checkpoints (serialization v6 with `seq_*` rows) are untouched — they store live run state, not workspace files. `link::Frame` adapter, CSV sinks, `peek`/`step` stepping semantics, and GUI are untouched; they consume state that now simply powers on differently.
- **Rationale**: Spec FR-012 + minimal scope. Verified: checkpoint save/load paths never touch `yaml_store`.

## R12 — No new commands, verbs, options, or registries change size

- **Decision**: No `static_assert` registry updates (command, IC, panel, frame-source registries all keep their sizes). Usage-text updates only where they name versions (`save`/`load` specs, sim `--seq-file` help).
- **Rationale**: R4 removes the only candidate for a new verb; everything else is behavior/format change under existing spellings.

## R13 — Test strategy: extend, don't fork

- **Decision**: Extend `test_logic_yaml.cpp` (canonical bytes, superset, refusals incl. legacy-v1-shaped/v2/foreign, init round-trips, clocks mismatch cases, 8-file examples test), update version-pinned fixtures in `test_cli_args.cpp` / `test_logic_seq_controls.cpp` / `test_logic_seq_cli_step.cpp`, add engine-init coverage to `test_logic_seq_runner.cpp` via direct section construction, and rework `examples/logic/*` (5 converted + 3 new) with the existing header Run/Expect convention. No new test target (keeps the `test_` prefix policy trivially satisfied).
- **Rationale**: The existing tests already own these seams; forking new targets would duplicate fixtures. The examples test already enumerates files by name — extending the name list keeps the "shipped files stay loadable" pin.

## R14 — Documentation updates

- **Decision**: `docs/logic.md` §9 (schema: version 1, `init` key, `clocks:`, canonical bytes, refusal table) and §12 (schema-evolution entry: v1 new generation, legacy retired, no fallback), `README.md` workspace paragraph (~line 564), and the `save`/`load` usage strings in `logic_registry.hpp`.
- **Rationale**: README is a living specification; stale "version: 2 / v1 fallback" statements would otherwise contradict behavior. Prior-spec docs under `specs/00N/` are historical records and are left untouched.
