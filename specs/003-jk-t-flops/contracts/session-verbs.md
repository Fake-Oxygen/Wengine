# Contracts: JK/T Flop Kinds

**Feature**: `specs/003-jk-t-flops/spec.md` | **Date**: 2026-11-01

Session-verb contracts (user-observable). Engine signatures are sketched
in `data-model.md`; exact C++ spelling lands in `tasks.md`/implementation.

## C1. Named pins (extends FR-010/FR-011 vocabulary)

New kinds need no opt-in (`add JK` ≡ 3 inputs, `add T` ≡ 2 inputs):

```text
JKn.J    input 0   sync J arm, active-high       (NEW)
JKn.K    input 1   sync K arm, active-high       (NEW)
JKn.CLK  input 2   clock (validated, not gating) (NEW)
JKn.Q    output    stored value (NEW)
JKn.NQ   output    NOT Q, live (NEW)

Tn.T     input 0   sync toggle, active-high      (NEW)
Tn.CLK   input 1   clock (validated, not gating) (NEW)
Tn.Q     output    stored value (NEW)
Tn.NQ     output   NOT Q, live (NEW)
```

Numeric aliases `JKn.1|2|3` ≡ `J|K|CLK`, `Tn.1|2` ≡ `T|CLK`.
Case-insensitive. `J`/`K`/`T` on the wrong kind (e.g. `DFF1.J`, `JK1.D`)
are usage errors naming the bad pin, like out-of-range numerics today.

## C2. `poke` (extends existing verb)

```text
poke JK1.J=1          -> "poked JK1.J=1 ..."  (Q unchanged until step)
poke JK1.K=1          -> "poked JK1.K=1 ..."  (Q unchanged until step)
poke T1.T=1           -> "poked T1.T=1 ..."    (Q unchanged until step)
poke JK1.J=release    -> "released JK1.J ..." (pin back to wired/dangling)
poke T1.T=2           -> usage error (exit 2), no state change
poke DFF1.J=1         -> usage error (exit 2), no state change
```

No immediate Q movement on the new kinds (synchronous-only): the poke reply
confirms the drive; Q moves only at `step`. Invalid values and unknown pins
follow existing `poke:` error text conventions.

## C3. `step` (extends existing verb)

- Each member JK flop resolves J/K per the excitation table from pre-edge
  Q; each member T flop holds/toggles per T; DFF/DLATCH capture as today —
  all in the same edge, same slot order, same reported `cycle N`.
- New-kind flops report per-flop transition lines in the existing
  `JK1.Q: a->b` / `T1.Q: a->b` form (held flops use the existing
  `v (no change)` line shape, matching the DFF EN=0 convention).
- Cycle counters advance only for executed edges, as today.

## C4. `wire` / `tie` / `ignore` / `use` (extends existing verbs)

```text
wire BUF1.Y JK1.J    # driven J arm (resolved at next edge)
wire T1.Q T2.T       # ripple divider chain (Q feeds next toggle)
wire JK1.Q DFF1.D    # JK counter feeding a DFF register
ignore JK1.K         # dangling J/K excluded from block interface (reads 0)
use JK1.K            # re-expose (same as D/CLK ignore/use today)
```

`tie` between J/K/T pins shares one leader value (existing tie rules).
Wired JK/T inputs resolve through the same OLD-Q phase-1 comb settle as
every other flop input.

## C5. `peek` / `list` / save-load (unchanged form, extended content)

- `peek` shows `JK1.Q=v NQ=w (serial s)` / `T1.Q=v NQ=w (serial s)` — same
  shape as DFF lines, values per the excitation tables.
- Saved YAML: JK rows carry `kind: jk, inputs: 3`; T rows carry
  `kind: t, inputs: 2`; `ignore:` lists may reference indices over the
  kind's range; wires name `JKn.J|K|CLK`, `Tn.T|CLK` endpoints. Legacy
  files (no JK/T rows) load unchanged.
- Forward versions / unknown kind or pin names: loud usage error with line
  number, no state change.

## C6. Help + completion (FR-011)

Session help lists the two new kinds with pin tables plus the normative
excitation sentences (C0):

```text
JK: J (sync set/toggle arm) + K (sync reset/toggle arm) + CLK; T: T (sync toggle) + CLK.
JK: J=0,K=0 hold; J=0,K=1 reset (Q=0); J=1,K=0 set (Q=1); J=1,K=1 toggle.
T: T=0 hold; T=1 toggle (Q=NOT Q). Evaluated at the clock edge from pre-edge Q.
Unconnected J/K/T read 0 (hold).
```

Tab-completion suggests `JK`, `T` as `add` kinds; suggests `J`, `K`, `CLK`
after `JKn.` and `T`, `CLK` after `Tn.`, matching existing pin completion
behavior.
