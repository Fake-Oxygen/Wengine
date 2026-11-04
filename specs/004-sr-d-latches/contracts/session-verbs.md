# Contracts: SR/D Latch Pair

**Feature**: `specs/004-sr-d-latches/spec.md` | **Date**: 2026-11-01

Session-verb contracts (user-observable). Engine signatures are sketched
in `data-model.md`; exact C++ spelling lands in `tasks.md`/implementation.

## C1. Named pins (DLATCH unchanged + SR new)

DLATCH vocabulary is unchanged (`add DLATCH` ≡ 2 inputs):

```text
DLATCHn.D    input 0   data, active-high
DLATCHn.EN   input 1   level enable (NOT a clock), active-high
DLATCHn.Q    output    stored value
DLATCHn.NQ   output    NOT Q, live
```

New kind needs no opt-in (`add SRLATCH` ≡ 2 inputs):

```text
SRLATCHn.S    input 0   async set, active-high       (NEW)
SRLATCHn.R    input 1   async reset, active-high     (NEW)
SRLATCHn.Q    output    stored value (NEW)
SRLATCHn.NQ   output    NOT Q, live (NEW)
```

Numeric aliases `DLATCHn.1|2` ≡ `D|EN`, `SRLATCHn.1|2` ≡ `S|R`.
Case-insensitive. `S`/`R` on the wrong kind (e.g. `DFF1.S`, `SRLATCH1.D`,
`SRLATCH1.J`, `JK1.S`) are usage errors naming the bad pin, like
out-of-range numerics today. `D`/`EN` keep meaning `D`/`CLK` only on
their own kinds (no cross-kind leakage).

## C2. `poke` (extends existing verb — level kinds move Q at once)

```text
poke DLATCH1.EN=1       -> "poked DLATCH1.EN=1 ..."  + transparent Q report (Q follows D at once)
poke DLATCH1.D=1        -> "poked DLATCH1.D=1 ..."    + "DLATCH1.Q: a->b" when EN active, no report when frozen
poke SRLATCH1.S=1            -> "poked SRLATCH1.S=1 ..."        + "SRLATCH1.Q: a->b" (set at once, R inactive)
poke SRLATCH1.R=1            -> "poked SRLATCH1.R=1 ..."        + "SRLATCH1.Q: a->b" (reset at once, S inactive)
poke SRLATCH1.S=release      -> "released SRLATCH1.S ..."      (pin back to wired/dangling; Q holds)
poke SRLATCH1.S=1            -> usage error (exit 2) naming "S=1,R=1 illegal" when R already active — no state change, drive not committed
poke DLATCH1.D=2        -> usage error (exit 2), no state change
poke DFF1.S=1           -> usage error (exit 2), no state change
```

Unlike synchronous flops (Q moves only at `step`), latch pokes move Q
immediately through the level pass; the reply confirms the drive and any
`Q: a->b` transition uses the existing transition-line form. Invalid
values and unknown pins follow existing `poke:` error text conventions.

## C3. `step` (extends existing verb — latches exempt from edging)

- At every step top the level pass runs first: transparent D latches
  re-track `D`, frozen latches hold, SR latches hold/set/reset per
  settled nets — all before any edge executes.
- The edge itself steps only edge-kind members (DFF/JK/T per the
  domain); level kinds never snapshot from `d` and ignore domain
  membership (advancing `clk` or any named domain leaves level Q
  exactly as the pre-edge level pass left it).
- Level transitions at step top report in the existing `Q: a->b` form
  (held latches use the existing `v (no change)` line shape, matching
  the DFF EN=0 convention); edge transitions report as today with one
  shared cycle count.
- `step` with a pending illegal `S=R=1` fails before any edge or counter
  movement: usage error (exit 2) naming the combination, state and cycle
  counters untouched.
- Cycle counters advance only for executed edges, as today.

## C4. `wire` / `tie` / `ignore` / `use` (extends existing verbs)

```text
wire BUF1.Y DLATCH1.D   # transparent data feed (Q tracks at once)
wire BUF1.Y SRLATCH1.S       # async set feed (Q sets at once when R inactive)
wire SRLATCH1.Q DFF1.D       # handshake flag feeding an edge register
wire T1.Q SRLATCH1.R         # divider output as async reset
ignore SRLATCH1.R            # dangling S/R excluded from block interface (reads 0)
use SRLATCH1.R              # re-expose (same as D/EN ignore/use today)
```

Probe-then-revert (existing `wire.cpp` shape): a `wire`/`tie` completing
`S=R=1` through any net is rejected before the driver/class commits
(exit 2, names the combination, state untouched). `tie` between `S`/`R`
/`D`/`EN` pins shares one leader value (existing tie rules). Wired latch
inputs resolve through the same OLD-Q phase-1 comb settle as every other
sequential input.

## C5. `peek` / `list` / save-load (unchanged form, extended content)

- `peek` shows `DLATCH1.Q=v NQ=w (serial s)` /
  `SRLATCH1.Q=v NQ=w (serial s)` — same shape as flop lines, values per the
  level tables, immediately after level changes with no advance needed.
- `list` shows latch blocks, wires, and the derived interface with
  canonical `D`/`EN`/`S`/`R` spellings; `ignore`d pins collapse exactly
  as today.
- Saved YAML: SR rows carry `kind: srlatch, inputs: 2`;
  `ignore:` lists may reference indices over the kind's range; wires
  name `SRLATCHn.S|R` endpoints. DLATCH rows unchanged. Legacy files (no SR
  rows) load unchanged (structurally; DLATCH rows follow the new level
  rule per the documented refinement).
- Forward versions / unknown kind or pin names: loud usage error with
  line number, no state change. A file whose wiring resolves `S=R=1`
  fails the load atomically (workspace kept).

## C6. Help + completion (FR-010/FR-011)

Session help lists the latch pair with the pin table plus the normative
level sentences (C0):

```text
DLATCH: D (data) + EN (level enable, not a clock). SRLATCH: S (async set) + R (async reset).
DLATCH: EN=1 transparent (Q follows D); EN=0 frozen (Q holds, D ignored). No clock advance required.
SRLATCH: S=1,R=0 set (Q=1); S=0,R=1 reset (Q=0); S=0,R=0 hold. S=1,R=1 is illegal and refused with state unchanged.
Level latches belong to no clock domain: advances never gate their Q. Unconnected D/EN/S/R read 0 (hold).
```

Tab-completion suggests `SRLATCH` as an `add` kind (alongside the
existing kinds); suggests `S`, `R` after `SRLATCHn.`, matching existing pin
completion behavior.
