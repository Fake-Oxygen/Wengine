# Research: Flop-Latch Opaque Barrier (Phase 0)

**Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md) | **Plan**: [plan.md](./plan.md)

Method: direct audit of `include/wengine/logic/optimize.hpp` (~2726 lines),
`eval.hpp` (`eval_gate`/`solve`), `netlist.hpp` (fanout/polarity bookkeeping),
and `src/cli/commands/logic/optimize.cpp`. No external unknowns — all
decisions are grounded in current code behavior cited below. All NEEDS
CLARIFICATION resolved by audit; none remain.

## R0. Where does today's optimizer actually destroy or refuse state?

 finding (audit of each pass over a sequential net):

- `classify` (opt:625): initializes every label to `kV`; sequential kinds
  hit the SEQ11 `break` (opt:679) and stay `kV`. Outputs correctly varying.
  **Safe already.**
- `const_fold_pass` drop switch (opt:1000): sequential kinds `break`
  without folding. Constant-root collapse (opt:1009) fires only on `kC0/kC1`
  labels, which sequential gates never carry. **Safe already** (a defensive
  explicit skip in the collapse loop is still added — one line, guards
  against future classify changes).
- `fusion_sweep` (opt:1092): a `NOT` driven by a sequential kind keeps
  `nk == kNot` and is skipped (opt:1116). In-place fusion never touches the
  consumers of the fused gate. **Safe already.**
- `bypass_pass` (opt:752) + fusion double-negation branch (opt:1086): only
  `BUF`/`NOT` gates are removed, but `rewire_users` (opt:731) rewrites
  **every** consumer — including sequential-gate inputs fed by the removed
  gate. **GAP (rewrites a barrier pin).**
- `exact_simplify_pass` (opt:130): calls `solve()`, and `eval_gate`
  (eval:72) reads every sequential output as `kLow`. On an acyclic
  sequential net (pipeline, no feedback cycle) `ensure_topology` succeeds,
  so the pass builds a **bogus truth table** (all state reads 0), then can
  collapse sequential roots into `AND(0,0)` constants and tie off dangling
  flop inputs as "dead" (opt:249 loop iterates all live gates, seq
  included). On cyclic nets `solve` fails loudly and the pass returns false
  (safe by accident). **GAP (deletes/mangles state on acyclic nets).**
- `dce_pass` (opt:693): roots are live fanout-0 gates only. An unobserved
  flop (fanout 0) is a root and survives — but a closed state cluster with
  no path to any block output (e.g. a free-running counter driving nothing)
  has no roots at all and is **deleted wholesale**. **GAP (deletes state).**
- O2 reachability (opt:2458): reverse DFS from sinks walks through
  sequential drivers — driver cones are kept. **Safe already.**
- O2 creation via `emit_gate` (opt:1414): sequential kinds `break` to
  `kNoDriver`, and the caller reports `optimize: gate budget exceeded` —
  so **O2 loudly fails on every sequential net today**. **GAP (refusal).**
- `try_minimized_o2` (opt:2159): returns false when any sequential gate is
  live → falls back to structural conversion. **Safe already, keep.**
- O2 post-cleanup: `dedup_universal_pass`/`double_inv_universal_pass`
  merge only the universal kind `u` (never sequential kinds). Post `dce_pass`
  (opt:2686) roots only original-sink fresh gates. **DCE roots GAP (same as
  O0/O1 for the fresh net).**
- `recount_fanout` (opt:513): recomputes `output_fanout` only, never
  `nq_fanout`; `optimize.hpp` never touches `driver_inv` (verified: zero
  hits). NQ polarity therefore cannot survive any fresh-build path unless
  explicitly carried. **GAP (O2 must copy `driver_inv` per pin and recount
  NQ fanout for fresh gates).**
- CLI verb (`src/cli/commands/logic/optimize.cpp`): forwards to
  `optimize()` and reports before/after counts plus interface changes; no
  sequential rejection at the CLI layer. **No change needed.**

## R1. DCE protection — what becomes a root?

- **Decision**: DCE roots = original sinks (as today) **plus every live
  sequential gate**, at all levels: the O0/O1 fixpoint `roots[]`
  (opt:2397) and the O2 post-cleanup `post_roots[]` (opt:2659, applied to
  fresh sequential gates).
- **Rationale**: State readback observes flops outside the block interface
  (FR-004), so fanout reasoning can never make a state element dead. This
  is the smallest change that fixes closed-cluster deletion; backward cone
  traversal already keeps driver cones of kept flops, which is required and
  correct.
- **Alternatives considered**: (a) Keep fanout-0-only roots and rely on
  sinks — rejected: fails the free-running-counter case (Spec Story 1,
  scenario 3 generalization). (b) Root only flops with fanout 0 —
  equivalent to today, fixes nothing. Rejected.

## R2. Pin freeze — may upstream combinational simplification re-point a flop input?

