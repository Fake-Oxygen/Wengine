# Data Model: SR/D Latch Pair

**Feature**: `specs/004-sr-d-latches/spec.md` | **Date**: 2026-11-01

No new structs. The model extends the existing `Gate` pin vocabulary in
place (`types.hpp` + `netlist.hpp`); state shape (`SeqState`) and snapshot
mechanics are untouched — the stepper gains one level-settle pass
(`research.md` R4) plus one edge-exemption guard. Research reference:
`research.md` R1–R7.

## Entity 1 — D Latch Pins (existing vocabulary, refined semantics)

Per-latch inputs at fixed indices (DLATCH only, fan-in exactly 2 —
unchanged):

| Index | Name | Role | Active level | Dangling default |
|-------|------|------|--------------|------------------|
| 0 | `D` | data | High = 1 | `kLow` (inactive) |
| 1 | `EN` | level enable (NOT a clock) | High = transparent | `kLow` (inactive → frozen) |

Outputs: `Q` (stored value), `NQ` (live NOT Q, same invariant as flops).

**Validation rules**:
- `valid_fanin(kDlatch)` = 2 only; `default_fanin(kDlatch)` = 2 (unchanged).
- Named matching case-insensitive; numeric aliases `.1`/`.2` accepted;
  `D`/`EN` on non-DLATCH kinds rejected (existing wrong-kind rule).
- Only `kLow`/`kHigh` accepted on `D`/`EN` nets; `kHighZ`/`kUnknown`
  rejected loudly with no state commit (existing X/Z rule).
- Wired inputs ignore the poke/`ignore` mark (wire wins — existing rule);
  tied `D`/`EN` pins share one leader value (existing tie classes).

**Relationships**: each `D`/`EN` pin belongs to exactly one DLATCH slot;
drive sources are block inputs (poke map), gate outputs (wire incl. NQ
polarity), or ties — same as `D`/`CLK` today. Each latch owns one
`SeqState` Q slot + one stable serial (existing attach path, unchanged).

## Entity 2 — SR Latch Pins (extends existing pin model)

Per-latch inputs at fixed indices (SRLATCH only, fan-in exactly 2):

| Index | Name | Role | Active level | Dangling default |
|-------|------|------|--------------|------------------|
| 0 | `S` | async set | High | `kLow` (inactive → hold row) |
| 1 | `R` | async reset | High | `kLow` (inactive → hold row) |

Outputs: `Q`, `NQ` (same as above). Validation/relationships mirror Entity
1 with `valid_fanin(kSrlatch)` = 2, `default_fanin(kSrlatch)` = 2, aliases
`.1`/`.2`. No clock or enable pin in this feature (hold is `S=R=0`).

## Entity 3 — Resolved Latch Output (level semantics)

`Q(slot)` after every level settle, resolved by one total function per
kind from settled nets + OLD Q (parallel update among latches):

```text
resolve_dlatch(d, en, q_old):
  if en: return d    # transparent follow
  return q_old       # frozen hold (D ignored)

resolve_sr(s, r, q_old):
  if s && r: return ILLEGAL   # refuse loudly, no commit (any member)
  if s: return High           # set
  if r: return Low            # reset
  return q_old                # hold (S=R=0, memory)
```

`NQ` = NOT `Q` at every observation point (existing live-negation
invariant, extended to SR and to post-level-change observations).
Power-on: `Q=kLow` (unchanged `init_seq_state` path); serials never reused
(unchanged).

**State transitions**:
- `poke DLATCH1.D=1` with `EN=1` → Q follows at once (no `step`);
  `poke DLATCH1.D=0/1` with `EN=0` → Q unchanged no matter how often.
- `poke SRLATCH1.S=1` (R=0) → Q=1 at once; `poke SRLATCH1.R=1` (S=0) → Q=0 at
  once; `poke` completing `S=R=1` → usage error, Q unchanged, drive not
  committed.
- `step N` with frozen latches → Q unchanged across all N edges;
  transparent D latch → Q tracks current `D` at every step top while EN
  stays active; edge flops in the same `step` follow their own tables.

## Entity 4 — Transparent/Frozen + SR Rules (normative statements)

Single ordered sentences shipped in help + `docs/logic.md` (contracts C6):

> `DLATCH: EN=1 transparent (Q follows D); EN=0 frozen (Q holds, D ignored). No clock advance required.`
> `SRLATCH: S=1,R=0 set (Q=1); S=0,R=1 reset (Q=0); S=0,R=0 hold. S=1,R=1 is illegal and refused with state unchanged.`
> `Level latches belong to no clock domain: advances never gate their Q. Unconnected D/EN/S/R read 0 (hold).`

Observable consequence (testable): for every row of each table × Q_old,
engine output, CLI readback, and replayed-file readback agree; the
illegal row fails identically on every path.

## Persistence mapping (YAML `version: 2`, unchanged schema)

- SR rows write `kind: srlatch, inputs: 2` with canonical `ignore:`
  indices over 1..2; DLATCH rows unchanged (`kind: dlatch, inputs: 2`).
- Wires to `SRLATCHn.S|R` persist as named `from:`/`to:` endpoints; loader
  resolves them through the extended `parse_pin`.
- Poke values are session-only (never serialized — existing rule);
  `SeqState` Q values are session-only (existing rule).
- Legacy files (no SR rows; DLATCH rows with edge-era expectations) load
  structurally unchanged — SR-free netlists step identically except for
  the documented DLATCH edge-to-level refinement (spec SC-004).
- Unknown kind spellings fail loudly with line numbers (existing rule).
