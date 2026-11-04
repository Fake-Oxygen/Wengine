# Research: SR/D Latch Pair

**Feature**: `specs/004-sr-d-latches/spec.md` | **Date**: 2026-11-01

All unknowns resolved — no NEEDS CLARIFICATION remains. Every decision is
grounded in the current tree (003 established the kind-extension pattern;
002 established the async-apply path; `seq_step.hpp` SEQ4/SEQ8 notes scope
the DLATCH refinement).

## R1. How to carry SR: new kind vs. DLATCH/DFF mode

**Decision**: One new appended `GateKind` code — `kSrlatch = 20` — with
fixed fan-in 2 (`S`/`R`). `default_fanin` returns 2 (`add SRLATCH` needs
no `--inputs`); any other `--inputs` value is rejected by `valid_fanin`.
`next_counter` grows 20 → 21 entries. DLATCH (`kDlatch = 17`, `D`/`EN`,
fan-in 2) is unchanged as vocabulary — this feature refines its
*semantics* from the documented SEQ4 edge-capture simplification to true
transparent/frozen level behavior.

**Rationale**: The spec calls for a "level-sensitive latch pair" completing
the palette. Kind-indexed machinery already exists everywhere that matters:
per-kind counters/serials (`next_counter`, `SeqState` attach),
`parse_pin` named rows, `pin_text` canonical names, per-kind display names
(`SR1`), YAML kind spelling. A mode flag on DLATCH/DFF would need a
parallel branch at every one of those sites plus a second snapshot rule —
more concepts, not fewer. The 003 pattern (append-only codes, fixed fan-in,
table-entry diffs) applies directly.

**Alternatives considered**:
- Mode bit on DLATCH (e.g. `is_sr` on `Gate`) — rejected: splits pin
  dispatch, breaks per-kind counters/naming/YAML spelling.
- Emulate SR from cross-coupled NOR gates — rejected: this is exactly the
  emulation the spec says to stop requiring, and it cannot produce the
  specified loud illegal-input refusal.

**Append-only rule** (`types.hpp`, `docs/logic.md` §12): persisted bytes
never shift — DFF=16, DLATCH=17, JK=18, T=19 stay; SR=20 appended.
`test_logic_seq_kinds.cpp` ordinal `static_assert`s extended (the compiler
plus `-Wswitch` coverage enforce the remaining `switch (kind)` sites:
`gate_kind_name`, `eval_gate`, bit-parallel eval, `optimize.hpp`).

## R2. Pin vocabulary and indices

**Decision**: Fixed positions, same conventions as DFF/DLATCH/JK/T:

| Kind | 0 | 1 | Outputs |
|------|---|---|---------|
| DLATCH (existing) | `D` | `EN` | `Q`, `NQ` live negation |
| SRLATCH (new) | `S` | `R` | `Q`, `NQ` live negation |

Case-insensitive named matching; numeric aliases (`.1`/`.2`) keep
working; named pins on combinational kinds stay rejected; latch pins on
the wrong kind rejected exactly like out-of-range numerics today
(`S` on DFF, `J` on SR, `S` on DLATCH, `D` on SR).

**Rationale**: `parse_pin`, `pin_text`, wire/tie/ignore/`use`,
block-input composition, and NQ polarity are all pin-index-driven — fixed
indices make every verb (FR-005) work with table-entry-sized diffs,
mirroring how JK/T rode indices 0/1/2 in 003.

**Dangling default** (FR-006): unconnected `D`/`EN`/`S`/`R` read `kLow`
via the existing rules (ignored-dangling-ties-to-`kLow`, poke-map compose
default) — i.e. the hold row — so bare latches hold state and existing
netlists are untouched. No 5-input-style opt-in: both latch kinds are
exactly 2 inputs, always.

## R3. Level semantics (independent reference for tests)

**Decision**: Level resolution from settled nets, in slot order, from OLD
Q where holding. Hand-derived tables (tests assert against THESE, never
against implementation output):

DLATCH (`d` = resolved D, `en` = resolved EN, `q` = old Q):

| EN | D | Q_next |
|----|---|--------|
| 0 | 0 | q (frozen hold) |
| 0 | 1 | q (frozen hold — D ignored) |
| 1 | 0 | 0 (transparent follow) |
| 1 | 1 | 1 (transparent follow) |

i.e. `Q_next = EN ? D : q_old`.

SRLATCH (`s` = resolved S, `r` = resolved R, `q` = old Q):

| S | R | Q_next |
|---|---|--------|
| 0 | 0 | q (hold / memory) |
| 1 | 0 | 1 (set) |
| 0 | 1 | 0 (reset) |
| 1 | 1 | ILLEGAL — refuse loudly, no commit (FR-003) |

**Rationale**: The textbook gated-D and NOR-SR definitions, reduced to the
spec's pinout (ungated SR; hold is `S=R=0`). Transparent/frozen and
set/reset/hold are the two normative sentences users predict from
(FR-011). Hold reads OLD Q (parallel update — same commit discipline as
the edge snapshot, so mixed swaps behave).

