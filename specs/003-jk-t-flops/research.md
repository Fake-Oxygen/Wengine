# Research: JK/T Flop Kinds

**Feature**: `specs/003-jk-t-flops/spec.md` | **Date**: 2026-11-01

All unknowns resolved — no NEEDS CLARIFICATION remains. Every decision is
grounded in the current tree (002 established the extension pattern).

## R1. How to carry JK/T: new kinds vs. DFF modes

**Decision**: Two new appended `GateKind` codes — `kJk = 18`, `kT = 19` —
with fixed fan-in (JK = 3: `J`/`K`/`CLK`; T = 2: `T`/`CLK`). `default_fanin`
returns the fixed count per new kind (`add JK` ≡ 3 inputs, `add T` ≡ 2
inputs); any other `--inputs` value is rejected by `valid_fanin`.

**Rationale**: The spec calls for "small additive kind extension" on the
same stepper. Kind-indexed machinery already exists everywhere that matters:
per-kind counters/serials (`next_counter`, `SeqState` attach), `parse_pin`
named rows, `pin_text` canonical names, per-kind display names
(`JK1`, `T1`), YAML kind spelling. A mode flag on DFF would need a parallel
branch at every one of those sites plus a second snapshot rule inside the
DFF path — more concepts, not fewer.

**Alternatives considered**:
- DFF mode bit (e.g. `is_jk` on `Gate`) — rejected: splits the DFF snapshot
  path, duplicates pin dispatch, breaks per-kind counters/naming.
- Emulate in session (DFF + combinational feedback) — rejected: this is
  exactly what the spec says to stop requiring.

**Append-only rule** (`types.hpp`, `docs/logic.md` §12): persisted bytes
never shift — DFF=16, DLATCH=17 stay; JK=18, T=19 appended. `next_counter`
grows 18 → 20 entries. `test_logic_seq_kinds.cpp` ordinal
`static_assert`s extended (the compiler plus `-Wswitch` coverage enforce
the remaining `switch (kind)` sites: `gate_kind_name`, `eval_gate`,
bit-parallel eval, `optimize.hpp`).

## R2. Pin vocabulary and indices

**Decision**: Fixed positions, same conventions as DFF/DLATCH:

| Kind | 0 | 1 | 2 | CLK name | Q/NQ |
|------|---|---|---|----------|------|
| JK | `J` | `K` | `CLK` | `CLK` | outputs, `NQ` live negation |
| T | `T` | `CLK` | — | `CLK` | outputs, `NQ` live negation |

Case-insensitive named matching; numeric aliases (`.1`/`.2`/`.3`) keep
working; named pins on combinational kinds stay rejected; new-kind pins on
the wrong kind rejected exactly like out-of-range numerics today.

**Rationale**: `parse_pin`, `pin_text`, wire/tie/ignore/`use`, block-input
composition, and NQ polarity are all pin-index-driven — fixed indices make
every verb (FR-006) work with table-entry-sized diffs, mirroring how
RST/SET/EN rode indices 2/3/4 in 002.

**Dangling default** (FR-007): unconnected J/K/T read `kLow` via the
existing rules (ignored-dangling-ties-to-`kLow`, poke-map compose default)
— i.e. the hold row — so bare new flops hold state and existing netlists
are untouched.

## R3. Excitation semantics (independent reference for tests)

**Decision**: Synchronous evaluation in the snapshot phase, per flop in
slot order, from OLD Q. Hand-derived tables (tests assert against THESE,
never against implementation output):

JK (`q` = old Q):

| J | K | Q_next |
|---|---|--------|
| 0 | 0 | q (hold) |
| 0 | 1 | 0 (reset) |
| 1 | 0 | 1 (set) |
| 1 | 1 | NOT q (toggle) |

T (`q` = old Q):

| T | Q_next |
|---|--------|
| 0 | q (hold) |
| 1 | NOT q (toggle) |

**Rationale**: The textbook JK/T definitions; the spec pins them as FR-003/
FR-004. Toggle reads OLD Q (parallel update — swaps and synchronous
counters behave, same as the DFF commit rule).

**Rules carried over unchanged**: `CLK` validated `kLow`/`kHigh` but not
gating (SEQ4 "the edge itself is the call", same as DFF); X/Z on any
resolved J/K/T/CLK fails loudly with state untouched (existing rule);
subset stepping (`seq_step_filtered` members) skips non-members exactly as
today; commit is slot-order parallel.

## R4. No async path for JK/T

**Decision**: `apply_async_controls` unchanged (DFF-only filter stays).
No RST/SET/EN pins on the new kinds in this feature (spec assumption).

**Rationale**: The "set/reset" rows are synchronous excitation rows, not
preset/clear pins. An async path would need new pins, new session
mutations, and new priority documentation — scope the spec explicitly
excludes ("Async controls remain DFF-only"). Hold is expressible as
J=K=0 / T=0.

## R5. Stepper integration point

**Decision**: One branch in the `seq_step_filtered` snapshot loop, after
the DFF full-control block, before the plain `next_q[s] = d` capture:

