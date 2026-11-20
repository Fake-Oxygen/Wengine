# Research: Enable-Gated Buffer plus Per-Net Pulls (012)

**Date**: 2026-11-20
**Inputs**: specs/012-gated-buffer-pulls/spec.md, codebase research
(engine seam, CLI surface, YAML store, optimizer passes, draw paths,
tests/benchmarks), specs/010 + specs/011 artifacts.

All resolved decisions below were deferred-to-planning in the spec's
Assumptions; each now has a concrete, precedent-anchored choice.

---

## D1 — Kind identity: `kTbuf = 8`, name "TBUF", exactly 2 inputs

**Decision**: the buffer appends at byte 8 — the first free
combinational-range byte — named `TBUF`, display `TBUF<n>`, fan-in
exactly 2 (`allowed_mask 0x02`, default 2). Input 1 is data, input 2 is
the active-high enable; addressed numerically (`TBUF1.1`, `TBUF1.2`).
`kKinds` gains one row; `kind_table_valid` count 13 → 14 (combinational
checks — no face, no role anchors, no clock/notch — apply unchanged);
`parse_gate_kind` want-string gains `TBUF`; `gate_kind_name` gains the
arm; the `add_gate` unknown-kind guard (`ord >= 8 &&
!is_sequential_kind`, netlist.hpp:68) admits byte 8 while still
failing loudly on 9..15. `next_counter` (21 slots, ordinals 0..20)
covers byte 8 with no resize; `resolve_gate` prefix matching falls
through correctly (`TBUF1` vs the `T` row fails the numeric tail and
continues — the existing pattern).

**Rationale**: byte 8 is the only choice that fits the existing
counter table without resizing it; the name is short, BUF-family
consistent, and prefix-unambiguous. Numeric pins keep `parse_pin`
untouched (names on combinational gates stay rejected); active-low
enable composes with `NOT` exactly as every other polarity need does.

**Alternatives considered**:
- Byte 9+: no reason to skip 8; rejected.
- Complementary variants (active-low / inverting-data kinds):
  out-of-scope per spec; `NOT` composes; rejected.
- Named `D`/`EN` pins: requires opening `parse_pin` to combinational
  names — scope beyond the contract (numeric pins are complete);
  rejected, recorded as a possible authoring follow-on.

## D2 — Buffer evaluation: `eval_gate` truth table, loud input seams

**Decision**: `eval_gate(kTbuf, {data, en}, 2)` returns `data` when
`en == kHigh`, else `kHighZ`. Precondition (both Low/High) is enforced
by `resolve_input` at the buffer's own input seams: X/Z/dangling on
either input fails loudly with state and timing untouched
(spec FR-004). The four-row table (EN×D → out: 0,0→Z; 0,1→Z; 1,0→L;
1,1→H) is verified against independent derivation, never the
implementation's output. `Gate` needs no shape change
(`drivers[0]` = data, `drivers[1]` = enable); `ignore` on buffer
inputs keeps existing semantics (ignored dangling ties Low —
an ignored enable parks the buffer at Z by construction, loudly
visible, never silent).

**Rationale**: the table is the only reading of "enable-gated" that
keeps the funnel total and the consumer contract intact; loud seams
are the module's standing gate-input convention, not a new rule.

**Alternatives considered**:
- Propagating X data while enabled: contradicts the loud-X-input
  convention every gate keeps; rejected.
- Defaulting an undriven enable (on or off): silent invention;
  rejected — dangling enable fails loudly like any dangling input.

## D3 — Pull storage: additive `pull_cfg` flat array, 8 KB

**Decision**: `Netlist::pull_cfg[flat]` (`std::array<std::uint8_t,
kMaxGates * kMaxInputs>`, 0 = none, 1 = up, 2 = down), indexed by the
existing `tie_flat(slot, input)` scheme — the same granularity as
`shared_meta`. Zero-initialization is the all-none default, so
`Netlist{}` starts pull-free and value equality stays
defaulted-structural. Memory add is 8 KB (`sizeof(Netlist)` ~150 KB →
~158 KB), same class as `tie_rep`/`driver_inv`. Pulls are persistent
topology state (set beside the pool, serialized per D6); contention
counts stay session diagnostics untouched.

