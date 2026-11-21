# Research: Opt-In Delay-Honoring Evaluator (014)

Phase 0 output. Every open choice from the spec's Assumptions and
the plan's Technical Context is resolved here as a decision block
(Decision / Rationale / Alternatives considered). No `[NEEDS
CLARIFICATION]` item remains — each default below is grounded in the
LOGIC-V2-TIM2 task's How/Done fields, 013's pinned conventions, the
module's standing contracts, or an independent published reference
(IEEE 1364 gate-level semantics, D5).

Grounding facts verified in-tree (commit b2ee2d3): the u64 field
ships on `Gate` (types.hpp:333), 013's verbs write it, the YAML store
persists it, O2 resets it with a note; the zero-delay evaluators are
`eval.hpp::solve`/`eval_block_word` (X/Z loud at every input seam via
`resolve_input`); the stepper is `seq_step.hpp::seq_step_filtered`
(two-phase, slot-order snapshot/commit, level settle + async apply
at step top); shared-net resolution funnels through
`resolve_net` with per-attachment polarity (`driver_inv`,
`extra_inv`); `LogicRegistry` is pinned at 27 (test_cli_args.cpp:689);
the logic bench is `bench_logic`.

---

## D1 — Placement and shape: one wheel header beside the evaluators

**Decision**: a new header-only engine module,
`include/wengine/logic/timing.hpp`, holding (a) the event wheel
`run_timing_window(...)`, (b) seed helpers the CLI adapters use
(block-input diff, Q diff), (c) a transient-tolerant input resolver
and a 4-state gate evaluator. `eval.hpp`, `seq_step.hpp`,
`types.hpp`, `block_interface.hpp` gain zero code.

**Rationale**: the spec's FR-007 pins a *separate discipline*, never
a conditional inside the fast path; a sibling header is the
module's own established pattern (`eval.hpp` combinational,
`seq_step.hpp` sequential, `timing.hpp` timing). The wheel reads
`Netlist`/`SeqState` and writes neither, so no existing seam moves.

**Alternatives considered**: a mode flag threaded through `solve()`
itself — rejected: it puts delay reads and branches on the measured
default path, violating the spec's "default evaluation path reads no
delay field and branches on no mode" and constitution IV's spirit. A
general discrete-event framework (generic queues, cancelable
events, process semantics) — rejected: no concrete need (V), and
generic event cancellation is exactly the inertial behavior the
spec forbids.

## D2 — Time axis and the unit

**Decision**: the wheel's time is `std::uint64_t` whole picoseconds
(the annotation field's storage unit). Every gate's transport delay is
its stored annotation; an un-annotated gate (field 0, per 013's
zero-is-un-annotated convention) takes the mode's unit delay of
**1 ps** — the field's smallest storable nonzero value. All time
arithmetic is integer with checked adds; overflow refuses loudly.

**Rationale**: the task's own Done field pins "unit-delay event
sim"; 1 ps is the only unit that is both grounded (the storage
granularity) and expressible without a new stored value. Keeping the
field untouched preserves 013's "no separate annotated-zero concept"
convention verbatim.

**Alternatives considered**: abstract ticks (1 tick = one gate hop)
— rejected: annotated picosecond values could not share the axis
without inventing a tick↔ps rate, forking the annotation's meaning
per mode. A nominal default stored value (auto-writing 1 ps to
un-annotated gates) — rejected: it would mutate user workspaces and
reinterpret 013's zero convention. Floating-point time — rejected:
constitution I/II exactness; integer ps matches the field exactly.

## D3 — Well-foundedness: the unit floor is the no-oscillation proof

**Decision**: the unit floor is a *semantic rule*, not just a
default: no event may land at the same picosecond that caused it.
Every cause precedes its effect by ≥ 1 ps, so within one time
bucket no event can trigger another at the same bucket time —
buckets never self-interact, and the wheel always advances.
Intra-bucket ordering therefore affects only the trace's listing
order (pinned slot order, D4/D8), never any value.

**Rationale**: this is the determinism argument for the whole mode:
it makes "same-instant events resolve in a pinned total order" true
by construction, makes the drain guarantee provable (a strict
time progress over a finite event budget must terminate), and
mirrors the module's flop-aware no-cycle guarantee on the timing
side.

