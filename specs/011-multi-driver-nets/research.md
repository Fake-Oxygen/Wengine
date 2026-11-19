# Research: Allowlisted Multi-Driver Nets (011)

**Date**: 2026-11-19
**Inputs**: specs/011-multi-driver-nets/spec.md, five codebase research passes
(engine seam, CLI/TUI surface, YAML store, optimizer passes, tests/benchmarks),
WENGI-14 (LOGIC-V2-VAL2) task notes, specs/010 artifacts.

All resolved decisions below were deferred-to-planning in the spec's
Assumptions; each now has a concrete, precedent-anchored choice.

---

## D1 — Authoring surface: `wire --shared` + new verb `unshare`

**Decision**: The opt-in is a `--shared` flag on the existing `wire` verb;
unmarking is a new `unshare` verb (LogicRegistry 22 → 23).

Semantics:

- `wire FROM.out TO.in --shared` on an unmarked input marks the input
  shared (explicit per-input opt-in, at the exact moment the second
  driver would otherwise be refused) and lands the driver. On an
  already-marked input the flag is accepted idempotently (mark
  unchanged) — the `tie_pins` idempotency precedent
  (tie_set.hpp:261-263).
- Plain `wire` (no flag) rides an existing mark: a marked input accepts
  further attachments without repeating the flag, because the mark is
  the user's prior explicit declaration. Plain `wire` on an **unmarked
  occupied** input keeps today's loud refusal, byte-identical
  (wire.cpp:47-50, netlist.hpp:239-241).
- `unshare TO.in` clears the mark; refused loudly while the input holds
  more than one attachment; refused on an unmarked or tied input.
- Inspectability: `list` shows shared marks and per-input attachment
  counts (FR-002).

**Rationale**: The refusal and the opt-in live at the same seam — a
second driver cannot land without either a prior mark or the flag in
hand, the strongest never-silent property. The common case (turn a
one-driver net into a bus) stays one command. `unshare` mirrors the
module's `tie`/`untie` two-verb precedent for per-input net constructs,
keeping the rare unmark path loud and inspectable rather than
overloading `unwire`.

**Alternatives considered**:
- `share`/`unshare` verb pair only (no flag): cleanest lifecycle but the
  hot path becomes two commands (`share BUS.1` then `wire SRC BUS.1`);
  the recorded task hint (WENGI-14 "wire --shared flag") points at the
  flag; rejected.
- `unwire --shared`: stretches `unwire`'s meaning (it removes a
  driver, not a property); rejected.

## D2 — Storage: additive dense-flat SoA arrays in `Netlist`

**Decision**: `Gate::drivers[]` remains the **primary** driver slot per
input (first attachment; `kNoDriver` semantics for
dangling/block-input detection preserved — block_interface.hpp
`kNoDriver` sentinel uses stay valid). Three new `Netlist` members,
indexed by the existing `tie_flat(slot, input)` scheme:

```text
shared_meta[flat]   uint8  — bit0: shared mark; bits1..2: extra count 0..3
extra_drivers[flat * kMaxSharedExtra + j]  uint16 — extra source slots
extra_inv[flat * kMaxSharedExtra + j]      uint8  — per-extra NQ polarity
```

`kMaxSharedExtra = 3` (4 attachments total per input). Memory add:
48 KB + 24 KB + 8 KB ≈ 80 KB; `sizeof(Netlist)` 69,728 → ~150 KB
(verified baseline from research). Session-singleton scale, same class
as today's `tie_rep` (16 KB) / `driver_inv` (8 KB) dense-flat stores.

Primary's polarity stays in `driver_inv[flat]` — the single-driver fast
path reads exactly today's bytes.

**Rationale**: Matches the established dense-flat, fixed-capacity,
header-only, SoA convention exactly (`tie_rep`, `driver_inv` are the
precedents). No allocation, no pointers, value-equality stays
defaulted-structural. Extras keep per-attachment polarity (spec US2.3:
Q+NQ self-contention requires it).

**Alternatives considered**:
- Per-`Gate` small vectors: inflates every `Gate` (40 B today) for a
  rare per-input property; rejected.
- Shared node pool with per-input chains: variable capacity but
  adds list management to every `drivers[]` reader; rejected.
- Encoding polarity into driver slots (high bit): breaks every existing
  `drivers[]` reader; rejected.