**Rationale**: matches the dense-flat, fixed-capacity, header-only SoA
convention exactly; a separate array avoids churn on every hot reader
(the 011-D2 rationale against packing into already-loaded bytes).

**Alternatives considered**:
- Packing the pull into `shared_meta` spare bits (bits3..4): saves 8 KB
  but makes every shared writer bit-preservative (churn risk across
  wire/unwire/remove_gate/optimizer writers); rejected for v1.
- Per-`Gate` storage: wrong granularity (pulls are per input, and
  gates hold no per-input bytes today); rejected.

## D4 — Resolution integration: pull maps the all-Z outcome, nowhere else

**Decision**:
- **Shared branch** (`resolve_input` marked path): after the unchanged
  fold, an all-Z result maps to the pull's idle level (up→High,
  down→Low, none→Z). Levels, X, and unknown-dominance resolve exactly
  as today under every pull setting. Contention counting is computed
  at span assembly (low+high present) **before** the pull map — a pull
  never creates or masks a count.
- **Single-driver branch**: gains the loud level check the shared
  branch already has — a non-level sunk value (reachable only via
  Z-producing drivers, impossible pre-012) fails loudly instead of
  flowing into `eval_gate` (whose `is_high(Z) == false` arm would
  silently read Z as low). A Z sunk with a pull maps to idle; without
  a pull it fails loudly.
- **Dangling inputs**: a pulled dangling input is *terminated* —
  `is_block_input` returns false for it (block counts, interface
  derivation, `input_index`, and `poke` all see a terminated net, never
  a block input), and `fill_resolved` resolves it to the idle level
  without consuming a block-input column. This gives spec US2's "no
  drivers at all → idle" uniformly for unmarked inputs; marked-empty
  inputs already take the shared branch (which never takes the
  dangling path — block_interface.hpp:418-420).
- **Disjointness** (both directions loud, netlist untouched):
  setting a pull on a tied or ignored input fails; tying or ignoring
  a pulled input fails (mirror of the tie/shared-mark disjointness).
- **Word path**: `truth`/`export --truth` refuse loudly when any live
  `kTbuf` gate exists (Z has no 1-bit representation) — new
  `has_buffer_gates(nl)` entry guards at the same three sites as the
  011 guard (truth.cpp, `write_truth_csv`, `write_truth_pretty`).
  Pulls alone never block truth: terminated dangling inputs simply
  shrink the interface; pulled wired nets with active drivers are
  pull-irrelevant. `eval_block_word`'s mandatory new `kTbuf` switch
  arm returns a loud failure (safety net for future callers —
  unreachable through the guarded entries, zero per-word cost).
  `bench_logic` builds kinds 0..7 only: untouched.
- **Z block outputs**: a dangling disabled-buffer output is a legal
  observable Z. `solve`/`step` render it explicitly (`Z` arm beside
  the `0`/`1` arms in solve.cpp:145, `bit_of` in step.cpp:57 and
  common.cpp:47) — reachable only with buffers, so v1 flows print
  byte-identically.

**Rationale**: the pull is a post-fold map on exactly one funnel
outcome (all-Z) — no strength, priority, or pre-dominance path, so
agreement/contention/unknown semantics cannot shift. Termination
semantics make the physics explicit (a resistor to the rail *is* a
driver) and keep one value per net with no dual-source ambiguity.

**Alternatives considered**:
- Pull as strength/priority over driven levels: contradicts the spec
  ("a pull never outvotes anything"); rejected.
- Mapping before X-dominance (pull swallowing contention): would paper
  over fights; rejected.
- Pulled dangling stays a block input with a default: two sources for
  one net (block value vs pull); rejected — termination is unambiguous.

## D5 — Authoring surface: `pull` / `unpull` verbs (tie/untie precedent)

