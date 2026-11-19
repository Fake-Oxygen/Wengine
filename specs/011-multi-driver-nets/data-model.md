# Data Model: Allowlisted Multi-Driver Nets (011)

**Date**: 2026-11-19 | **Spec**: [spec.md](spec.md) | **Research**: [research.md](research.md)

All sizes/locations verified against the current tree; new fields are
additive to existing structs (no existing field moves, renames, or
reinterprets).

## Entities

### Shared-net mark (per destination input)

The per-input opt-in that lifts the single-driver rule for exactly one
input. Persistent topology state; serialized; inspectable via `list`.

- **Key**: `(slot, input)` — the existing `tie_flat(slot, input)` flat
  index, `kMaxGates * kMaxInputs` = 8192 positions.
- **Fields**: mark bit; extra-attachment count (0..3).
- **Storage**: `Netlist::shared_meta[flat]`, one byte:
  bit0 = mark, bits1..2 = extra count. Byte value 0 = unmarked
  (the fast test on both eval paths).
- **Validation**: marks apply to wired-driver inputs only — a tied
  input can never be marked (`wire` refuses tied destinations today,
  netlist.hpp:242-244; `tie_pins` refuses wired pins, tie_set.hpp:251-253;
  the states are disjoint per flat).
- **State transitions**:
  - unmarked → marked: explicit `wire --shared` (marks, then attaches)
    or pre-marking an unoccupied input with the same flag; idempotent
    on already-marked inputs.
  - marked → unmarked: `unshare`; refused loudly while extra count > 0
    (unwire down to ≤1 attachment first).
  - marks die with their gate (`remove_gate` resets the slot; the flat
    position dies with it).

### Driver attachment (per marked input)

One edge onto a shared net: a source slot plus a polarity. The unit of
the resolution span.

- **Primary attachment**: lives in `Gate::drivers[input]` with polarity
  in `driver_inv[flat]` — exactly today's representation, unchanged.
  Its presence keeps the wired-vs-dangling bit (`kNoDriver`) semantics
  for block-input detection, poke keys, and interface counting.
- **Extra attachments** (0..3): live in
  `Netlist::extra_drivers[flat * kMaxSharedExtra + j]` with per-extra
  polarity `Netlist::extra_inv[flat * kMaxSharedExtra + j]`.
- **Invariants**:
  - Primary exists iff the input has ≥1 attachment (extras never dangle
    behind a `kNoDriver` primary — promotion on primary-unwire
    preserves this).
  - Extras are dense: indices `0..count-1`, no holes; compaction is
    left-shift, promotion moves extra 0 to primary.
  - Every attachment (primary or extra) is fanout-accounted per its
    polarity (`output_fanout` / `nq_fanout` of the source).
- **Validation per attachment** (same as any wire, FR-012): live
  slots, input range, untied destination, flop-aware cycle check
  (`topo::reaches`, `kCombOnly`), cap (loud at 4 attachments).

### Resolved shared-net value

The single four-value result of folding all attachments through the
unchanged `resolve_net` funnel: agreement → level; low+high →
`kUnknown`; no attachments → `kHighZ`. Per-attachment polarity applies
before the fold (`apply_polarity` per attachment value).

### Contention count (per destination input, diagnostic)

- **Key**: the same flat index; **storage**: session-side `SolveDiag`
  POD (`std::array<std::uint32_t, 8192>`), owned by the CLI logic
  session beside `poke_map`/`cycle_counts` (logic.cpp singletons).
- **Semantics**: incremented once per contended resolution (span
  presents both polarized low and high) when a diagnostics channel is
  attached to the evaluation call.
- **Lifecycle**: cleared by `reset` with the other session state;
  **never serialized** into workspaces or checkpoints; observation-only
  (never alters values, state, timing, or outputs).
- **Visibility**: `solve`/`step` report per-net counts; `list` shows
  marks (topology readback).

## Structural changes to `Netlist` (include/wengine/logic/types.hpp)

| Member | Type | Size | Notes |
|---|---|---|---|
| `shared_meta` | `std::array<std::uint8_t, kMaxGates * kMaxInputs>` | 8 KB | mark + count byte per flat |
| `extra_drivers` | `std::array<std::uint16_t, kMaxGates * kMaxInputs * kMaxSharedExtra>` | 48 KB | extra source slots |
| `extra_inv` | `std::array<std::uint8_t, kMaxGates * kMaxInputs * kMaxSharedExtra>` | 24 KB | per-extra NQ polarity |

`sizeof(Netlist)`: 69,728 → ~149,728 B. `Gate` unchanged (40 B). All
new arrays are zero-initialized by `Netlist{}` (fresh-state default =
unmarked everywhere). Value equality stays defaulted-structural — new
arrays participate automatically.

New constant: `kMaxSharedExtra = 3` (4 attachments per input, loud
overflow).

## Serialization model (versioned YAML workspace, additive — research D5)

```yaml
version: 1            # marker unchanged; no generation retired
gates:                # unchanged rows
wires:                # unchanged row shape {from: SRC.pin, to: DST.pin};
                      # repeated to: DST.pin rows legal iff DST.pin is marked
shared:               # NEW optional section, emitted iff >=1 mark,
                      #       one row per marked input, slot order:
- net: AND2.2         # mark with its attachment count is derivable;
                      # zero-attachment marks are expressible
ties:                 # unchanged
clocks:               # unchanged
meta:                 # unchanged
```

Loader rules: `shared` joins the accepted root-key set (unknown keys
stay loud); absence = no marks (pre-011 files load byte-identically);
duplicate `to:` without a prior mark keeps today's "input already wired"
refusal byte-identically; duplicate `to:` rows on a marked input are
validated per attachment (live slots, range, untied, cycle, cap — the
engine rules), replayed in file order, and must not exceed the cap.

## Relationship to existing pools

- **Tie classes**: disjoint per flat (a wired/marked input is never
  tied; `tie_flat` indices shared, states exclusive) — no tie_store
  change.
- **SeqState**: untouched. Shared sequential inputs (D/CLK/RST/SET/EN/
  J/K/T/S/R) resolve through the same `resolve_input` fold; contended
  control/clock nets fail loudly via the existing X/Z consumer rules.
- **Clock domains**: named from the primary CLK attachment
  (documented edge, research D10).
- **Snapshot/Frame (world side)**: untouched — this feature is
  netlist-internal; `SequentialSolver` gains no fields (diagnostics are
  CLI-session state, research D4).