- Packing mark/count into `driver_inv`'s spare bits: saves the 8 KB
  array but makes every `driver_inv` writer bit-preservative (churn risk
  across wire/unwire/remove_gate/optimizer writers); rejected for v1 —
  recorded as the D11 contingency if the bench shows measurable cost.

## D3 — Resolution integration: span assembly at `resolve_input`; word path refuses

**Decision**: `resolve_input` (block_interface.hpp:391-419, the one
funnel every scalar evaluator uses — solve, both seq phases, async and
level apply) gains the shared branch: when `shared_meta[flat]` reports
extras, assemble the span on a stack buffer (cap 4) — primary value
with `driver_inv` polarity, then each extra with its `extra_inv`
polarity, in compact index order — and pass it to the unchanged
`resolve_net`. The `n ≤ 1` path stays structurally today's code
(read `drivers[]`, one-element span, `apply_polarity`): unmarked nets
never assemble a span. All consumers (`eval.hpp:145`, seq_step.hpp
:135/:178/:222/:225/:351/:354/:378/:443) inherit the fold with no
further change.

The word-parallel twin **refuses loudly**: `eval_block_word` /
`resolve_word` cannot carry four values (research: no `resolve_net` on
that path at all). `truth` over a netlist where any shared input holds
≥2 attachments fails loudly naming the construct; marked inputs with
≤1 attachment ride the primary word path unchanged (bit-identical per
FR-006). The guard is a `shared_meta[flat]` byte test in the word seam;
cost measured per D11.

**Rationale**: One fold point (the documented single funnel), zero
call-site churn, and the only structurally four-value-incapable path
gets the module's standard loud refusal instead of silent
misrepresentation.

**Alternatives considered**:
- Word-path span folding: impossible — 1-bit words cannot represent X;
  folding agreeing words per-bit would silently under-report contention.
- Refusing `truth` on any mark (even 0/1-driver marks): coarser than
  needed; ≤1-attachment marks are provably word-identical; rejected.

## D4 — Contention counting: `SolveDiag` out-channel, session-owned, observation-only

**Decision**: A POD diagnostic channel defined beside the funnel
(block_interface.hpp):

```text
SolveDiag { std::array<std::uint32_t, kMaxGates * kMaxInputs> per_net; }
```

`resolve_input` takes `SolveDiag *diag = nullptr` (defaulted; all
existing call sites compile and behave identically). During span
assembly it computes `saw_low && saw_high` across the polarized span —
**before/regardless of** `resolve_net`'s X result — and, when contended
and `diag` present, increments `diag->per_net[flat]`. This is exact
FR-007 counting (low+high disagreement), immune to future X/Z-presenting
drivers (VAL3) which must not false-count as contention.

`solve()` and `seq_step_filtered()` gain the same defaulted pointer.
The CLI session owns one `SolveDiag` beside `poke_map`/`cycle_counts`
(logic.cpp:80-120 anonymous-namespace singleton block, the documented
home), cleared by `reset_seq_session_state()` — cleared by `reset`,
never serialized (FR-009), matching the poke/cycle convention
(docs/logic.md:414-415). `solve`/`step` report per-net counts (see
contracts/cli-verbs.md); the engine `SequentialSolver::step` path passes
nothing — counting is an interactive-module diagnostic, and the Runner
path's loud X-consumer failures are unchanged.

A step's two settle phases each resolve shared nets, so a persistently
contended net counts once per phase — every resolution counts, exactly
as FR-007 words it; the semantics are documented in the contract.

**Rationale**: Counting must not mutate `Netlist` (const-evaluation
path, value-equality, serialization, and determinism all forbid it) —
an out-of-band defaulted channel is the minimal observation seam.
Per-flat fixed array avoids heap maps in engine code (CLI-side map
precedent kept for session state; the array is zeroed once per
session/reset, incremented in place, allocation-free on the hot path).

**Alternatives considered**:
- Counter inside `Netlist`: breaks const-eval, structural equality, and
  the topology/diagnostic separation; rejected.
- Event-append buffer with overflow latch (o2_emit pend-buffer
  precedent): adds a drain protocol and an overflow policy the per-flat
  array doesn't need; rejected.
- Return-code/bitmap tricks on `resolve_input`'s bool: no room; rejected.