**Rules carried over unchanged**: only `kLow`/`kHigh` accepted on
`D`/`EN`/`S`/`R` nets; `kHighZ`/`kUnknown` rejected loudly with no state
commit (existing X/Z rule); commit is slot-order parallel; power-on
`Q=kLow` (unchanged `init_seq_state` path); serials never reused.

## R4. Level-resolution integration point (DLATCH refinement + SR)

**Decision**: Level latches resolve *outside* clock advances, in a shared
level-settle pass invoked after every control-net mutation
(poke/wire/tie/load) and at step top before edging — the same call sites
that invoke `apply_async_controls` today (session `apply_async_and_report`
+ `step` pre-edge). Exact placement:

1. Phase-1 combinational settle with OLD Qs (existing
   `eval_comb_phase1` helper, reused verbatim).
2. Slot-order level pass: for each live DLATCH/SRLATCH, resolve its two
   inputs from the settled nets; compute `Q_next` per R3; on any
   `S=R=1` (resolved through wire/tie/poke alike) return failure with
   *nothing committed*.
3. Slot-order commit of level Qs; NQ follows live (no stored state).
4. Edge snapshot (`seq_step_filtered` loop) *skips* level kinds — they
   never snapshot from `d`; frozen latches hold by construction,
   transparent D latches already track `D` from step 3 (re-tracked at
   every step top while EN stays active).

`apply_async_controls` (DFF-only filter) stays untouched; the level pass
is a sibling function (`apply_level_latches` or equivalent) so the DFF
priority block and its tests never change shape. The SEQ4 edge-capture
fallthrough (`next_q[s] = d`) loses its DLATCH arm — DLATCH joins the
level-exempt set with SR.

**Rationale**: Mirrors the proven DFF async pattern (002): immediate
visibility with no advance (FR-004), one shared resolver for all entry
paths, atomic fail-without-commit. Reusing `eval_comb_phase1` keeps the
two-phase contract (comb with OLD Qs → slot-order resolve → slot-order
commit → re-settle) identical to the edge path, so US3 (mixed stepping)
falls out with no separate stepper.

**Alternatives considered**:
- Keep DLATCH on the edge and add SR as edge-triggered — rejected: the
  spec explicitly requires transparent-without-advance and frozen-across-
  advances; edge capture cannot show follow-with-no-advance.
- Resolve latches inside combinational `eval_gate` — rejected: `eval`
  is combinational-only by contract (`solve`/`eval_block_word` never see
  sequential gates; flop feedback is a loud cycle error by design).
  Level state lives in `SeqState`, not in comb eval.

## R5. Illegal-input refusal across all entry paths

**Decision**: One central check — the level pass returns failure on any
member resolving `S=kHigh, R=kHigh` — surfaced by every verb through the
existing failure channels:

- Direct drive (`poke SR1.S=1` completing `S=R=1`): poke composes, level
  pass fails, the verb reports a usage error (exit 2) naming the illegal
  combination and leaves state untouched (poke entry not committed —
  same atomicity as the existing X/Z-on-poke rule).
- Wired nets (`wire X.Y SR1.S` completing the combination through comb
  logic): `wire` probes via `validate_sequential` + level pass on a
  scratch evaluation before committing the driver (existing probe-then-
  revert shape in `wire.cpp`); failure reverts the wire and reports exit
  2 with no state change.
- `tie` classes: same probe-then-revert — a tie completing `S=R=1`
  through a shared leader value is rejected before the class commits.
- `load` (YAML file containing `S=R=1` wiring/drives): loader evaluates
  the level pass after build; failure aborts the load atomically (`out`
  untouched, workspace kept — existing `load_text` atomicity).
- `step`: pre-edge level pass fails first, so the edge never executes
  and cycle counters never advance.

**Rationale**: FR-003 requires refusal "regardless of whether the
combination arises from a direct drive, wired logic, tied pins, or a
loaded file". A single resolver plus per-verb atomicity (all of which
already exist as patterns) satisfies this with no new error vocabulary:
usage error, exit 2, names the combination, state untouched.

## R6. Clock domains, stepping, and what stays untouched

**Decision**:
- `clock_domains()` (session grouping by input-1 driver) covers edge
  kinds only (DFF/JK/T). Level latches are excluded: DLATCH input 1 is a
  *level enable*, not a clock, and SR input 1 is data (`R`) — grouping
  either by driver would manufacture bogus domains. Practically: the
  domain scan filters `kind == kDff || kind == kJk || kind == kT`.