**Decision**: `pull <TO.i> <up|down>` sets the pull;
`unpull <TO.i>` clears it (`LogicRegistry` 23 → 25). `pull` refuses
loudly (usage, exit 2) on dead/out-of-range pins, output pins, tied
inputs ("untie it first"), ignored inputs ("use first"), and unknown
level spellings; `unpull` refuses loudly when no pull is set. Success
lines name the net and level (nothing rides silently). `wire` is
unchanged — pulled destinations wire normally, and post-wire
async/level settle observes resolved idle values as ordinary data.
`unwire` of the last driver onto a pulled net leaves the pull stored:
the net becomes terminated-dangling (block interface shrinks by
construction). Completion: input pins + `up`/`down` values for `pull`;
input pins for `unpull`.

**Rationale**: the two-verb set/clear shape mirrors `tie`/`untie` and
`share`/`unshare` (mark lifecycle precedent, 011 D1); a pull is a
net property (settable on dangling inputs too), not an attachment
mode, so a `wire --pull` flag would be the wrong seam.

**Alternatives considered**:
- Single verb with `none` (`pull NET none`): breaks the set/clear
  two-verb precedent for zero gain; rejected.
- `--pull` flag on `wire`: cannot express pulls on dangling inputs
  and conflates attachment with property; rejected.

## D6 — Workspace schema: additive `pulls:` section, no retirement

**Decision**: no generation bump, no retirement; `kWorkspaceVersion`
stays `1`. New constructs: optional top-level `pulls:` section
(written iff ≥1 pull exists; absent on read = no pulls), one row per
pulled input in slot order (`- {net: AND2.2, pull: up}` — only
`up`/`down` spellings; `none` is absence, never written); `tbuf` kind
rows in the existing lowercase row shape; canonical save order
`version, gates, wires, shared, pulls, ties, clocks, meta`. Loader:
unknown levels/sections stay loud; duplicate `pulls` rows for one net
fail loudly; pre-012 files (no `pulls:`, no `tbuf` rows) load
byte-identically. The decision is recorded loudly in docs/logic.md
schema notes under the 011-D5 convention (additive in-generation
change; the no-fallback-loader rule governs retired generations, and
none retire here).

**Rationale**: nothing existing is reinterpreted and no new required
section exists — the exact 011-D5 situation, so the same recorded
additive path applies; retiring every current file would be pain
without semantic need. Spec US4.2 explicitly allows "carried" as the
recorded loud decision.

**Alternatives considered**:
- Re-pin marker + required `pulls:` section (006-style): retires all
  current files for no ambiguity gain; rejected.
- Per-wire inline pull keys: pulls live on inputs including dangling
  ones with no wire row; rejected (section is the source of truth,
  mirroring `shared:`).

## D7 — Optimizer policy: freeze the buffer, skip pulled inputs, refuse O2 loudly

**Decision** (spec FR-016 made concrete; every `drivers[]`/kind
decision site from the research inventory is covered):
- **Buffer-gate freeze**: no pass folds, drops, or retags a `kTbuf`
  gate — its 2-input shape is load-bearing (`drop_inputs` to n=1
  would retag to a fan-in-invalid 1-input buffer). Concretely:
  `const_fold_pass` and root-collapse skip gates of kind `kTbuf`
  (same lines as the `has_marked_input` skips); `bypass_pass` is
  structurally safe (keyed on `kBuf`/`kNot` — `kTbuf` never matches);
  `classify` leaves `kTbuf` at `kV` via its default arm (documented,
  so no fold can lift it).
- **Sink-side skip on pulled inputs**: `const_fold_pass` /
  collapse skip gates with any pulled input (new `has_pulled_input`
  predicate beside `has_marked_input` in optimize_barrier.hpp) —
  index shifts would desynchronize flat-keyed `pull_cfg`, the exact
  011 extras rationale.