**Alternatives considered**: allowing 0 ps annotated gates to fire
within the originating instant — rejected: it would reintroduce
level-order evaluation inside the wheel (two value systems in one
mode), require an intra-instant ordering convention beyond listing,
and undermine the termination argument; the spec's edge-case list
already pins "no gate can be faster than unit".

## D4 — Transport semantics and the collapse rule

**Decision**: pure transport — every input-net transition schedules
a recomputation of each consuming gate at (event time + gate delay);
no event is ever canceled or filtered. Duplicate schedules for the
same (gate, time) pair collapse into a single evaluation that reads
input nets as of that time. An evaluation that produces the gate's
already-current output emits no event (no-op evaluations are silent;
the trace lists transitions only).

**Rationale**: the spec's FR-003 pins observation over realism
("a pulse the physics would swallow still shows"). The collapse
rule is required for determinism (two same-time arrivals must not
double-fire) and keeps goldens hand-derivable: each event = one
(net, time) transition.

**Alternatives considered**: inertial delay (pending events
canceled when a closer input event arrives) — rejected: it hides
narrow pulses, the mode's entire subject. Per-event double-firing
without collapse — rejected: nondeterministic trace content and
unbounded event duplication at reconvergent same-time paths.

## D5 — 4-state gate semantics during transients

**Decision**: while the wheel runs, gates evaluate over all four
`LogicValue`s with dominant-value tables anchored to IEEE 1364
gate-level semantics: AND/NAND output 0 if any input reads 0,
else X if any input is unknown, else 1 (NAND inverts; OR/NOR dual:
any 1 → 1); NOT/BUF pass X through (and read Z as X); XOR/XNOR
yield X if any input is unknown (parity has no dominant value);
TBUF drives Z only when released, its data pass-through treats
unknowns as X. A Z arriving at any gate *input* reads as X for
evaluation (Z is a net state, not a gate operand — consistent with
the shipped funnel where Z is transparent and only consumers give
it meaning). Settled-state refusals are unchanged: the adapters run
the ordinary zero-delay validation for the settled tail, so a
settled X/Z at a level consumer or a settled S=R=1 net refuses
exactly as today; the transient window only ever *shows* X.

**Rationale**: the spec pins "transients propagate X visibly, never
silently coerced" — dominant-value is the strictly more informative
reading (AND(0, X) = 0 is *correct*, not coercion; a conservative
X would fabricate false-positive pulses, the worst failure for an
observation tool). IEEE 1364's gate tables are the independent
published reference the tests derive against (constitution I:
trusted reference, not implementation readback).

**Alternatives considered**: conservative any-unknown → X —
rejected: false-positive hazard pulses on nets a dominant value
provably stabilizes. Full 9/12-state strength semantics — rejected:
the module's value system is four-state, pinned by VAL1–VAL3;
inventing states is out of scope. Refusing any transient X —
rejected: it would make the hazard demo itself illegal (its whole
point is showing the window).

## D6 — Adapters, seeds, anchors, and the parity construction

**Decision**: one wheel, three thin adapters; state and validation
stay with the unchanged machinery:

- **solve `--with-delays`**: pre-state = the ordinary zero-delay
  solve of the all-zero vector (the same default `solve` already
  echoes loudly); seeds = block-input nets whose applied value
  differs from that pre-state, landing at t=0. Run the wheel;
  print the trace block; then print the *unchanged* settled tail by
  running the ordinary `solve()` on the applied assignment.