- `seq_step_filtered` member filtering applies to edge kinds only;
  level kinds are unconditionally step-exempt (they hold/track per R4
  regardless of which domain advances — spec edge case "advances never
  gate level-resolved Q").
- `validate_sequential` flop-aware cycle exemption extends to SR by
  construction (`is_sequential_kind` += SR): edges into `S`/`R` never
  close a cycle (Q outputs break loops), so legal SR feedback
  (`Q -> comb -> S/R`) wires in the same order as DFF/JK/T feedback.
- Unchanged: `SeqState` shape (generic over `is_sequential_kind`),
  `eval_gate` + bit-parallel eval sequential fallthrough (SR joins the
  skip set with DFF/DLATCH/JK/T), `optimize.hpp` sequential guards (SR
  skipped like every other sequential kind), YAML `version: 2` schema
  (SR rows ride `valid_fanin`; no bump).

**Rationale**: Keeps the "advances step only the edge kinds" contract
(US3) with the smallest diff — one filter predicate plus one snapshot
guard — while preserving every existing topology invariant.

## R7. What changes per file (grounded in current tree)

- `include/wengine/logic/types.hpp`: `kSrlatch = 20`;
  `gate_kind_name` `"SRLATCH"`; `next_counter` 20 → 21 (comment: indices
  8..15 unused, 16 DFF, 17 DLATCH, 18 JK, 19 T, 20 SR).
- `include/wengine/logic/netlist.hpp`: `valid_fanin` (SR==2);
  `default_fanin` (SR→2); `is_sequential_kind` += SR (explicit, per the
  "never silently DFF-shaped" comment); `parse_gate_kind` table +=
  `{"SRLATCH", kSrlatch}` (+ `"SR"` alias only if the existing alias
  pattern warrants it — default is the full name only, mirroring
  `DLATCH`) with want-string update; `resolve_gate` `kAllKinds` += SR;
  `parse_pin` SR rows (S→0, R→1) ahead of the Q/NQ checks.
- `include/wengine/logic/eval.hpp`: SR joins the existing sequential
  fallthrough (comb eval never sees latches). Bit-parallel `sink_word`
  needs no change (index-driven).
- `include/wengine/logic/seq_state.hpp`: unchanged (generic over
  `is_sequential_kind`; serials/attach/validation cover SR).
- `include/wengine/logic/seq_step.hpp`: new level-settle helper
  (R4, reusing `eval_comb_phase1` + `resolve_step_input`); snapshot loop
  skips DLATCH/SR (edge-exempt); header comment updated (SEQ4
  simplification note replaced by the level rule + SEQ8 pointer).
- `include/wengine/logic/optimize.hpp`: audit every `switch (kind)` /
  sequential guard — combinational rewrites must skip SR (as they do
  DFF/DLATCH/JK/T); retag/`canonical` logic must not claim SR.
- `src/logic/yaml_store.cpp`: `pin_text` canonical `SRn.S|R`;
  kind want-strings (`want AND|...|SRLATCH`); loader accepts
  `inputs: 2` (SR) via `valid_fanin`. No schema version bump (still
  `version: 2`), mirroring 002/003.
- Session layer (`src/cli/commands/logic/`): `common.cpp`
  (`pin_text` SR rows, `clock_domains` edge-only filter,
  `apply_async_and_report` gains the level-pass call with
  `SR1.S/R: illegal` report text); `step.cpp` (pre-edge level pass;
  level kinds excluded from member stepping; transparent/frozen report
  lines); `poke`/`wire`/`tie`/`load` verbs gain the probe-then-revert
  level check per R5 (no logic-path changes — they call the engine
  helper generically). `support::pin_text` session display mapping
  extended with the same SR rows (the second DFF/DLATCH/JK/T-only
  mapping noted in 003 R6).
- Help/completion (`logic_registry.hpp`, `add.cpp` `kCompleteKinds`,
  `common.cpp` pin completion): kind list += SRLATCH with the pin table
  + the two normative sentences (contracts C6); tab-completion suggests
  `S`, `R` after `SRn.`.
- `docs/logic.md` (§12 kind table + level tables + domain-exclusion
  note) + `README.md` (logic row, run examples, test list).
- Tests: extend `test_logic_seq_kinds` (ordinal/name/parse/pins for SR),
  new/expanded level tests (`test_logic_seq_step` R3 tables vs hand
  reference incl. illegal refusal, transparent/frozen across advances,
  determinism replay), CLI verb goldens
  (`test_logic_seq_cli_*`: add/poke/step/peek/wire/list + illegal paths),
  `test_logic_yaml` (SR round-trip + legacy files load unchanged).
  `test_cli_args.cpp` only if top-level completion changes (it should
  not — kinds complete inside the existing `logic` command).

## R8. Risks and mitigations

- **Missed `switch (kind)` site** (silent fallthrough): mitigated by
  `-Wswitch` (zero-warning gate turns it into a build break where
  switches lack `default`), plus a grep audit task and the
  kind-vocabulary test.
- **DLATCH behavior-change regression**: the SEQ4 edge-capture path has
  golden tests (`test_dlatch_edge_capture_seq4`, EN-hold tests). They
  are *updated*, not deleted, to the R3 table — and SC-004 is scoped as
  "modulo the documented DLATCH edge-to-level refinement", so the one
  intentional output change is explicit and help-documented.
- **Illegal-combination atomicity gap** (partial commit on one path):
  mitigated by the resolve-all-then-commit discipline (single pass
  returns failure before any `st.q` write) plus per-verb revert tests
  (poke/wire/tie/load/step illegal paths each assert state-untouched).
- **Scope creep toward gated SR / async JK controls**: explicitly out
  (spec assumptions); review gate rejects EN-on-SR diffs.