```text
if kind == kJk: resolve J(0), K(1) [D/CLK already resolved as d/clk slots 0/1 -> use J/K indices];
  next_q = !J&&!K ? q_old : !J&&K ? Low : J&&!K ? High : NOT q_old
if kind == kT: resolve T(0);
  next_q = T ? NOT q_old : q_old
else (DFF bare / DLATCH): next_q = d   # unchanged
```

(D/CLK slots 0/1 are already resolved+validated by the shared prelude, so
the JK branch resolves J=0/K=1 and reuses the validated CLK; the T branch
resolves T=0 the same way. Exact C++ spelling lands in `tasks.md`.)

**Rationale**: Single shared snapshot/commit keeps the two-phase contract
(phase-1 comb with OLD Qs, slot-order snapshot, slot-order commit, phase-2
re-settle) identical for all kinds — US3 (mixed stepping) falls out with no
separate path, satisfying FR-005.

**Alternatives considered**: Separate JK/T stepper function — rejected:
duplicates phase-1/phase-2/commit and risks drift between kind paths.

## R6. What changes per file (grounded in current tree)

- `include/wengine/logic/types.hpp`: `kJk = 18`, `kT = 19`; `gate_kind_name`
  `"JK"`/`"T"`; `next_counter` 18 → 20 (comment: indices 8..15 unused,
  16 DFF, 17 DLATCH, 18 JK, 19 T).
- `include/wengine/logic/netlist.hpp`: `valid_fanin` (JK==3, T==2);
  `default_fanin` (JK→3, T→2); `is_sequential_kind` += JK/T (explicit, per
  the "never silently DFF-shaped" comment); `parse_gate_kind` table +=
  `{"JK", kJk}`, `{"T", kT}` + want-string update; `resolve_gate`
  `kAllKinds` += JK/T; `parse_pin` JK rows (J→0, K→1, CLK→2) and T rows
  (T→0, CLK→1) ahead of the Q/NQ checks.
- `include/wengine/logic/eval.hpp`: `eval_gate` + bit-parallel word eval —
  JK/T join the existing sequential fallthrough (comb eval never sees
  flops). Bit-parallel `sink_word` needs no change (index-driven).
- `include/wengine/logic/seq_state.hpp`: unchanged (generic over
  `is_sequential_kind`; serials/attach/validation cover the new kinds).
- `include/wengine/logic/seq_step.hpp`: snapshot branch per R5; comments
  updated (SEQ4 header rules + DFF-control comment stays DFF-scoped).
- `include/wengine/logic/optimize.hpp`: audit every `switch (kind)` /
  sequential guard — combinational rewrites must skip the new kinds (as
  they do DFF/DLATCH); retag/`canonical` logic must not claim JK/T.
- `src/logic/yaml_store.cpp`: `pin_text` canonical J/K/CLK + T/CLK;
  kind want-strings (`want AND|...|JK|T`); `ignore:` range validation is
  `input_count`-driven (no change); loader accepts `inputs: 3` (JK) /
  `inputs: 2` (T) via `valid_fanin`. No schema version bump (still
  `version: 2`), mirroring 002.
- `src/cli/commands/logic/common.cpp` (+ `logic_registry.hpp` if kind
  lists live there): help kind list += JK/T with pin tables + the two
  excitation sentences (contracts C6); tab-completion suggests
  `J`/`K`/`CLK` after `JKn.`, `T`/`CLK` after `Tn.`. `support::pin_text`
  (session display names) is a second DFF/DLATCH-only mapping beside the
  YAML one — extended with the same JK/T rows (implementation addendum:
  found via a `poked T1.D=1` mislabel during T009; behavior was already
  correct).
- No changes to `add`/`poke`/`step`/`peek`/`wire`/`tie`/`ignore`/`use`/
  `save`/`load` logic paths — they are kind-table-driven and pick up the
  new kinds via the engine tables (verified by tests, not by inspection).
- `docs/logic.md` (§12 kind table + excitation tables) + `README.md`
  (logic row, run examples, test list).
- Tests: extend `test_logic_seq_kinds` (ordinals/names/parse/pins),
  `test_logic_seq_step` (R3 tables vs hand reference, mixed-kind stepping,
  determinism replay), CLI verb goldens
  (`test_logic_seq_cli_*`: add/poke/step/peek/wire/list), `test_logic_yaml`
  (JK/T round-trip + legacy files load unchanged). `test_cli_args.cpp`
  only if top-level completion changes (it should not — kinds complete
  inside the existing `logic` command).

## R7. Risks and mitigations

- **Missed `switch (kind)` site** (silent fallthrough): mitigated by
  `-Wswitch` (zero-warning gate turns it into a build break where switches
  lack `default`), plus a grep audit task and the kind-vocabulary test.
- **`"T"` single-letter parse collisions**: `parse_gate_kind` is exact-match
  only, and no existing kind/pin is named `T` — verified by the near-miss
  assertions extended in the kinds test.
- **Scope creep toward async JK controls**: explicitly out (R4); review
  gate rejects RST/SET/EN-on-JK diffs.
