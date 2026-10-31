# Research: DFF Control Set

**Feature**: `specs/002-dff-control-set/spec.md` | **Date**: 2026-10-31

All unknowns resolved — no NEEDS CLARIFICATION remains.

## R1. How to carry RST/SET/EN: fan-in extension vs. sidecar attributes

**Decision**: Extend DFF fan-in from exactly-2 to 2-or-5 with fixed
positions `D`=0, `CLK`=1, `RST`=2, `SET`=3, `EN`=4 (`valid_fanin(DFF)` =
`{2, 5}`).

**Rationale**: The `Gate` struct already supports `kMaxInputs`=8 and the
whole session stack (wire/poke/tie/ignore/`use`, block-interface
derivation, `compose_block_inputs`, YAML `ignore:` lists, NQ polarity) is
pin-index-driven. New pins therefore inherit every verb for free via
`parse_pin` + `pin_text`. A sidecar attribute scheme would need a parallel
drive/wire/persist stack and would violate FR-007.

**Alternatives considered**:
- Sidecar bools on `Gate` (e.g. `has_rst`) — rejected: duplicates the
  pin machinery, breaks wireability, violates minimal-scope (new
  concepts instead of reusing pins).
- New gate kind (e.g. `DFFR`) — rejected: combinatorial explosion for
  every control subset; spec wants one flop with one priority rule.

**Backward compatibility**: `inputs: 2` files keep loading (new pins
dangling → inactive per the existing ignored-dangling-ties-to-inactive
rule). Writer emits `inputs: 5` for DFFs going forward. No YAML schema
version bump (still `version: 2`); the loader accepts both counts.

## R2. Async immediacy: when does Q change?

**Decision**: Two paths, one resolver:
1. **Immediate path** — new engine helper `apply_async_controls(nl, st,
   block_inputs)` (slot order, `noexcept`, no allocation): resolves
   `RST`/`SET` nets for every live DFF and forces `Q` per priority
   (RST→0, else SET→1). Called from session code after every mutation
   that can change control nets (poke, wire, unwire, tie/untie, load) and
   at the top of `step` before edge looping. Never touches cycle counters.
2. **Step path** — `seq_step_filtered` snapshot phase resolves
   RST/SET/EN per flop in slot order *before* D capture and applies the
   same priority, so an edge while controls are held is a deterministic
   no-op-or-force rather than a capture.

**Rationale**: Spec FR-002/FR-003 require Q to move with no clock advance.
Poke only writes the block-input map; Q lives in `SeqState`. Without an
explicit apply step, async assertion would sit latent until the next edge
— violating the spec. A single shared resolver keeps both paths
identical (no drift between immediate and stepped outcomes).

**Alternatives considered**:
- Edge-only async (controls sampled at `step`) — rejected: violates
  "immediately (with no clock advance required)".
- Continuous evaluation on every `peek` — rejected: observation must not
  mutate state (peek stays read-only; determinism gate).

## R3. Priority order and simultaneous-assertion outcome

**Decision**: Total order `RST > SET > EN-hold > D`, reset wins
simultaneous RST+SET (Q=0). Hand-derived truth table (independent
reference for tests):

| RST | SET | EN | D | Q_next / Q_now |
|-----|-----|----|---|----------------|
| 1 | * | * | * | 0 |
| 0 | 1 | * | * | 1 |
| 0 | 0 | 0 | * | hold Q |
| 0 | 0 | 1 | d | d |

(`*` = don't care; dangling RST/SET read 0/inactive, dangling EN reads
1/asserted per the inactive-default rule.)

**Rationale**: Industry-standard (reset dominates; preset next; enable
gates only the synchronous capture). Reset-wins keeps power-on/reset
value 0 the safe default for counters and FSM idle states, matching the
spec assumption.

**Alternatives considered**: SET-wins — rejected (non-standard, risks
power-on ambiguity); EN-overrides-async — rejected (async must work
before any clock exists, e.g. reset with clock unwired).

## R4. Enable semantics: synchronous hold

**Decision**: `EN` is evaluated at the edge like `D`. `EN=0` → flop
holds Q for that edge (snapshot skipped, commit keeps old Q); `EN=1` (or
dangling) → normal D capture. `EN` never affects the immediate async
path (async overrides enable per the priority order).

**Rationale**: Matches the spec assumption (sync enable / async
controls split) and the existing SEQ4 "edge itself is the call" model:
CLK is validated-not-gating today, and EN slots into the same snapshot
phase with an actual gating effect.

## R5. Selective clock domains vs. async scope

**Decision**: Async-apply is global (all live DFFs, slot order),
independent of `seq_step_filtered` member subsets. Enable-hold and D
capture remain per-member (subset stepping only snapshots/commits
members; non-members hold Q as today).

**Rationale**: Async controls are physical pins, not clock edges — a
board reset hits every flop regardless of which clock domain is being
stepped. Scoping async to step members would make reset behavior depend
on which domain the user stepped last (non-deterministic from the user's
perspective). Global async + per-member sync is the least surprising
rule and stays deterministic.

## R6. DLATCH scope

**Decision**: DLATCH untouched (`D`=0, `EN`=1, fan-in exactly 2). Name
collision is only textual: DLATCH input 1 keeps displaying `EN`
(level-gate), DFF input 4 displays `EN` (clock enable). `parse_pin` and
`pin_text` branch on kind first, so no ambiguity.

**Rationale**: Spec scopes to DFF only; DLATCH level semantics arrive in
SEQ8 per the `seq_step.hpp` header roadmap. Touching DLATCH would
expand scope and risk the SEQ8 design space.

## R7. What changes per file (grounded in current tree)

- `include/wengine/logic/netlist.hpp`: `valid_fanin` (DFF `{2,5}`),
  `default_fanin` stays 2 (bare `add DFF` = D-only, zero-migration),
  `parse_pin` named `RST`/`SET`/`EN` on DFF (indices 2/3/4),
  case-insensitive, numeric aliases `.3`/`.4`/`.5` keep working.
- `include/wengine/logic/seq_step.hpp`: snapshot resolves RST/SET/EN
  and applies R2/R3 priority; new `apply_async_controls` helper; commit
  skips held/disabled flops; X/Z on any resolved control fails loudly
  without committing (existing rule extended to 3 more pins).
- `src/logic/yaml_store.cpp`: writer canonical names for inputs
  2/3/4; loader accepts `inputs: 2|5` for DFF and validates ascending
  unique `ignore:` indices over the new range.
- `src/cli/commands/logic/*`: `poke`/`wire`/`tie`/`load` call
  `apply_async_controls` after mutation; `step` applies async first,
  then edges; transition report shows forced/hold outcomes in existing
  `Q: a->b` form; help/completion gain pin names + priority line.
- `docs/logic.md` + `README.md`: priority statement, pin table, run
  examples, test list.