- **Decision**: Strict freeze for O0/O1 in-place passes — **no pass writes
  to any field of a live sequential gate** (`kind`, `input_count`,
  `ignored_mask`, any `drivers[]` entry), and **no pass rewires a consumer
  away from a sequential output pin** (kind, slot identity, and
  Q/NQ polarity preserved). Concretely: `bypass_pass` and the fusion
  double-negation branch skip removing a `BUF`/`NOT` that has any live
  sequential consumer (new `has_sequential_user` guard); the redundant gate
  survives and DCE keeps it as a driver of the kept flop. O2's fresh build
  re-emits flop inputs pointing at the corresponding fresh survivors via
  the existing `emit_map` (the spec-blessed "surviving canonical source"
  re-point, generalized to any surviving source through the map).
- **Rationale**: "Opaque barrier" means the pin is never an optimization
  subject. A surviving redundant `BUF` feeding only a flop costs one gate
  and is still correct; skipping removal keeps `rewire_users` untouched for
  the combinational case and makes the freeze uniform and reviewable.
  In-place O1 fusion already preserves driver slots (same-slot retag), so
  Story 2 acceptance 1 holds unchanged.
- **Alternatives considered**: (a) Allow `rewire_users` to skip sequential
  users while still removing the gate — rejected: leaves dangling flop
  inputs (incorrect). (b) Allow semantics-preserving re-points of flop
  inputs — rejected: violates the spec's FR-002 letter ("must not rewire")
  and complicates the gate-set-compare test oracle.

## R3. Exact truth-table pass on sequential nets