- **No driver-side freeze for buffer feeders**: unlike shared spans
  (extras bookkeeping), buffers are ordinary `drivers[]` consumers —
  value-preserving rewrites of their drivers are sound, and no pass
  keys on buffer inputs. `dedup_key_for` distinguishes `kTbuf` by kind
  automatically. (If review finds a pass that cannot prove safety for
  a buffer-feeding rewrite, the standing rule applies: leave it
  untouched — the skips above are the floor, not the ceiling.)
- **`exact_simplify_pass`**: conservative whole-net skip when live
  buffers or any pull exist (the existing sequential-refusal flavor,
  extended — mirrors `any_shared_mark`).
- **O2 / O2-min**: loud refusal when live `kTbuf` gates or non-none
  pulls exist (optimize.hpp:124 disjunct) — silently emitting
  buffer-stripped output under an O2 request would be a silent
  contract break. A verbatim-carry O2 emitter is the named follow-on.
  `optimize_o2_emit`'s mandatory new `kTbuf` switch arm is a
  loud-failure arm (unreachable through the refusal, keeps `-Wswitch`
  clean).
- **Schematic draws** (`logic_draw_sdl/imgui.cpp`): `kTbuf` renders
  the BUF triangle with an enable side-stub (standard tri-state
  symbol); layout boxes are input-count-sized (kind-agnostic —
  verified at implementation).

**Rationale**: the freeze/skip/refuse ladder is the proven 007/011
soundness pattern applied at exactly the points where a buffer or a
flat-keyed pull could be observably altered; everything else rides
unchanged because buffers use the ordinary `drivers[]` representation.

**Alternatives considered**:
- Whole-verb refusal for O0/O1: blocks optimizing the untouched 90%
  of a netlist; FR-016 asks only for observably-unchanged results;
  rejected.
- Correct O2 verbatim carry now: emitter surgery across
  wire_pending/emit paths for a rare construct; named follow-on;
  rejected for v1.

## D8 — Readback: pulls in `list`, Z in value lines, contention line unchanged

**Decision**: `list` annotates pulled inputs (net + level, beside the
shared-mark annotations); buffer/pull-free netlists list
byte-identically. `solve`/`step` value lines render Z outputs
explicitly (D4). No new status line: pulls are visible in the values
themselves (idle levels flow as data), and the 011 `contention:` line
keeps its shape and zero-inclusive presence rule (counts are computed
pre-pull-map, so pull settings never alter it). `reset` clears counts
(unchanged); pulls persist (topology, like marks).

**Rationale**: observability without new vocabulary — the pull's
effect *is* the resolved level, inspectability lives in `list`,
exactly the 011 division between readback and status.

## D9 — Identity and caps: existing conventions cover both

