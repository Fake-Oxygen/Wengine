# Contracts: DFF Control Set

**Feature**: `specs/002-dff-control-set/spec.md` | **Date**: 2026-10-31

Session-verb contracts (user-observable). Engine signatures are sketched
in `data-model.md`; exact C++ spelling lands in `tasks.md`/implementation.

## C1. Named pins (extends FR-010/FR-011 vocabulary)

```text
DFFn.D    input 0   data (unchanged)
DFFn.CLK  input 1   clock (unchanged)
DFFn.RST  input 2   async reset, active-high      (NEW)
DFFn.SET  input 3   async set, active-high        (NEW)
DFFn.EN   input 4   clock enable, active-high     (NEW)
DFFn.Q    output    stored value (unchanged)
DFFn.NQ   output    NOT Q, live (unchanged)
```

Numeric aliases `DFFn.3|4|5` ≡ `RST|SET|EN`. Case-insensitive.
`DLATCHn.EN` (input 1, level-gate) is unchanged and distinct from
`DFFn.EN` (input 4, clock enable) — kind-qualified, never ambiguous.

## C2. `poke` (extends existing verb)

```text
poke DFF1.RST=1        -> "poked DFF1.RST=1 ..." + Q reads 0 at once
poke DFF1.SET=1        -> "poked DFF1.SET=1 ..." + Q reads 1 at once
poke DFF1.EN=0         -> "poked DFF1.EN=0 ..."  (Q unchanged until step)
poke DFF1.RST=release  -> "released DFF1.RST ..." (Q holds until next force/edge)
poke DFF1.RST=2        -> usage error (exit 2), no state change
```

Async pokes report the forced Q in the same reply line form as existing
poke confirmations. Invalid values and unknown pins follow existing
`poke:` error text conventions.

## C3. `step` (extends existing verb)

- Async controls are applied before edge looping; forces appear in the
  per-flop transition report using the existing `DFF1.Q: a->b` form
  (a forced `0->1` is indistinguishable in form from a captured one —
  the cause is the asserted control, visible via `poke` map/`peek`).
- `EN=0` flops report `DFF1.Q: v (no change)` / held lines while
  siblings report captures — same line shapes as the existing
  implicit-domain untouched report.
- Cycle counters advance only for executed edges; async-only activity
  (no `step`) never changes `cycle N`.

## C4. `wire` / `tie` / `ignore` / `use` (extends existing verbs)

```text
wire BUF1.Y DFF1.RST   # driven reset (immediate force on next resolve)
wire DFF1.Q DFF2.EN    # conditional stall
ignore DFF1.RST        # dangling control excluded from block interface
use DFF1.RST           # re-expose (same as D/CLK ignore/use today)
```

Wiring an active driver to `RST`/`SET` forces Q at the next async-apply
(step top, or immediately where the session applies it post-wire).
`tie` between control pins shares one leader value (existing tie rules).

## C5. `peek` / `list` / save-load (unchanged form, extended content)

- `peek` shows `DFF1.Q=v NQ=w (serial s)` — same shape; values reflect
  async forces and holds.
- Saved YAML: DFF rows carry `inputs: 5`; `ignore:` lists may reference
  indices 1..5; wires name `DFFn.RST|SET|EN` endpoints. Legacy
  `inputs: 2` files load with controls dangling-inactive.
- Forward versions / unknown pin names: loud usage error, no state change.

## C6. Help + completion (FR-011)

Session help lists the three new pins with active levels plus the one
priority sentence (C0):

```text
DFF controls: RST (async reset) > SET (async set) > EN-hold (sync) > D; RST+SET -> Q=0.
Unconnected RST/SET read 0 (inactive), EN reads 1 (enabled).
```

Tab-completion suggests `RST`, `SET`, `EN` after `DFFn.` and the
`--from/--to` value forms where applicable, matching existing pin
completion behavior.
