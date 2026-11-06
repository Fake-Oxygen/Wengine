# Contract: `logic optimize` over Sequential Nets

**Date**: 2026-11-04 | **Spec**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md)

The CLI surface does not change (no new commands, flags, or report fields).
This contract pins the observable behavior of the existing surface when the
workspace net contains flop/latch gates, so tests assert the right thing.

## Command schema (unchanged)

```text
logic optimize [--level O0|O1|O2] [--to nand|nor]
```

- `--to` still requires `--level O2` (usage error, exit 2, otherwise).
- Stray positionals are usage errors (exit 2). Bad level/target values are
  usage errors (exit 2).

## Success contract (extends to sequential nets)

**Given** a valid workspace net containing any mix of combinational gates
and flop/latch gates (`DFF`, `DLATCH`, `JK`, `T`, `SRLATCH`, including
full-control DFFs and NQ consumers):

| Level | Exit | Result |
|-------|------|--------|
| O0 | `0` | Optimized net; every sequential gate present with unchanged logical wiring (Story 1). |
| O1 | `0` | Optimized net; barriers hold, `NOT(Q)`-style consumers never fused through state (Story 2). |
| O2 (`--to nand\|nor`) | `0` | Universal-vocabulary net with sequential gates carried verbatim; combinational regions converted (Story 2/3). Previously this failed loudly — that failure is gone (FR-007). |

## Report contract (unchanged shape)

Standard output keeps the existing form:

```text
optimize: <before> -> <after> gates (level=<O0|O1|O2>[ to=<nand|nor>])
```

plus the existing `interface:` notes when pin sets change (O2 pin
duplication, dissolved ties on O2 sequential pins — see research R5).

- The report MUST NOT show a sequential-gate-count drop: any total-count
  reduction is fully accounted for by removed combinational gates
  (Story 4). Tests assert `after_seq_count == before_seq_count` via the
  gate-kind census, not by parsing prose.
- Exit `1` (runtime) remains reserved for genuine failures: gate-budget
  overflow (`kMaxGates`), topology errors. Mere presence of sequential
  gates MUST NOT produce exit 1.

## Equivalence contract (what "same behavior" means)

Row-for-row truth-table comparison is **not** the equivalence check for
sequential nets (state makes block tables history-dependent, and O2 may
change the pin interface — both pre-existing caveats the CLI already
reports). The equivalence check is:

1. **Structural**: sequential gate set identical (names, kinds, logical
   drivers, polarity) per `data-model.md` validation rules.
2. **Behavioral**: fixed-step run parity — N steps from the same start give
   identical stored values and settled outputs at every step (FR-011),
   including async-control priority and illegal-combination refusal.

## Completion contract (unchanged)

`logic optimize --level O<TAB>` completes `O0 O1 O2`; `--to` completes
`nand nor`. No new completions.