- **step `--with-delays`**: the unchanged step sequence runs first
  (async apply, level settle, then the edge loop of
  `seq_step_filtered`). Per edge: snapshot Q before the call, diff
  after — each differing (flop, Q) is a seed at t=0 (its NQ
  transition shares the timestamp, per the spec's FR-006); one wheel
  window per edge, separated by `edge k:` lines when `--cycles N`
  is used. Settled tail = the loop's existing outputs.
- **truth `--with-delays`**: per row, seeds = bits differing from
  the all-zero vector (same row convention as solve); the row's
  drain time is the window's last event time. Values are the
  unchanged word-eval rows; only the settle column is new.

Parity is a *test* duty, not engine logic: the wheel's end-state is
asserted equal to the zero-delay result (solve) and the stepped
trajectory is asserted equal across modes (step) — drift is caught
by the suite, and the adapters report the settled values from the
unchanged path, so the two disciplines can never disagree in user
output even if the wheel had a bug (they would fail loudly in
tests, not silently in production).

**Rationale**: the spec's FR-005/FR-006 pin endpoint/trajectory
parity; anchoring parity on the unchanged path makes it true by
construction for everything the user sees, with the wheel owning
only the trace. Per-window anchors (each invocation/edge reports
its own t=0) match the spec's window-anchor entity and dodge the
"what ps is a clock cycle" hole the module never defined.

**Alternatives considered**: letting the wheel own validation and
outputs (refusing X/Z at drain itself) — rejected: it would
duplicate the settled-refusal logic (drift hazard) and risk the
settled tail drifting from default mode. Cross-invocation absolute
time accumulation — rejected: the module has no cycle-period
concept; inventing one is a new feature, not this one. Seeding step
windows from *poke* transitions — rejected: pokes are authoring
(the spec's assumption); the window's subject is what the *edge*
caused.

## D7 — Flag surface and registry posture

**Decision**: a boolean `--with-delays` flag on the three existing
evaluation verbs (`solve`, `step`, `truth`), parsed with the
existing `take_flag`; per-invocation, never sticky, never
serialized. Completion gains the flag in each verb's option list.
`LogicRegistry` stays at 27 — no new verbs, no registry-size
changes; `test_cli_args.cpp` gains only completion vectors.
Interactive/one-shot/TUI parity rides the standing byte-identity
construction (the TUI composes the same lines; no logic form
exists). Refusals follow the module's conventions: unknown flag
spellings hit the existing stray-argument error path; the wheel's
budget/overflow failures are runtime failures (exit 1) with the
workspace untouched.

**Rationale**: the task's How pins "solve/truth gain flag" (and
races require step); the spec's FR-001/FR-013 pin one vocabulary
inside the existing domain. 013's precedent (usage strings updated,
registry grown only for new verbs) governs: flags change no
registry math.

**Alternatives considered**: a new `trace` verb — rejected: forking
the evaluation vocabulary (FR-013 anchors the flag); a sub-shell
mode or session-sticky timing toggle — rejected: sticky state
would violate the per-invocation contract and complicate
determinism reasoning.

## D8 — Trace grammar (byte-stable, greppable)

**Decision**: the timing block printed by the shared helper is:

```text
timing: unit 1ps, <K> annotated
  t=<T>ps <PIN>: <old>-><new>
  ...
settled in <D>ps
```

where `<PIN>` is the module's existing display-name pin spelling
(`AND2.Y`, `DFF1.Q`, `DFF1.NQ`, block-input pins in `solve --from`
spelling), values spell `0|1|X|Z`, events list in pinned order
(time, then slot order; NQ immediately after its Q at the same
timestamp), and `<D>` is the drain time (0 ps for an empty window).
The block is inserted before the invocation's settled tail, and
the tail bytes are exactly the flag-off output. `step --cycles N`
prints one block per edge prefixed `edge <k>:`; the n=0 no-op
report is unchanged (no edges, no window). `truth --with-delays`
appends a `settle` column to each pretty row and a trailing
`settle_ps` column to `--csv` rows; flag-off truth output is
byte-identical.

**Rationale**: byte-identity contracts need an exact grammar;
reusing the step report's `a->b` transition spelling keeps one
vocabulary; the header line names the unit floor so the spec's
"mode's output names the unit" acceptance is satisfied by
construction.

**Alternatives considered**: CSV-only traces — rejected: the
terminal is the module's observation surface (CSV export is the
named follow-on in the spec's assumptions). ISO time units or
engineering suffixes in stamps — rejected: `ps` matches the
annotation grammar; no conversion tables anywhere (013 D2
discipline).

## D9 — Budget and overflow: loud caps, never hangs

**Decision**: the wheel refuses loudly (`core::Status` runtime
failure, workspace untouched) when (a) trace entries exceed a
constant event cap `kMaxTimingEvents`, or (b) a time add overflows
u64. The refusal names the cap/overflow and the offending
netlist shape. The cap is a *real* guard, not paranoia: transport
propagation through reconvergent fan-out can multiply (gate ×
arrival-time) pairs far beyond gate count (the classic
transport-sim worst case), and annotations make path sums distinct
on purpose.

