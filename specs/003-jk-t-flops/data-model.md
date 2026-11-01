# Data Model: JK/T Flop Kinds

**Feature**: `specs/003-jk-t-flops/spec.md` | **Date**: 2026-11-01

No new structs. The model extends the existing `Gate` pin vocabulary in
place (`types.hpp` + `netlist.hpp`); state shape (`SeqState`) and snapshot
mechanics are untouched — the stepper gains one excitation branch per new
kind. Research reference: `research.md` R1–R6.

## Entity 1 — JK Flop Pins (extends existing pin model)

Per-flop inputs at fixed indices (JK only, fan-in exactly 3):

| Index | Name | Role | Active level | Dangling default |
|-------|------|------|--------------|------------------|
| 0 | `J` | sync set/toggle arm | High | `kLow` (inactive → hold row) |
| 1 | `K` | sync reset/toggle arm | High | `kLow` (inactive → hold row) |
| 2 | `CLK` | clock (validated, not gating — SEQ4 rule) | — | `kLow` (existing rule) |

Outputs: `Q` (stored value), `NQ` (live NOT Q, same invariant as DFF).

**Validation rules**:
- `valid_fanin(kJk)` = 3 only; `default_fanin(kJk)` = 3 (`add JK` needs no
  `--inputs`; any other count rejected as "bad fan-in for kind").
- Named matching case-insensitive (`j`, `K`, `clk`); numeric aliases
  `.1`/`.2`/`.3` accepted; J/K/CLK on non-JK kinds rejected (existing
  wrong-kind rule); named pins on combinational kinds rejected (existing).
- Only `kLow`/`kHigh` accepted on J/K/CLK nets; `kHighZ`/`kUnknown`
  rejected loudly with no state commit (existing X/Z rule).
- Wired inputs ignore the poke/`ignore` mark (wire wins — existing rule);
  tied J/K pins share one leader value (existing tie classes).

**Relationships**: each J/K/CLK pin belongs to exactly one JK slot; drive
sources are block inputs (poke map), flop outputs (wire incl. NQ polarity),
or ties — same as `D`/`CLK` today. Each JK flop owns one `SeqState` Q slot
+ one stable serial (existing attach path, unchanged).

## Entity 2 — T Flop Pins (extends existing pin model)

Per-flop inputs at fixed indices (T only, fan-in exactly 2):

| Index | Name | Role | Active level | Dangling default |
|-------|------|------|--------------|------------------|
| 0 | `T` | sync toggle control | High | `kLow` (inactive → hold) |
| 1 | `CLK` | clock (validated, not gating — SEQ4 rule) | — | `kLow` (existing rule) |

Outputs: `Q`, `NQ` (same as above). Validation/relationships mirror Entity
1 with `valid_fanin(kT)` = 2, `default_fanin(kT)` = 2, aliases `.1`/`.2`.

## Entity 3 — Resolved Flop Output (extends Q semantics)

`Q(slot)` after each edge, resolved by one total function per kind from
OLD Q (parallel update — toggle reads pre-edge Q):

```text
resolve_jk(j, k, q_old):
  if !j && !k: return q_old   # hold
  if !j &&  k: return Low     # reset
  if  j && !k: return High    # set
  return NOT q_old            # toggle (j && k)

resolve_t(t, q_old):
  if !t: return q_old         # hold
  return NOT q_old            # toggle
```

`NQ` = NOT `Q` at every observation point (existing live-negation
invariant, extended to the new kinds at power-on and after every step).
Power-on: `Q=kLow` (unchanged `init_seq_state` path); serials never reused
(unchanged).

**State transitions**:
- `poke JKn.J=1` + `step 1` → Q follows the table row; no immediate change
  before the edge (synchronous-only — no async-apply involvement).
- `poke Tn.T=1` + `step 4` → Q toggles 4 times (divider); `T=0` + `step N`
  → Q unchanged across all N edges.
- Mixed `step N` over DFF+JK+T → each slot follows its own resolver from
  the same OLD-Q vector; single shared cycle count.

## Entity 4 — Excitation Table (normative statements)

Single ordered sentences shipped in help + `docs/logic.md` (contracts C6):

> `JK: J=0,K=0 hold; J=0,K=1 reset (Q=0); J=1,K=0 set (Q=1); J=1,K=1 toggle.`
> `T: T=0 hold; T=1 toggle (Q=NOT Q). Evaluated at the clock edge from pre-edge Q.`

Observable consequence (testable): for every row of each table × Q_old,
engine output, CLI readback, and replayed-file readback agree.

## Persistence mapping (YAML `version: 2`, unchanged schema)

- JK rows write `kind: jk, inputs: 3` with canonical `ignore:` indices over
  1..3; T rows write `kind: t, inputs: 2` with indices over 1..2.
- Wires to `JKn.J|K|CLK`, `Tn.T|CLK` persist as named `from:`/`to:`
  endpoints; loader resolves them through the extended `parse_pin`.
- Poke values are session-only (never serialized — existing rule);
  `SeqState` Q values are session-only (existing rule).
- Legacy files (no JK/T rows) load exactly as before — zero migration;
  unknown kind spellings fail loudly with line numbers (existing rule).
