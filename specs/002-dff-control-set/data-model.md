# Data Model: DFF Control Set

**Feature**: `specs/002-dff-control-set/spec.md` | **Date**: 2026-10-31

No new structs. The model extends the existing `Gate` pin vocabulary in
place; state shape (`SeqState`) is untouched.

## Entity 1 — DFF Control Pin (extends existing pin model)

Per-flop inputs at fixed indices (DFF only):

| Index | Name | Role | Active level | Dangling default |
|-------|------|------|--------------|------------------|
| 0 | `D` | data capture | — | `kLow` (existing rule) |
| 1 | `CLK` | clock (validated, not gating — SEQ4) | — | `kLow` (existing rule) |
| 2 | `RST` | async reset | High | inactive = `kLow` (no reset) |
| 3 | `SET` | async set/preset | High | inactive = `kLow` (no set) |
| 4 | `EN` | clock enable (synchronous) | High | asserted = `kHigh` (enabled) |

**Validation rules**:
- `valid_fanin(kDff)` = 2 or 5 only; `default_fanin(kDff)` stays 2 (bare
  `add DFF` is D-only — zero-migration).
- Named matching case-insensitive (`rst`, `Rst`, `RST`); numeric aliases
  `.3`/`.4`/`.5` accepted; named pins on combinational kinds rejected
  (existing rule).
- Only `kLow`/`kHigh` accepted on control nets; `kHighZ`/`kUnknown`
  rejected loudly with no state commit (existing X/Z rule extended).
- Wired inputs ignore the poke/`ignore` mark (wire wins — existing rule);
  tied control pins share one leader value (existing tie classes).

**Relationships**: each control pin belongs to exactly one DFF slot;
each DFF has at most one of each control pin; drive sources are block
inputs (poke map), flop outputs (wire incl. NQ polarity), or ties —
same as `D`/`CLK` today.

## Entity 2 — Resolved Flop Output (extends Q semantics)

`Q(slot)` after any mutation or edge, resolved by one total function:

```text
resolve_q(rst, set, en, d, q_old):
  if rst == High: return Low        # async reset dominates everything
  if set == High: return High       # async set next
  if en  == Low:  return q_old      # sync hold (edge path only)
  return d                          # normal capture (edge path only)
```

Immediate (async) path evaluates only the first two rows; the edge path
evaluates all four. `NQ` = NOT `Q` at every observation point (existing
live-negation invariant, extended to post-async reads).

**State transitions**:
- `poke RST=1` / `wire` active driver → `Q=0` at once (no cycle-count
  change); release → Q stays until next force/edge.
- `poke SET=1` → `Q=1` at once; RST+SET → `Q=0` (reset wins).
- `step N` with `EN=0` → `Q` unchanged across all N edges; siblings with
  `EN=1`/dangling step normally.
- Power-on: `Q=kLow` (unchanged); `init_seq_state` attach path unchanged;
  serials never reused (unchanged).

## Entity 3 — Priority Rule (normative statement)

Single ordered sentence shipped in help + `docs/logic.md`:

> `RST (async) > SET (async) > EN-hold (sync) > D capture; RST+SET → Q=0.`

Observable consequence (testable): for every one of the 2³ async/EN
combinations × D × Q_old rows in the research truth table, engine
output, CLI readback, and replayed-file readback agree.

## Entity 4 — Stall Subset (derived, not stored)

The set of DFF slots with resolved `EN == kLow` at an edge. Not persisted
anywhere; derived fresh each snapshot in slot order. Intersects
orthogonally with clock-domain member subsets (`seq_step_filtered`):
a flop steps iff it is a step member AND (`EN` active OR async asserted).

## Persistence mapping (YAML `version: 2`, unchanged schema)

- DFF rows write `inputs: 5` with canonical `ignore:` indices over 1..5;
  legacy `inputs: 2` rows load with pins 2–4 dangling-inactive.
- Wires to `DFFn.RST|SET|EN` persist as named `from:`/`to:` endpoints;
  poke values are session-only (never serialized — existing rule);
  `SeqState` Q values are session-only (existing rule).
