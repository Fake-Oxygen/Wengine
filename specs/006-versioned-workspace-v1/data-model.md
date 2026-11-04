# Data Model: Versioned Workspace v1

**Feature**: specs/006-versioned-workspace-v1/spec.md | **Date**: 2026-11-04
(Derived from spec entities; research decisions R1–R10.)

## 1. Workspace File v1 (persisted artifact)

Top-level mapping with exactly these sections (order fixed for canonical output; unknown keys/sections are loud errors):

| Section | Shape | Required | Notes |
|---|---|---|---|
| `version:` | scalar `1` | yes | New generation. `1` without the sections below = legacy refusal (R1). Any other value = foreign-version refusal naming the found version. |
| `gates:` | sequence of gate rows | yes (may be empty) | Slot order == file order. |
| `wires:` | sequence of `{from, to}` | yes (may be empty) | Single-driver, known pins, flop-aware cycle rules — unchanged. |
| `ties:` | sequence of `{a, b}` | yes (may be empty) | One entry per non-leader tied member, `b` slot-ordered — unchanged. |
| `clocks:` | sequence of `{name}` | yes (may be empty) | One entry per derived clock domain, first-seen slot order (R3). |
| `meta:` | map with `tool:` | yes | Unchanged shape. |

Gate-row keys: `id`, `kind`, `inputs`, `ignore`, `ignore_output`, `delay_ps` (unchanged, all required), plus optional `ignore_nq` (sequential only, default true — unchanged) and optional **`init`** (sequential only, `0|1`, default `0`; R2/R9). `init` on a combinational row, or a non-binary `init` value, is a loud load error naming the gate (FR-006).

`clocks:` entry: exactly one key, `name`. Valid names are the derived domain spellings: a source-pin text (`BUF1.Y`, `DFF2.NQ`, …) or the bare `clk` for the implicit dangling-clock domain. Validation (loader, R3/R6): derive domains from the stored topology with the single engine derivation; require exact set-equality of names. Offenses (all loud, workspace untouched): unknown/duplicate name, `clk` entry with no dangling-clock flop, named entry whose source resolves to no live domain, missing entry for a derived domain, extra entry for no derived domain.

Empty workspace: all sections present and empty; round-trips byte-identically.

## 2. FlopInit (per-slot power-on value)

- **Carrier**: file `init:` key (R2) → load-bundle init array (R7) → pool `q[slot]` at attach (R5/R8).
- **Domain**: binary only (`LogicValue::kLow`/`kHigh`); covers every sequential kind (DFF/DLATCH/JK/T/SRLATCH). No Z/X (existing value-system rule).
- **Defaults**: absent key reads Low; saver always writes explicit `0|1` (R9).
- **Save direction**: saver reads *live* pool Q of valid sequential slots (R4) — save captures state, not design intent.

## 3. PinnedClockDomain (persisted name + source binding)

- **Fields**: `name` (derived spelling; IS the source binding — pin text or bare `clk`).
- **Membership**: never stored; always derived from topology (single source of truth). Edge kinds DFF/JK/T only; latches belong to no domain; dangling clocks form `clk`; first-seen slot order; members slot-ordered (engine derivation, R6).
- **Lifecycle**: computed at save (written to `clocks:`), re-derived and set-compared at load. No in-session persistence — session verbs re-derive live every step.

## 4. Load Bundle (`logic::Workspace`, R7)

Plain-data struct beside the loader (NOT in `Netlist`):

- `netlist`: gate topology in slot order (same build rules as today).
- `init[slot]`: per-slot power-on value, Low-defaulted; meaningful only where the slot holds a live sequential gate.
- Consumers: session `load` (→ pool, R5), `sim --seq-file` lowering (→ `SequentialRunConfig`, R8).

## 5. Pool Lifetime (session + engine)

States: `fresh (all invalid)` → `resynced (valid slots attached, kLow)` → `init-applied (Q = bundle inits)` → `stepping (Q evolves)` → …

- Session verbs keep idempotent resync (still-valid slots keep Q/serials across steps — unchanged).
- Session `load` starts a **new lifetime**: reset → resync → apply inits → existing async-control/level-settle application. Pre-load pool snapshotted; any failure restores pool + workspace (atomicity, FR-011).
- Engine driver (`configured_run` logic branch): state pool is per-run fresh; after `init_seq_state`, apply section inits to sequential slots before the first step (R8).

Serials: monotonic per lifetime from 0, never reused within it; new lifetime on load/run restarts them deterministically.

## 6. `SequentialRunConfig` delta (R8)

One added parsed-values field carrying per-slot power-on values (same shape as the bundle array), defaulting all-Low. Honored only when the `logic` solver is selected (existing section/solver gate); driver applies post-sync, pre-first-step. No flat fields, no other section changes.

## 7. Error taxonomy (all loud, all atomic)

| Class | Example diagnostic shape | Workspace/pool after |
|---|---|---|
| Foreign version | `unsupported workspace version N (want 1)` | untouched |
| Legacy v1-shaped (`version: 1`, missing new sections) | names found version + missing-section cause, states retirement | untouched |
| Legacy v2 (`version: 2`, missing `clocks:`) | names found version 2, states retirement | untouched |
| Bad init | `init` on combinational gate / non-binary value, names gate | untouched |
| Bad clocks | unknown/duplicate/unresolvable/empty/missing/extra domain, names domain | untouched |
| Existing classes | bad YAML, unknown keys, single-driver, cycles, illegal SR post-swap | untouched (existing guarantees kept) |

## 8. What does NOT change

`Gate`/`Netlist` layout and kind bytes; evaluation, excitation tables, control priority, level rules; `step`/`poke`/`peek` spellings and selection vocabulary; snapshot/Frame/CSV shapes; world checkpoint format; command/registry sizes (R12).