- **Decision**: `exact_simplify_pass` returns `false` immediately when any
  live sequential gate exists (same conservative shape as the existing
  hidden-output early-out at opt:141). Structural passes (fold/bypass/DCE)
  still simplify glue around barriers, so FR-008 holds via the listed
  mechanisms (exact is not among FR-008's required mechanisms).
- **Rationale**: Block truth tables are undefined for stateful nets —
  outputs depend on history, and `solve()` demonstrably fabricates them
  (`kLow` state reads). Partitioning the net into combinational regions
  between barriers and resynthesizing each was considered and rejected:
  substantial complexity for no required gain; the structural passes already
  deliver glue simplification.
- **Alternatives considered**: per-region exact with barrier pins as frozen
  boundary values — rejected as over-engineering; revisit only with a
  measured motivating case (per Constitution IV).

## R4. O2 verbatim carry-through

- **Decision**: In the O2 creation pass (opt:2554), reached live sequential
  gates are emitted verbatim in slot order: same kind, same `input_count`,
  same `ignored_mask` / `ignore_output` / `ignore_nq_output`, fresh slot via
  `add_gate` (fresh counter, deterministic), one `PendingWire` per input
  resolved through `emit_map` in the existing wiring pass (opt:2611), plus a
  per-pin `driver_inv` copy so NQ polarity survives, followed by an NQ
  fanout recount for the fresh net. Constant-label classification keeps
  treating sequential outputs as `kV` (classify unchanged); pool wiring for
  ignored dangling pins applies to sequential pins exactly as to
  combinational ones. `try_minimized_o2` keeps refusing sequential nets
  (structural path is the sequential path). Post-cleanup dedup/double-inv
  need no change (universal-kind-only); post DCE uses sequential-inclusive
  roots per R1.
- **Rationale**: Smallest change that turns O2 refusal into barrier
  semantics while reusing the existing reach/emit/wire/post machinery and
  its determinism (slot order, fresh counters).
- **Alternatives considered**: (a) Keep O2 refusal on sequential nets —
  rejected: violates FR-007 / Story 3 (optimizer must run at all levels).
  (b) Extend two-level resynthesis to stateful blocks — rejected:
  truth-table resynthesis is meaningless across state (same argument as R3).

**R4b (found during T009 test)**: O2's structural conversion duplicates
dangling pins per use (`BUF(x)` → two NANDs each dangling), so reader-pin
multiplicity and block-input counts legitimately change at O2 — the
pre-existing O2 interface contract, not a barrier violation. Consequences
pinned in tests: the fanout oracle applies to O0/O1 only (reader counts are
exact in place); O2 tests use shape + polarity + projection-aware run
parity (`proj` maps each fresh dangling pin back to its single source
signal, the same idea as the existing `check_equiv_proj`).

## R5. Ties on O2-emitted sequential pins

- **Decision**: O2 does not carry `tie_rep` classes into the fresh net
  (fresh slots invalidate flat indices); non-ignored dangling sequential
  pins stay dangling block inputs, exactly like combinational pins today.
  The existing CLI interface-change report covers any resulting interface
  difference loudly.
- **Rationale**: Matches O2's existing interface-may-change contract
  (pin duplication for inversion already changes it); remapping tie classes
  through `emit_map` would be new machinery for an edge case. O0/O1 never
  touch ties on sequential pins (freeze, R2), so the common path is exact.
- **Alternatives considered**: remap tie classes through `emit_map` —
  rejected as complexity without a motivating case; revisit on demand.

## R7. Fixpoint topology on sequential nets (found during T003 red run)

- **Finding**: `optimize()` fails loudly (`topology error`) on any net with
  flop feedback, because `ensure_topology` counts edges into sequential
  inputs as cycle edges. The closed-counter T003 test goes red at
  `r.ok()`, not at gate comparison — refusal, not deletion, is today's
  behavior on cyclic nets (acyclic nets proceed and get mangled per R0).
- **Decision**: `optimize()` dispatches recompilation: no live sequential
  gates → `ensure_topology` exactly as today (FR-009 bit-identical);
  otherwise a barrier order — `validate_sequential` first (loud on genuine
  defects, same contract as stepping), then live sequential gates in slot
  order as roots followed by `compile_comb_order`-style comb Kahn order
  (same determinism: slot-order seeds/successors). All `ensure_topology`
  calls inside `optimize()` (fixpoint loop + O2 post-cleanup on the fresh
  net) route through the dispatching wrapper. `classify`/`const_fold`
  iterate the barrier order (seq labels stay pinned `kV`; comb drivers
  precede comb users across the barrier since seq outputs are constant
  `kV`). `try_minimized_o2` keeps its own `ensure_topology` (unreachable
  for seq nets — early `false` retained).
- **Alternatives considered**: (a) slot-order passes without any topo —
  rejected: `classify`/`const_fold` need driver-before-user propagation.
  (b) duplicating the comb Kahn inside `optimize.hpp` instead of reusing
  the `seq_step` pattern — the builder mirrors `compile_comb_order`
  line-for-line (slot-order determinism); `seq_step.hpp` itself is not
  included (only light `seq_state.hpp` for `validate_sequential` /
  `has_sequential_gates`), keeping the header light per the dependency
  rule.

## R6. Test strategy (oracles, not implementation)

- Gate-set oracle: before/after compare of the sequential subset — same
  display names resolvable, same kinds, same logical drivers (source name +
  polarity), same pin counts. Deterministic hand-built nets: DFF counter
  (closed cluster, no outputs), T divider + glue with a known redundant
  `BUF`/`AND`, DLATCH + SRLATCH pipeline (acyclic — the exact-simplify
  trap), JK flop with NQ consumer, full-control DFF with async pins.
- Run-parity oracle: fixed-step run (N=10) of original vs optimized from
  the same start; stored values + settled outputs identical every step
  (uses `seq_step` semantics as the reference, FR-011).
- Regression oracle: existing `test_logic` / `test_cli_args` suites pass
  unchanged (FR-009 — combinational behavior bit-identical).
- CLI oracle: `logic optimize --level O0|O1|O2` over a sequential net exits
  0 with a well-formed net and full state-element count; before/after report
  shape unchanged.

## Open items

None. All unknowns resolved by audit; no external dependencies, no
prototyping needed. Proceed to Phase 1.

## R8. Sequential O2 toggle blowup (user report, fixed post-implementation)

- **Finding**: `optimize --level O2 --to nand|nor` toggles on AND(a,b)+DFF
  grew 3→6→9→18→33→66 gates with 3→3→5→9→17→33 block inputs, while the
  same toggles on combinational nets converge (2↔3). Cause chain: (a) each
  structural `record_input` for a dangling pin mints an independent fresh
  pin per use site, so one variable fans out to N block inputs; (b)
  `try_minimized_o2` refuses sequential nets (correctly — truth tables are
  meaningless across state), so the re-tying minimized path never runs and
  the duplication compounds every toggle; (c) `dedup_universal_pass`
  refused to merge gates with dangling inputs, so duplicate converters
  accumulated instead of collapsing through the existing double-inverter
  cancellation.
- **Decision**: two small gated changes, both no-ops for
  combinational-only nets (FR-009 — comb O2 output bit-identical,
  verified by the unchanged comb goldens): (1) structural dangling wires
  record their source pin's tie *leader* (own flat when untied) and a
  post-wiring pass ties same-leader fresh pins — one variable per pin,
  semantics-preserving by construction; (2) dedup merges gates whose
  inputs match modulo tie leaders (normalized u32 keys: wired slot+1,
  tied-leader 0x10000+leader, otherwise per-pin-unique so comb decisions
  are provably identical). Both run only when the source net has live
  sequential gates. Measured result on the reported net: inputs pinned at
  3, gates settle into a bounded 7↔6 oscillation (comb precedent: 2↔3).
- **Alternatives considered**: (a) verbatim-carry for already-in-target
  gates — dropped: useless for alternating toggles, convergence comes
  from tie+dedup+double-inv. (b) per-region minimized resynthesis with
  barrier pins frozen — rejected as over-engineering for optimality the
  O2 contract never promises ("near-optimal", "may still grow").
  (c) allowing dangling-E double-inverter cancellation — unnecessary: the
  E in collapse patterns is always a wired gate once dedup unifies the
  twins, so the existing rule already fires.