## D5 — Workspace schema: additive optional constructs, no retirement, no marker change

**Decision**: No generation bump, no retirement. `kWorkspaceVersion`
stays `1`; pre-011 files load byte-identically (they cannot contain a
shared construct — old savers never wrote one and old loaders refused
duplicate `to:` targets at yaml_store.cpp:649-652). New constructs:

- Optional top-level `shared:` section (written iff ≥1 mark exists;
  absent on read = no marks), listing marked inputs in slot order —
  carries marks with zero attachments too (spec US1 edge).
- `wires:` rows may repeat a `to:` target **iff** that input is
  marked; the existing "input already wired" refusal stays
  byte-identical for unmarked duplicates (root-key and per-wire
  known-key want-strings grow `shared` — unknown-key loudness
  preserved everywhere).
- Canonical save order: `version, gates, wires, shared, ties, clocks,
  meta` (only when marks exist is `shared:` emitted).

The decision is recorded loudly in docs/logic.md schema notes: the
module's no-fallback-loader rule governs **retired** generations; 011
retires none — the additive path is the documented in-generation
convention (`ignore_nq` at 25287f0: "additive: no file predating
creation support can contain them, so no fallback needed").

**Rationale**: The 006 re-pin existed to retire ambiguous prior
generations. Here nothing existing is reinterpreted and no new
required section exists; retiring every current file (8 shipped
examples + user designs) would be pain without semantic need. Spec US4.2
explicitly allows "carried" as the recorded loud decision.

**Alternatives considered**:
- Re-pin marker + required `shared:` section (006-style): retires all
  current files; no ambiguity exists to justify it; rejected.
- Per-wire inline `shared: true` key only (no section): cannot express
  a zero-attachment mark; rejected (section is the mark source of truth).

## D6 — Optimizer policy: driver-side freeze, sink-side skip, exact conservative skip, O2 loud refusal

**Decision** (FR-017 policy made concrete; every `drivers[]` write site
from the research inventory is covered):

- **Driver-side freeze** (the `feeds_seq` pattern, optimize_passes.hpp
  :261-263/:562-564, dedup :157-159, double-inv :251-253): a gate with
  any attachment (primary or extra) into a shared-marked input is
  frozen — never removed, never retagged, never rewired-through.
  `drives_shared(nl, slot)` is the new freeze predicate; DCE treats
  such gates as roots (barrier-root precedent, optimize_barrier.hpp
  :70-75). Guards live at transform decision points, never inside
  `rewire_users` (spec 007 research R2 rationale, verbatim
  application).
- **Sink-side skip**: `const_fold_pass`/`drop_inputs` never fold, drop,
  or retag through a shared-marked input; `classify` marks shared nets
  variable.
- **`exact_simplify_pass`**: conservative whole-net skip when shared
  inputs exist — the exact pass's existing sequential refusal flavor
  (optimize_exact.hpp:98-104), documented.
- **O2 / O2-min**: loud refusal when any shared-marked input exists
  ("optimize --level O2: shared nets unsupported …"). O2 replaces the
  workspace wholesale; silently emitting O1-level output under an O2
  request would be a silent contract break. Input untouched, exit 1,
  O0/O1 remain available. A verbatim-carry emitter is the named
  follow-on.
- **Machinery fixes required for soundness** (not policy — correctness):
  `recount_fanout` counts extra edges; `follow_drivers`/
  `compute_feeds_seq` traverse extras; `remove_gate` sweeps extras
  as-driver (polarity-aware release per extra); `netlists_equal`
  (test oracle) compares marks + extras; `dedup_key_for` never keys on
  frozen gates.

**Rationale**: The conservative freeze is the proven soundness pattern
(spec 007 shipped it for sequential gates); refusing O2 loudly is the
constitution's never-silently-weaker rule. Missed optimization on shared
netlists is the documented price of FR-017's "cannot prove safety →
leave untouched".

**Alternatives considered**:
- Whole-verb refusal for all levels (O0/O1 too): blocks optimizing the
  non-shared 90% of a netlist; FR-017 asks only for untouched shared
  nets; rejected.
- Correct O2 verbatim carry (extras-aware emitter): emitter surgery
  across wire_pending/wire_fresh_with_polarity/emit_seq_verbatim for a
  rare construct; named follow-on instead; rejected for v1.

## D7 — Cap: 4 attachments per input (1 primary + 3 extras)

**Decision**: `kMaxSharedExtra = 3`. Overflow refuses loudly at
`wire` ("shared net full (cap 4)").

**Rationale**: Buses beyond 4 drivers are rare in a 1024-gate sandbox;
memory stays ~80 KB; the number is a named constant, loud-refused,
never silently growable (FR-003). Spec explicitly leaves the value to
planning.

**Alternatives considered**: 8 (matches `kMaxInputs`, but ~172 KB extra
for a rare case); 2 (wired-OR minimum, too tight for real buses);
rejected both.

## D8 — Duplicate attachments carry no special semantics

**Decision**: The same source pin attached twice (same polarity)
resolves as ordinary agreeing drivers; no authoring-level duplicate
refusal in v1. Q+NQ of the same source self-contends (US2.3).

**Rationale**: Spec assumption honored as-is; a duplicate refusal would
be an authoring nicety, not a contract — recording it keeps FR text
honest.

## D9 — Deterministic promotion and compaction

**Decision**: Unwiring the primary promotes extra index 0 into
`drivers[]`/`driver_inv`; unwiring extra j compacts left. All order
effects are pinned to compact index order; attachment/unwiring
sequences in different orders converge to identical array states
(resolution is multiset-order-independent anyway — resolve_net's
shipped contract).

## D10 — Shared CLK nets: no authoring special-case

**Decision**: A shared-marked CLK input is legal to author. Step's
existing X-loud CLK validation (seq_step.hpp:357-360) fails loudly on a
contended clock; domain naming keeps the primary driver
(derive_clock_domains, clock_domains.hpp:68-79), documented as the
edge in docs/logic.md. No new refusal path.

**Rationale**: The loud X paths already cover the fault; a wire-time
special case would add vocabulary the failure already makes
unnecessary.

## D11 — Fast-path verification plan (constitution IV: measured only)

**Decision**: `bench_logic` (word path, ns/gate-word) runs before/after
with identical arguments (e.g. `./build-bench/bench_logic 8 8 2000`,
docs/benchmarks/logic.md:14-18). The scalar `n ≤ 1` path is structurally
untouched (D3); the word seam gains one `shared_meta[flat]` byte test.
Acceptance: within run-to-run noise. Contingency if measurable: pack
mark/count into `driver_inv`'s unused bits (byte already loaded on the
word path) per D2's rejected alternative.

