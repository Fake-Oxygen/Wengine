# Data Model: Enable-Gated Buffer plus Per-Net Pulls (012)

**Date**: 2026-11-20 | **Spec**: [spec.md](spec.md) | **Research**: [research.md](research.md)

All sizes/locations verified against the current tree; new fields are
additive to existing structs (no existing field moves, renames, or
reinterprets).

## Entities

### Gated buffer (new gate kind)

A two-input combinational gate whose output contribution is its data
level while enabled and high-Z while released.

- **Key**: gate slot; display identity `TBUF<n>` from the per-kind
  monotonic counter `next_counter[8]` (never reused; slots recycle).
- **Kind byte**: 8 (`GateKind::kTbuf`), append-only; persisted in YAML
  `tbuf` rows; `valid_fanin` allows exactly 2, default 2.
- **Inputs**: `drivers[0]` = data, `drivers[1]` = enable
  (active-high). Both are ordinary wired inputs: they resolve through
  `resolve_input`, require definite levels, and reject X/Z/dangling
  loudly at their seams.
- **Output**: `gate_out[slot]` holds Low/High while enabled,
  `kHighZ` while released — the first legal Z producer in `gate_out`.
- **Fan-out**: ordinary `output_fanout` accounting (buffers are never
  NQ sources — `wire_pins` refuses NQ on combinational gates, so
  buffer attachments always carry polarity 0).
- **State transitions**: none of its own (combinational, stateless);
  lives/dies with `add_gate`/`remove_gate` like any gate.

### Pull setting (per destination input)

The per-input attribute naming what an all-floating span means on
exactly that net.

- **Key**: `(slot, input)` — the existing `tie_flat(slot, input)`
  flat index, `kMaxGates * kMaxInputs` = 8192 positions (same
  granularity as shared marks).
- **Values**: 0 = none (default, today's behavior), 1 = up, 2 = down.
- **Storage**: `Netlist::pull_cfg[flat]`, one byte; zero-initialized
  by `Netlist{}` (fresh-state default = unpulled everywhere).
- **Validation**: refused on tied inputs ("untie first") and on
  ignored inputs ("use first"); tying or ignoring a pulled input is
  refused symmetrically. Settable on wired inputs (any attachment
  count, marked or not) and on dangling inputs (termination).
- **State transitions**:
  - none → up/down: explicit `pull` verb; idempotent re-set to the
    same level.
  - up/down → up/down: re-set (loud confirmation, no transition
    subtlety — the level is simply replaced).
  - up/down → none: explicit `unpull` verb; refused when no pull set.
  - pulls die with their gate (`remove_gate` clears the slot's flats;
    `reset_netlist` zeroes the array).

### Floating vs. contended net (resolution outcome classes)

Not stored — a property of each evaluation, decided by the shipped
funnel before the pull map:

- **Floating**: empty span or every attachment high-Z (all buffers
  disabled, or no drivers). Pull maps: up → `kHigh`, down → `kLow`,
  none → `kHighZ`.
- **Driven**: any active level, agreement or not. Pull is inert:
  agreement → level; low+high → `kUnknown` (counted); unknown present
  → `kUnknown`.

### Terminated dangling input

A dangling input with a pull set. Not a block input (excluded from
`block_input_count`, `block_interface`, `input_index`,
`build_input_positions`, and `poke` validation — "not a block input"
refusals cover it); resolved to its idle level in `fill_resolved`
without consuming a block-input column. Never tied (disjoint by
refusal), never ignored (disjoint by refusal).

### Resolved buffer/pull net value

The single four-value result of the unchanged `resolve_net` fold plus
the pull's all-Z map (research D4): agreement → level; low+high →
`kUnknown` (+1 contention iff a diagnostics channel observes a
low+high span at assembly); no active drivers → idle level or `kHighZ`
per the pull. Per-attachment polarity applies before the fold
(`apply_polarity` per attachment value; buffer attachments are
always non-inverted by the NQ-source rule).

## Structural changes to `Netlist` (include/wengine/logic/types.hpp)

| Member | Type | Size | Notes |
|---|---|---|---|
| `pull_cfg` | `std::array<std::uint8_t, kMaxGates * kMaxInputs>` | 8 KB | pull level per flat (0/1/2) |
| `kTbuf` | `GateKind` enumerator | — | byte 8; `gate_kind_name` arm |

`sizeof(Netlist)`: ~149,728 → ~157,928 B. `Gate` unchanged (no new
per-gate bytes — buffers reuse `drivers[2]`). `next_counter` unchanged
(21 slots already cover ordinal 8). Value equality stays
defaulted-structural — the new array participates automatically
(test-equality oracles inherit it).

New predicates (beside the 011 set in types/block_interface/barrier
headers): `is_pulled_flat` / `pull_level_flat` (types.hpp, mirroring
`is_shared_flat` / `shared_extra_flat`); `has_pulled_input`
(optimize_barrier.hpp, mirroring `has_marked_input`);
`has_buffer_gates`, `any_pull` (block_interface.hpp, mirroring
`shared_word_blocked` / `any_shared_mark`).

## Serialization model (versioned YAML workspace, additive — research D6)

```yaml
version: 1            # marker unchanged; no generation retired
gates:                # unchanged rows + tbuf kind rows ({id: TBUF1, kind: tbuf, inputs: 2})
wires:                # unchanged row shape; buffer data/enable wire like any input
shared:               # unchanged (011)
pulls:                # NEW optional section, emitted iff >= 1 pull,
                      #       one row per pulled input, slot order:
- {net: AND2.2, pull: up}   # level spellings: up | down only
ties:                 # unchanged
clocks:               # unchanged
meta:                 # unchanged
```

Loader rules: `pulls` joins the accepted root-key set (unknown keys
stay loud); absence = no pulls (pre-012 files load byte-identically);
duplicate `net:` rows fail loudly; unknown levels fail loudly; `none`
is never written (absence expresses it). Pull rows validate against
live inputs (dead/out-of-range/tied/ignored destinations fail loudly,
workspace untouched on failure — the atomic-load convention).

## Relationship to existing pools

- **Tie classes**: disjoint per flat (a tied input is never pulled;
  `tie_flat` indices shared, states exclusive) — no tie_store change.
- **Shared marks**: compose freely (a marked input may carry a pull;
  the shared branch maps all-Z spans through it) — no shared_meta
  change.
- **SeqState**: untouched. Buffered control/clock nets resolve through
  the same `resolve_input` fold; contended or unresolvable nets fail
  loudly via the existing X/Z consumer rules. Buffer gates are
  combinational: no `SeqState` rows, no clock-domain membership of
  their own.
- **Clock domains**: named from the primary CLK attachment (011-D10
  convention, unchanged); a buffer driving a CLK input is an ordinary
  attachment — a contended clock still fails loudly at the step seam.
- **Snapshot/Frame (world side)**: untouched — this feature is
  netlist-internal; `SequentialSolver` gains no fields (diagnostics
  stay CLI-session state, research 011-D4).