**Rationale**: the spec's edge-case list pins "a budget breach is a
loud runtime failure, never a hang" and the module's loud-cap
convention (shared-net attachment caps, truth row caps) governs.
D3's strict time progress guarantees termination *given* a finite
budget; the budget makes the guarantee concrete and testable (a
depth-~20 reconvergent XOR net with staggered annotations drives
the refusal in test).

**Alternatives considered**: no cap (trusting wire-time cycle
rejection to bound events) — rejected: cycles are rejected, but
event *multiplication* through reconvergent acyclic nets is real
and can outrun any linear bound. Silent truncation of the trace
with a footnote — rejected: silent drops are forbidden module-wide
(AGENTS.md §8 drop vocabulary; the trace is the product, not an
overflowable channel). A netlist-size-scaled cap — rejected: a
constant is easier to document, test, and refuse against; scaling
would still need a loud ceiling.

## D10 — Fast-path protection (constitution IV)

**Decision**: no claim is made for the wheel's speed; it is a cold
per-invocation reporting path with fixed stack-sized scratch. The
measured duty is negative: `eval.hpp`, `seq_step.hpp`, and the word
evaluator gain zero code, and the unchanged `bench_logic` run in
the suite is the no-regression proof. Default byte-identity is
pinned by the full pre-feature golden suite plus new flag-off
comparisons.

**Rationale**: exactly 013's D9 posture — constitution IV is
satisfied by the absence of a claim plus the pinned unchanged
measurement, never by asserting a new path is "probably fine".

**Alternatives considered**: adding a timing benchmark now —
rejected: no consumer asked for wheel speed; a benchmark without a
measured need is the speculative work the constitution forbids
(recorded as optional planning scope in the spec's assumptions).

## D11 — Verification strategy (constitution III)

**Decision**: `tests/test_logic_timing.cpp` verifies at the
module's boundaries, every golden independently derived:

- hand arithmetic: pulse width = path-delay difference (3-inverter
  chain → 3 ps window, placement = gate delays); drain times =
  longest activated annotated path; race offsets = clock-to-Q +
  downstream path sums; 4-state cases derived from the 1364 tables
  (AND(0,X)=0 — no pulse; AND(1,X)=X — pulse; TBUF released window);
- refusals: event cap (reconvergent annotated bomb), time overflow
  (annotation near the u64 cap on a long chain), settled X/Z,
  settled S=R=1 — all with state/output untouched;
- parity: wheel end-state == zero-delay solve; step trajectories
  equal across modes for the same sequence; repeated invocations
  byte-identical;
- CLI representation: `--with-delays` blocks against the D8
  grammar; flag-off outputs byte-identical;
- examples: `glitch_chain.yaml`/`race_window.yaml` replay their
  header traces exactly (the load path already round-trips
  annotations per 013).

Registry/completion pins in `test_cli_args.cpp` (vectors only,
size-27 pin untouched); catalog audit stays zero-missing/zero-ghost.

**Rationale**: the spec's FR-014 and SC-001..004 demand exactly
these levels; the module's three-level norm (reference → engine →
CLI → artifact → re-read) is preserved via the examples.

**Alternatives considered**: property-style randomized trace
fuzzing — rejected for this change: every trace property reduces to
the parity construction already pinned deterministically; fuzzing
would add nondeterministic test load without a named hazard.

## D12 — Named out-of-scope (recorded once, honored everywhere)

**Decision**: the following are explicitly out of this feature and
remain named follow-ons: engine `Runner`/`sim run --solver logic`
timing integration (snapshot/checkpoint untouched); machine-
readable trace export (CSV/other); setup/hold *verdicts* (the mode
reports arrivals; judging budgets is the user's call, FR-006);
per-pin or rise/fall delay splits (the field is per-gate, uniform
across its inputs); inertial-delay mode; GUI surfaces; clock-path
delays (the edge is the call in this module's clock model — the
window observes what the edge caused).

**Rationale**: each is either a different consumer of the same trace
(Runner, exporters, GUI), a different discipline (inertial, STA
verdicts), or a storage change the spec pinned out. Recording them
prevents scope creep during tasks/implementation.

**Alternatives considered**: folding the Runner path in now —
rejected: `SequentialRunConfig`/`Snapshot` growth would violate the
spec's "engine gains nothing" assumption for zero named consumers.
