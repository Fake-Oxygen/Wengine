# Data Model: Tri-state Net Resolution (kHighZ/kUnknown Semantics)

**Feature**: [spec.md](spec.md) | **Date**: 2026-11-19

This feature adds no stored state: it defines the semantics of a
pure function and its value vocabulary. The model below records the
entities, their meaning, the validation rules derived from the
specification's functional requirements, and the (stateless)
transitions a net undergoes as its drivers change.

## Entities

### LogicValue (net value vocabulary)

The four-valued net vocabulary, append-only and never renumbered
(pinned at compile time: `kHighZ == 2`, `kUnknown == 3`).

| Member | Byte | Meaning | Origin semantics |
|---|---|---|---|
| `kLow` | 0 | logic 0 | an active driver presenting low |
| `kHigh` | 1 | logic 1 | an active driver presenting high |
| `kHighZ` (Z) | 2 | undriven / tri-state off | no active driver on the net |
| `kUnknown` (X) | 3 | conflict marker | active low+high contention, or an unknown driver |

**Relationships**: values are produced by the resolution funnel and
by gate evaluation (gates emit only `kLow`/`kHigh`); values are
consumed by gate inputs, sequential control nets, and readback
(consumers accept only `kLow`/`kHigh` and reject Z/X loudly —
unchanged by this feature).

### Driver contribution

What one driver presents to a net — one `LogicValue` per driver:

| Contribution | Reading |
|---|---|
| `kLow`/`kHigh` | actively driving a level |
| `kHighZ` | present but off (enable-gated / tri-state disabled) |
| `kUnknown` | driving an unresolved value (upstream conflict) |

### Resolved net value

The single `LogicValue` a net carries after combining all driver
contributions. Result semantics enforced by the funnel:

- `kLow`/`kHigh` — the (agreed) active level
- `kHighZ` — no active drivers (empty span or all-Z)
- `kUnknown` — low+high contention among actives, or any unknown
  driver

Undriven (`kHighZ`) and contended (`kUnknown`) are distinct outcomes
and never conflated (FR-007).

### resolve_net (the single funnel)

The one seam every input resolves through. Contract:
total, commutative, order-independent, deterministic, single-pass
O(n) over the driver span, allocation-free, exception-free,
compile-time evaluatable. See [contracts/resolve-net.md](contracts/resolve-net.md)
for the full API contract, including the pairwise resolution matrix.

## Validation rules (from functional requirements)

| Rule | Source |
|---|---|
| Accepts any driver count, zero or more | FR-001 |
| High-Z contributions are transparent: adding/removing Z drivers never changes the result | FR-002 |
| Exactly one active driver → that driver's level | FR-003 |
| Multiple agreeing actives → the agreed level | FR-004 |
| Actives presenting both levels → `kUnknown`, never a silently picked contender | FR-005 |
| Any `kUnknown` contribution → `kUnknown`, regardless of other drivers | FR-006 |
| No actives (empty or all-Z) → `kHighZ` | FR-007 |
| Total: every well-formed span yields exactly one of the four values; value combination is not an error at the seam | FR-008 |
| Order-independent and deterministic: permutations of the same multiset resolve identically | FR-009 |
| V1 passthrough preserved: one low/high driver → that value; consumers keep rejecting X/Z loudly with state/timing untouched | FR-010 |
| Verified against an independently derived reference (IEEE 1164-anchored table), never against the implementation's own output | FR-011 |

## State transitions

None — `resolve_net` is a pure function with no stored state. The
observable "transition" is a net's value as its driver set changes
over time (the bus-toggle scenario), each evaluation independent and
stateless:

```text
drivers {A=level, B=Z}          → net = A's level      (one active)
drivers {A=level, B=opposite}  → net = kUnknown       (contention)
drivers {A=Z, B=Z}             → net = kHighZ         (undriven)
drivers {A=level, B=same}      → net = the level      (consensus)
drivers {}                     → net = kHighZ         (no drivers)
drivers {A=unknown, ...any...} → net = kUnknown        (X dominance)
```

Nothing about this feature adds, removes, or reorders persisted
vocabulary; the YAML workspace, world checkpoints, snapshot rows,
and Frame channels are untouched (Q values stay 0/1 because
consumers keep rejecting X/Z).
