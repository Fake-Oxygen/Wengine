# Data Model: Propagation-Delay Annotation Verbs (013)

**Date**: 2026-11-20 | **Spec**: [spec.md](spec.md) | **Research**: [research.md](research.md)

All sizes/locations verified against the current tree; the feature adds
**no struct, no array, no schema, no checkpoint change** — the storage
slot shipped with the V2 timing hook and this feature only opens it to
users. New data is one value per existing gate.

## Entities

### Propagation-delay annotation (per gate)

The stored duration naming how long the gate's output takes to respond
— stored now, honored later.

- **Key**: gate slot; addressed through the module's display identity
  (`AND1`, `DFF2`, `TBUF3` — per-kind monotonic never-reused counters,
  `resolve_gate` at pins.hpp:364). Dead slots are not addressable.
- **Storage**: `Gate::propagation_delay_ps` (types.hpp:333),
  `std::uint64_t`, whole picoseconds, default 0 meaning un-annotated.
  The zero value and the cleared state are the same state — there is no
  "annotated zero" anywhere in the module.
- **Domain**: any live gate regardless of kind (combinational and
  sequential alike); what a delay means per kind is the future delay
  evaluator's decision (spec FR-008) — nothing stored constrains it.
- **Validation**: value grammar per the exactness contract
  ([contracts/engine-seam.md](contracts/engine-seam.md)) —
  non-negative, whole picoseconds, within the field's u64 range;
  refusals are loud and touch no state.
- **State transitions**:
  - 0 → N: explicit `set-delay` verb; re-set replaces (last write
    wins, deterministic, immediate).
  - N → M: re-set (same path, same rules).
  - N → 0: explicit `unset-delay` verb (refused loudly when N = 0,
    mirroring `unpull`), or `set-delay X 0` (equivalent by the
    zero-is-default rule).
  - Any → 0 implicitly: `add_gate` on a fresh/recycled slot zeroes
    the field (netlist.hpp:103); gate removal / `reset` zero it
    (netlist.hpp:764) — an annotation never survives its gate, and a
    recycled slot starts un-annotated.
  - O0/O1 optimize: preserved in place (documented, optimize.hpp:63).
  - O2 optimize: reset on reshape (documented,
    optimize_o2_emit.hpp:195), reported when ≥1 annotation existed
    (research D6).

### Value grammar (the accepted token forms)

Not stored — a property of the parse at the verb boundary:

- **Bare**: `^[0-9]+$` → exact integer picoseconds, full u64 range.
- **Suffixed**: plain decimal `^[0-9]+(\.[0-9]+)?$` + one owned time
  spelling (`s sec ms us ns min h ps`, case-sensitive), converted by
  integer arithmetic (factor × 10^(12+exp10), implied fraction divisor)
  — accepted iff the result is a whole number of picoseconds within
  the u64 range.
- **Refused loudly**: negative signs, exponents, malformed decimals,
  sub-picosecond remainders, values beyond the u64 field, unknown or
  cross-dimension suffixes, empty/missing operands.
- No floating point participates; the parse is a pure function of the
  token (deterministic across runs and platforms by construction).

### Gate identity (existing, unchanged)

Per-kind monotonic never-reused counters rendered as `KIND<n>`;
`resolve_gate` (pins.hpp:364) returns the live slot or a loud failure.
Uniqueness is by construction, so annotation addressing never collides.

## Structural changes to `Netlist` / `Gate`

None. The member exists (types.hpp:333); value equality already
compares it (defaulted `operator==`, pinned by test_logic.cpp:1034 and
test_logic_yaml.cpp:72). `sizeof(Netlist)` is unchanged.

New engine functions (netlist.hpp, beside `set_pull`:361 /
`clear_pull`:393): `set_gate_delay(nl, slot, value)` and
`clear_gate_delay(nl, slot)` — live-slot validation, `[[nodiscard]]`
`core::Status`, Doxygen; see [contracts/engine-seam.md](contracts/engine-seam.md).

## Serialization model (already shipped — pinned, not changed)

```yaml
version: 1            # marker unchanged; no section added, none retired
gates:
  - {id: AND1, kind: and, inputs: 8, ignore: [2, 8], ignore_output: false, delay_ps: 1500}
                       # delay_ps: required key on EVERY row (this feature writes
                       # what the loader already validates), value = whole ps
```

- Save writes `delay_ps:` on every gate row (yaml_store_save.cpp:142);
  the checker requires the key and validates it as u64
  (yaml_store_check.cpp:22/:40/:184); replay stores it
  (yaml_store_replay.cpp:48). Annotated workspaces therefore save
  with **identical row shape** to un-annotated ones — only the value
  differs, and pre-feature files (all zeros) are already in canonical
  form.
- World binary checkpoint: the field is a u64 column, round-tripped
  (serialize.hpp:352 put / :791 load) — unchanged.
- Loading a future delay-aware workspace in this build round-trips the
  values identically (honoring them is the evaluator's job, not the
  loader's).

## Relationship to existing pools

- **Ties / pulls / shared marks**: orthogonal — the annotation is a
  per-gate attribute, disjoint from every per-input topology
  attribute; no interplay, no combined validation.
- **SeqState / clock domains**: untouched — the annotation rides on
  `Gate`, not on sequential state; evaluation and stepping never read
  it (inertness pinned by test).
- **Snapshot / Frame (world side)**: untouched — this feature is
  netlist-internal; `SequentialSolver` gains no fields.
- **Optimizer pools**: untouched — O0/O1 preserve in place; O2's
  documented reset gains only a conditional report line (research D6).
