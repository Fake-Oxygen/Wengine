# Data Model: Flop-Latch Opaque Barrier

**Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md) | **Research**: [research.md](./research.md)

This feature changes **no stored shapes**. All entities below already exist
in `include/wengine/logic/`; the model records the barrier-relevant fields,
the new invariants, and which passes may touch what. Field names mirror the
code so tests can assert directly.

## Entities

### Gate (existing — `logic::Gate`, `netlist.hpp`)

The unit of preservation. No field added, none removed.

| Field | Barrier role |
|-------|--------------|
| `live` | Only live gates participate; dead slots are ignored by every pass (unchanged). |
| `kind` (`kDff/kDlatch/kJk/kT/kSrlatch` = state elements) | Discriminator: `is_sequential_kind(kind)` selects the barrier set. Never rewritten by any pass (FR-001). |
| `counter` | Display identity (`DFF1`, `JK2`, …). In-place levels (O0/O1) never retag sequential gates, so counters are stable; O2 verbatim emit takes a fresh counter via `add_gate` (names still resolve 1:1). |
| `input_count` | Pin count. Frozen for sequential gates at every level (FR-002). |
| `drivers[]` (per-pin source slot or `kNoDriver`) | Logical wiring. O0/O1: bit-identical before/after. O2: re-emitted through the emit map onto the corresponding fresh sources (same logical source, same position). Never dropped, folded, or bypassed. |
| `ignored_mask` | Dangling-pin exclusion. Frozen for sequential gates (a dangling flop input is never tied off by exact/dead-input reasoning). |
| `ignore_output` / `ignore_nq_output` | Hidden-output flags. Preserved verbatim (including across the O2 rebuild) so hidden cones survive exactly as today. |
| `output_fanout` / `nq_fanout` | Derived counts, recomputed by `recount_fanout` (+ NQ recount for fresh O2 nets). Read by DCE root computation and interface derivation; never a deletion reason for state elements. |
| `propagation_delay_ps` | Stored-now/honored-later hook. O0/O1 preserve in place; O2 resets on reshape (existing behavior, unchanged). |

### Netlist (existing — `logic::Netlist`, `netlist.hpp`)

| Field | Barrier role |
|-------|--------------|
| `gates[1024]` (slot pool; slots recycle, counters don't) | Container. O0/O1 work in place (preserved sequential gates keep slots); O2 compacts into a fresh net (identity by display name, not slot). |
| `driver_inv[slot*8+pin]` (NQ polarity) | **Must be copied per pin** for every O2-emitted sequential consumer pin (optimizer never touched it before — there were no sequential consumers in O2 output). O0/O1: untouched (freeze). |
| `tie_rep[]` (input-input tie classes) | O0/O1: sequential pins never join/leave classes (freeze; exact-skip removes the only mutator). O2: classes on sequential pins dissolve into independent dangling pins (documented, interface report covers it — research R5). |
| `topo_order` / `dirty` | Recompiled after structural passes (existing discipline, unchanged). |

### Constness label (existing — `detail::Constness`, `optimize.hpp`)

Per-gate `kC0/kC1/kV` scratch, recomputed by `classify` each pass.

- **Invariant (new, explicit)**: a live sequential gate's label is always
  `kV`. `classify` already yields this (init-all-`kV` + SEQ11 skip);
  the collapse-roots loops gain a defensive sequential skip so no future
  label change can constant-fold a state element (FR-005).

### DCE root set (existing pattern, extended — `roots[]` / `post_roots[]`)

- **Invariant (new)**: every live sequential gate is a root, in both the
  O0/O1 fixpoint (`roots[]`) and the O2 post-cleanup (`post_roots[]` on
  fresh sequential gates), in addition to the existing fanout-0 sinks
  (ignored outputs included). Roots are computed once per `optimize()`
  call from the input net (O0/O1) / fresh net (O2 post) — deterministic,
  slot order (FR-004).

### O2 emit map (existing — `emit_map[]` + `PendingWire`, extended)

- One entry per original slot → fresh slot. Sequential gates gain a
  verbatim-emit branch (kind/count/flags copied, one pending wire per
  input, polarity copied per pin). Wiring pass resolves drivers through the
  map exactly as for combinational gates (FR-007, research R4).

## Validation rules (FR → checkable rule)

| FR | Rule |
|----|------|
| FR-001 | After any level: sequential name set before == after (resolve every `DFFn`/`DLATCHn`/`JKn`/`Tn`/`SRn` name before and after); kinds equal. |
| FR-002 | After O0/O1: each sequential gate's `input_count`, `ignored_mask`, and every `drivers[]` entry bit-identical. After O2: same `input_count`/`ignored_mask`, each driver maps via emit correspondence to the same logical source, polarity equal. |
| FR-003 | Every consumer pin reading a sequential Q/NQ before reads the same gate's same pin after (name + polarity compare). |
| FR-004 | Nets with unobserved flops and closed state clusters optimize without losing any sequential gate. |
| FR-005 | No sequential gate is ever constant-collapsed (labels stay `kV`; collapse loops skip sequential kinds). |
| FR-006 | Two identically wired flops both survive (dedup never merges sequential kinds). |
| FR-007 | `optimize()` returns success on sequential nets at O0/O1/O2 (no "budget exceeded" from `emit_gate` on sequential kinds). |
| FR-008 | A net with known redundant combinational glue still shrinks in total count while the sequential count is flat. |
| FR-009 | All pre-existing combinational optimizer tests pass unmodified. |
| FR-010 | optimize(optimize(x)) == optimize(x); two identical calls agree. |
| FR-011 | 10-step run parity original vs optimized (stored + settled, every step). |

## State transitions

None. `optimize()` is a pure function (`const Netlist&` in → new
`Netlist` out); barrier work adds no state machine, no lifecycle change,
no persistence change.