**Rationale**: The done-criteria demand "single-driver fast path
unchanged in bench" — this is the measurement that makes it a claim
rather than an assumption.

---

## Resolved NEEDS-CLARIFICATION ledger

None remain. The spec deferred: authoring shape (D1), storage (D2),
word-path policy (D3), counter home (D4), prior-generation policy
(D5), optimizer strategy (D6), cap value (D7) — all now decided with
codebase-precedent rationale. Technical Context carries no NEEDS
CLARIFICATION markers.

## Facts that bind the implementation (from research)

- `resolve_net` has exactly one production call site today
  (block_interface.hpp:401, span of 1); its contract is total,
  order-independent, and unchanged by this feature.
- `kNoDriver` (0xFFFF) doubles as the "not a block input" sentinel in
  block_interface/poke/compose paths — primary slot representation is
  mandatory (D2).
- The `wire` CLI verb re-checks occupied/tied before the engine and
  probes cycles via tentative mutate+revert on sequential netlists
  (wire.cpp:48-74) — the probe must learn the extra-append branch.
- `test_cli_args.cpp` registry pins to sync: :689 (22 → 23), :813,
  :819, the `u` prefix vector :1763-1764, `wire` option completion
  :1811-1814; README verb sentence :160-162; docs/logic.md §10 verb
  grammar, §11 non-goals (multi-driver leaves the list), §12 Drivers
  slot ("reserved" → shipped authoring).
- `test_examples` loads every file in `examples/logic/` — the new
  example joins its golden table (test_logic_yaml.cpp:719-893).
- The YAML store is the single yaml-cpp TU (src/logic/yaml_store.cpp)
  with a platform-free header; all schema work stays inside that seam.
- No TUI change is required for any of this (no logic form exists;
  TUI parity is by construction via `TuiExecutor` → `cli::Shell`).