**Decision**: buffer gates take per-kind monotonic never-reused
counters from `next_counter[8]` (display `TBUF<n>`; slots recycle,
counters don't — the stable-identity convention, no new machinery).
Buffers attach to shared nets as ordinary attachments within the
existing cap (4 per input, loud overflow) — no second cap, no
per-net pull configurability beyond up/down/none. Duplicate
attachments keep the 011-D8 semantics (agreeing duplicates resolve
as agreement; Q+NQ self-contends).

**Rationale**: no new identity or capacity vocabulary is needed — the
shipped schemes already say the right thing.

## D10 — TUI, docs, registry sync (constitution: README is a living spec)

**Decision**: no TUI change (parity by construction — no logic form
exists, per 011 research). Registry pins to sync in
`tests/test_cli_args.cpp`: `LogicRegistry::all().size() == 25`,
per-verb `find` blocks for `pull`/`unpull`, `verbs.size()` asserts,
the `p`-prefix completion vector gains `pull`, the `u`-prefix vector
gains `unpull` in registry order, `add` kind want-strings and
`kCompleteKinds` gain `TBUF`. README: verb sentence gains
`pull`/`unpull` + the TBUF flow; a runnable bus example; test list
gains the new binary; module table's `logic` row mentions gated
buffers and pulls. `docs/logic.md`: §10 verb grammar gains
`pull`/`unpull` + TBUF (pins 1=data/2=enable); §9 single-driver rule
gains the termination note; §11 non-goals drops the tri-state/pull
gate entries; §12 Drivers slot ships buffer authoring + pull mapping +
O2 follow-on. `docs/tests/logic.md`: page for the new binary; catalog
audit stays zero-missing/zero-ghost. One new example joins
`examples/logic/` (`buffer_bus.yaml` — gated bus with pulls, in the
automatic `test_examples` golden table).

## D11 — Fast-path verification plan (constitution IV: measured only)

**Decision**: `bench_logic 8 8 2000` runs before/after with identical
arguments (docs/benchmarks/logic.md protocol). Structural facts: the
scalar `n ≤ 1` path gains one level-check branch (D4 — provably
untaken pre-012, since Z-producing drivers cannot exist); the shared
branch gains the pull map only on the Z arm; the word seam gains no
per-word branch (entry guards only). Acceptance: within run-to-run
noise. Contingency if measurable: hoist the pull lookup behind the
Z test (already the written structure) or relocate the level check to
the Z-producer registration — re-measure either way.

**Rationale**: the done-criteria demand "single-driver fast path
unchanged in bench" — this is the measurement that makes it a claim
rather than an assumption.

---

## Resolved NEEDS-CLARIFICATION ledger

None remain. The spec deferred: buffer shape/polarity (D1 — active-high,
2-input, numeric pins), pull granularity/persistence (D3/D6 — per-input,
persistent, additive schema), strength model (none — D4 post-fold map),
tie/pull disjointness (D4 — refused both directions), authoring shape
(D5 — pull/unpull verbs), optimizer strategy (D7), cap treatment (D9 —
existing cap). Technical Context carries no NEEDS CLARIFICATION markers.

## Facts that bind the implementation (from research)

- `resolve_net` keeps exactly its contract (total, order-independent);
  all pull behavior is a post-fold map on the all-Z arm — the funnel
  needs no edit.
- `gate_out[]` can now hold `kHighZ` (disabled buffers); the only
  silent-as-low hazard was `eval_gate`'s `is_high` arm — closed by the
  D4 loud level check (single-driver) and the existing shared-branch
  check. `fill_resolved` never sees Z (pulled dangling short-circuits
  before the block-value path; X/Z block inputs still refuse loudly).
- `kNoDriver` (0xFFFF) sentinel uses stay valid: pulls never fabricate
  a driver slot (termination is a value rule, not a wiring).
- The `add` verb's kind lists are string tables (add.cpp:31/:38/:95-98)
  plus registry usage text — all gain `TBUF`; completion derives from
  `kCompleteKinds` (no table-driven auto-complete to inherit).
- The word-path entry guards live in exactly three places
  (truth.cpp:36, common.cpp:272/:339) — the `has_buffer_gates` guard
  joins each; `eval_block_word` itself keeps its branch structure plus
  the loud `kTbuf` arm.
- Optimizer decision points: barrier predicates
  (optimize_barrier.hpp:48/:72/:111/:138), passes
  (optimize_passes.hpp:141/:146/:283/:339/:462/:465/:507/:510/:589),
  O2 gate (optimize.hpp:124), o2_emit switch
  (optimize_o2_emit.hpp:288-434), exact skip (barrier:357 pattern),
  draw switches (logic_draw_sdl.cpp:222, logic_draw_imgui.cpp:146).
- `test_cli_args.cpp` registry pins (per 011 research): size assert
  (~:689, 23 → 25), `find` blocks, `verbs.size()` (~:813/:819),
  completion vectors (`u` prefix ~:1763-1764, `wire` opts ~:1811-1814);
  `add`/`parse_gate_kind` want-strings pin kind lists in
  pins.hpp:346, add.cpp:31/:38, test_logic_seq_kinds.cpp.
- The YAML store is the single yaml-cpp TU
  (src/logic/yaml_store.cpp) with a platform-free header; all schema
  work stays inside that seam.
