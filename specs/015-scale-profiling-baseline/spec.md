# Feature Specification: Measured Scale-Profiling Baseline for One-Million-Gate Logic Nets

**Feature Branch**: `015-scale-profiling-baseline`

**Created**: 2026-11-21

**Status**: Draft

**Input**: User description: "What: Measure current bottlenecks on growing nets before optimizing (per AGENTS.md §5). Why: No SIMD/threading/GPU work is justified without a measured hotspot; baseline pins the O(N^2) scans to kill."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - A million-gate net exists on the measured path (Priority: P1)

Today the logic domain tops out at a fixed pool of 1024 gate slots: a net
of one million gates cannot be built, compiled, or evaluated anywhere in
the project. After this feature, the measured path constructs, wires,
compiles, and evaluates nets of at least 1,000,000 live gates — layered
combinational growth, shared (multi-driver) nets, and sequential
elements mixed in — under every standing domain contract: the
single-driver rule, flop-aware cycle rejection, the four-value funnel,
never-reused per-kind counters, and loud overflow. Correctness at scale
is pinned by the domain's own two evaluation disciplines agreeing
bit-exactly on the same slice — the scalar level-order evaluation and
the bit-parallel word evaluation — never by the scale path grading its
own homework; repeated invocations are bit-identical; a small prefix of
the giant net evaluated standalone matches the same slice's values
inside the giant net, so growth changes size and nothing else.

**Why this priority**: The task title's target is unreachable without
it — nothing at one million gates is measurable until a one-million-gate
net can exist. Every other story measures this one's substrate.

**Independent Test**: Can be fully tested by building the million-gate
net on the measured path, running one scalar solve, one word-evaluated
slice, and one sequential step, and asserting cross-discipline parity,
bit-identical repeats, and prefix-slice agreement — delivering
million-gate reach with no sweep, report, or attribution story
involved.

**Acceptance Scenarios**:

1. **Given** the measured path, **When** a net of at least 1,000,000
   live gates is built, wired, compiled, and evaluated, **Then** every
   stage completes within the session and the scalar and word-parallel
   results agree bit-exactly on the evaluated slice.
2. **Given** the same million-gate net and the same inputs, **When**
   evaluation and stepping repeat, **Then** outputs are bit-identical —
   the domain's determinism contract holds at scale.
3. **Given** enabled capacity exceeded, **When** more gates are added,
   **Then** the addition refuses loudly under the domain's standing
   overflow convention — never silent, never truncated.
4. **Given** a mixed net at scale (shared nets, pull terminations,
   sequential elements, delay annotations), **When** solved and stepped,
   **Then** the 011–014 contracts — funnel resolution, pull-terminated
   idle, drained-before-edge sampling — hold exactly as at the current
   scale.
5. **Given** a single-driver violation or combinational cycle attempted
   at scale, **When** wired, **Then** the refusal is loud and follows
   the standing conventions — scale adds no new vocabulary and relaxes
   no rule.
6. **Given** a small prefix of the million-gate net, **When** evaluated
   standalone at the current scale, **Then** its outputs match the same
   slice's values inside the full net — growth alters size, never
   semantics.

---

### User Story 2 - The recorded growth baseline (Priority: P1)

A developer runs one shipped sweep entry and gets the growth baseline:
for a ladder of net sizes from the current cap up through at least one
million gates (spanning at least three orders of magnitude), the cost
of every pipeline stage measured separately — incremental growth
(gate adds and wire bursts), the per-mutation topology compile, the
steady-state scalar solve, the bit-parallel word evaluation, the
shared-net scalar path, sequential stepping, and the optimizer passes —
with warmup and first-compile costs separated from steady state, with
inputs opaque to the compiler, with outputs checksummed, and with the
peak memory footprint reported per size. The numbers are committed as
the documented baseline in the project's bench-documentation suite, the
environment pinned (compiler, build configuration, host) and the
repetition and variance policy stated; re-running the sweep reproduces
the committed curves within that stated variance. A stage whose cost
blows a stated per-size time budget appears as a loud
unmeasured-at-this-size entry — the report never extrapolates, never
estimates, and never silently skips.

**Why this priority**: This measurement is the feature's entire
deliverable and the task's stated purpose: the project's
measured-performance rule (AGENTS.md §5) makes this baseline the
prerequisite for every later optimization, and "before optimizing" is
the task's own ordering.

**Independent Test**: Can be fully tested by running the shipped sweep
entry on the pinned environment and comparing its per-stage numbers
against the committed baseline within the stated variance — delivering
the recorded baseline with no attribution, gate, or reachability story
beyond story 1's substrate.

**Acceptance Scenarios**:

1. **Given** the shipped sweep entry, **When** run over the full size
   ladder, **Then** per-stage timings and footprints exist for every
   size where the stage fits its stated budget.
2. **Given** the per-stage numbers across size doublings, **When** the
   report classifies each stage's scaling, **Then** every stage carries
   a measured complexity verdict derived from its own ratios — the
   report's classifications follow its numbers, never the other way.
3. **Given** the committed baseline and the same pinned environment,
   **When** the sweep re-runs, **Then** the curves reproduce within the
   stated variance.
4. **Given** a stage too costly to finish at a size within its budget,
   **When** the sweep reaches that size, **Then** the report records it
   as unmeasured-at-this-size loudly, and the report contains zero
   extrapolated or estimated numbers.
5. **Given** the sweep's net construction, **When** the same arguments
   re-run, **Then** the constructed nets and checksums are bit-identical
   — timings vary, shapes never do.
6. **Given** any timed stage, **When** measured, **Then** its inputs
   stay opaque (seeded from runtime arguments, outputs checksummed), so
   no measured cost is constant-folded away.

---

### User Story 3 - Named attribution and the measured-hotspot gate (Priority: P2)

The baseline does not stop at curves: every super-linear suspect the
task names gets a measured verdict. The standing suspects, read off the
domain's current algorithms — the full-pool successor sweep inside
every topology-compile pop, the free-slot scan on every gate add, the
full-pool sweep on every gate removal, the per-wire reachability probe,
the all-pairs structural-dedup scan, and the full-capacity
initialization paid on every solve — each receives either a confirmed
verdict (its measured size-doubling ratios show super-linear cost) or
a cleared verdict (measured linear within stated variance). The
committed report ranks what to kill first at the one-million-gate end,
split mutation-side, compile-side, and evaluation-side, so later work
targets scans by measured cost and never by suspicion. And the report
makes the governance gate operational: from this baseline on, no SIMD,
threading, GPU, or structural-rework proposal for the logic domain
proceeds unless it cites the baseline number of the hotspot it attacks;
a proposal that cannot name its measured hotspot is out of order.

**Why this priority**: This is the task's Why made enforceable — "no
SIMD/threading/GPU work is justified without a measured hotspot" only
bites when a committed artifact exists to cite. It rides on story 2's
numbers; a user whose sweep runs already has the feature's core value.

**Independent Test**: Can be fully tested by reading the committed
report and asserting each named suspect carries a verdict grounded in
its own measured ratios, the ranking follows those numbers, and the
gate's citation requirement is stated so a hotspot-less proposal is
rejectable by citation alone.

**Acceptance Scenarios**:

1. **Given** the committed report, **When** a reader looks up any named
   suspect, **Then** it carries a measured verdict — confirmed with its
   size-doubling ratios, or cleared within stated variance — and no
   verdict is presumed guilty by suspicion.
2. **Given** the one-million-gate end of the sweep, **When** the report
   ranks costs, **Then** the top costs appear in ranked order split
   mutation-side, compile-side, and evaluation-side.
3. **Given** the report's gate section, **When** a later optimization
   proposal arrives, **Then** it is acceptable or rejectable by citing
   the baseline alone — the proposal must name the measured hotspot it
   attacks.
4. **Given** a verdict, **When** traced, **Then** it derives from the
   report's own committed numbers, never from code reading or
   extrapolation.

---

### User Story 4 - Nothing that ships today moves (Priority: P2)

The pre-feature suite is the regression pin. Every existing golden,
example replay, refusal message, benchmark measurement, and save/load
round-trip at the current scale passes unchanged after the feature.
Wherever the enablement substrate must touch mechanics shared with the
current scale — pool sizing, scratch ownership, slot width — the change
is the smallest one that reaches the target, is justified and re-measured
under the project's standing measured-performance process, and existing
bench measurements move not at all beyond stated noise. User-facing
capacity policy follows one bounding rule: wherever a loud cap remains,
exceeding it fails loudly; wherever capacity extends to a user-facing
verb, the extension ships with its documentation duties (run examples,
module table, test list) in the same change, and the workspace schema
changes only if enablement strictly requires it — governed then by the
domain's standing loud-refusal and version-bump policy.

**Why this priority**: The domain's standing contract is that existing
documented behavior is preserved unless the task explicitly changes it.
This feature measures; it must not quietly mutate what it measures, or
the baseline's before/after meaning is destroyed at the current scale.

**Independent Test**: Can be fully tested by running every pre-feature
golden, example replay, refusal class, bench entry, and save/load
round-trip and asserting byte-identical outputs and unchanged
measurements — no scale story required.

**Acceptance Scenarios**:

1. **Given** any pre-feature flow at the current scale, **When** it
   runs after this feature, **Then** its output is byte-identical to
   before the feature.
2. **Given** the domain's existing bench suite, **When** re-run after
   the feature, **Then** measurements are unchanged within stated
   noise — the current scale paid nothing measurable for the
   enablement, or the paid cost is named and justified per the standing
   process.
3. **Given** current-scale workspaces, **When** saved and reloaded
   after the feature, **Then** the round-trip is identical; if
   enablement strictly requires a schema change, the version bump and
   its refusal policy ship in the same change, never silently.
4. **Given** any user-visible capacity extension on a verb, **When** it
   lands, **Then** the run examples, module table, and test list update
   in the same change.
5. **Given** any cap that remains, **When** exceeded, **Then** the
   refusal is loud — never a silently smaller net.

---

### User Story 5 - The harness outlives the baseline (Priority: P3)

The sweep is not a one-shot artifact: it stays the standing
before/after instrument for the optimization follow-ons it exists to
gate. Killing a pinned scan means re-running the sweep and showing the
targeted stage's curve changed while no other stage regressed beyond
noise; both runs are cited in the follow-on's evidence. The committed
report documents the re-baselining procedure — the entry invocation,
the environment pinning, the repetition and variance policy — so any
follow-on task reproduces the comparison without re-deriving
methodology, and the report's own reproduction section demonstrates
once that a no-change re-run lands within the stated variance.

**Why this priority**: This is the baseline's forward value — the
follow-on tasks in the scale series consume it — but the feature
delivers its full contract with stories 1–4 even if no follow-on
exists yet, hence P3.

**Independent Test**: Can be fully tested by re-running the sweep with
no intervening code change and asserting the curves land within the
committed baseline's stated variance, using only the procedure
documented in the report.

**Acceptance Scenarios**:

1. **Given** the committed report, **When** a follow-on needs a
   before/after comparison, **Then** the re-run procedure is fully
   documented (entry invocation, environment, variance policy) and
   sufficient to reproduce without methodology re-derivation.
2. **Given** a no-change re-run, **When** compared against the
   committed baseline, **Then** every stage's curve lands within the
   stated variance.
3. **Given** a follow-on that kills a pinned scan, **When** it cites
   its evidence, **Then** the acceptance shape is available verbatim:
   the targeted stage's curve changed, no other stage regressed beyond
   noise.

---

### Edge Cases

- What happens when **capacity is exceeded on the measured path**? →
  loud refusal under the standing overflow convention — never a
  silently smaller net, never truncation.
- What happens when **a stage is too costly to finish at a size within
  its stated budget** (a confirmed quadratic at one million gates can
  outlast any reasonable run)? → the report records the stage as
  unmeasured-at-this-size, loudly; the report never extrapolates a
  number to fill the gap, and the loud gap is itself baseline evidence
  of the bottleneck.
- What happens when **memory footprint at one million gates approaches
  the host's budget**? → the sweep reports peak footprint per size, and
  a stated budget breach fails loudly with the measured peak in the
  message — never a silent thrash.
- What happens when **growth shape changes which scan hurts** (deep
  chains vs. wide layers vs. dense fan-in)? → the sweep covers both
  standing growth motifs and the mixed-feature motif (shared nets,
  pulls, sequential elements, delay annotations), because a
  single-shape baseline would pin the wrong scan.
- What happens when **the compiler could constant-fold the timed
  region**? → construction stays opaque (runtime-seeded, checksummed)
  per the standing bench rules; a folded-away stage is a defect in the
  harness, not a fast measurement.
- What happens when **first-compile and steady-state costs differ**
  (mutation churn dirties the compile; steady solves do not)? → they
  are measured as separate stages and never averaged together.
- What happens when **timings vary run to run**? → the report states
  its repetition and variance policy up front and applies it
  consistently; reproduction claims are always relative to that
  stated variance, never to folklore precision.
- What happens on **a different host or compiler than the pinned
  environment**? → the committed numbers are the pinned environment's;
  the other host re-runs the shipped sweep and records its own numbers
  beside them, never edits the baseline in place.
- What happens when **a suspect turns out innocent**? → it is cleared
  with its measured ratios and stays cleared until a future change
  re-suspects it; the baseline never pre-convicts by code reading.
- What happens when **the enablement substrate must touch serialization
  bounds**? → the workspace keeps its standing loud-refusal policy;
  any version change ships its bump and refusal text in the same
  change, with current-scale round-trips pinned byte-identical.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The logic domain MUST be able to construct, wire,
  compile, and evaluate nets of at least 1,000,000 live gates on the
  measured path — combinational layers, shared nets, and sequential
  elements mixed in — with every standing domain contract intact
  (single-driver rule, flop-aware cycle rejection, four-value funnel,
  never-reused per-kind counters) and loud refusal on any capacity
  breach.
- **FR-002**: The measurement MUST exercise the domain's real
  algorithms: the harness MUST drive the same netlist, topology,
  evaluation, stepping, and optimizer code the domain ships — a shadow
  or forked implementation measured in place of the shipping one is a
  defect, not a shortcut.
- **FR-003**: The harness MUST sweep a ladder of growing net sizes
  spanning at least three orders of magnitude up to at least one
  million gates, MUST attribute costs per pipeline stage (incremental
  growth, per-mutation topology compile, steady-state scalar solve,
  bit-parallel word evaluation, shared-net scalar path, sequential
  stepping, optimizer passes), MUST separate warmup and first-compile
  costs from steady state, MUST keep inputs opaque and outputs
  checksummed, and MUST report peak memory footprint per size.
- **FR-004**: The baseline MUST be committed to the project's bench
  documentation suite with per-stage, per-size numbers, the environment
  pinned (compiler, build configuration, host), and a stated
  repetition and variance policy; re-running the shipped sweep MUST
  reproduce the committed numbers within that stated variance.
- **FR-005**: Every named super-linear suspect (the per-pop full-pool
  successor sweep in the topology compile, the per-add free-slot scan,
  the per-remove full-pool sweep, the per-wire reachability probe, the
  all-pairs structural-dedup scan, the full-capacity initialization on
  every solve) MUST receive a measured verdict — confirmed with
  size-doubling ratios, or cleared within stated variance — and the
  report MUST rank the top costs at the one-million-gate end, split
  mutation-side, compile-side, and evaluation-side.
- **FR-006**: A stage exceeding its stated per-size budget MUST be
  reported as unmeasured-at-this-size loudly; the committed baseline
  MUST contain no extrapolated, estimated, or projected numbers.
- **FR-007**: The measured-hotspot gate MUST be stated operationally in
  the committed report: subsequent optimization work on the logic
  domain (vectorization, threading, GPU offload, structural rework)
  MUST cite the baseline number of the hotspot it attacks and MUST
  re-run the sweep as its before/after evidence; a proposal naming no
  measured hotspot is out of order until re-baselined.
- **FR-008**: This feature MUST NOT itself optimize: no
  vectorization, no threading, no GPU work, and no algorithmic
  complexity rework beyond what the enablement substrate strictly
  requires to reach the capacity target; any unavoidable change to
  mechanics shared with the current scale MUST be the smallest
  coherent one, justified and re-measured under the standing
  measured-performance process.
- **FR-009**: Pre-feature behavior MUST be preserved: existing goldens,
  example replays, refusal messages, bench measurements (within stated
  noise), and workspace save/load round-trips at the current scale
  MUST pass unchanged; remaining caps stay loud wherever enablement
  does not extend them; any user-visible capacity extension MUST ship
  its run examples, module-table entry, and test-list updates in the
  same change.
- **FR-010**: Correctness at scale MUST be pinned independently of the
  scale path's own readback: the domain's two evaluation disciplines
  (scalar level-order and bit-parallel word) MUST agree bit-exactly on
  the same slice at sweep sizes, repeated invocations MUST be
  bit-identical, and a small prefix evaluated standalone MUST match the
  same slice inside the full net at every size.
- **FR-011**: The harness MUST remain usable as the standing
  before/after instrument: the committed report MUST document the
  re-run procedure (entry invocation, environment pinning, variance
  policy) sufficient for follow-on tasks to reproduce comparisons
  without re-deriving methodology.

### Key Entities *(include if feature involves data)*

- **Scale sweep harness**: the one shipped growth-measurement entry —
  builds growing nets per motif, times each pipeline stage separately,
  reports per-size costs and footprints, and regenerates the committed
  baseline on demand.
- **Per-stage cost curve**: one pipeline stage's measured cost across
  the size ladder; the raw material of every verdict and ranking.
- **Measured complexity verdict**: a stage's or suspect's classification
  (linear, super-linear with ratios, unmeasured-at-size within budget)
  derived from its own committed numbers — confirmed, cleared, or
  loudly absent, never presumed.
- **Committed baseline report**: the documentation-suite artifact:
  per-stage, per-size numbers, environment pinning, variance policy,
  suspect verdicts, the one-million-gate ranking, the operational
  gate, and the re-run procedure.
- **Capacity enablement**: the substrate change letting the measured
  path hold at least one million gates under the standing contracts —
  bounded by honest measurement (no shadow implementation), loud
  overflow, and byte-identical current-scale behavior.
- **Measured-hotspot gate**: the project's measured-performance rule
  (AGENTS.md §5) made operational for the logic domain — optimization
  proposals must cite a baseline number to proceed.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Million-gate reach — in one session on the measured
  path, a net of at least 1,000,000 live gates is built, wired,
  compiled, scalar-solved, word-evaluated, and stepped, with
  scalar/word bit-exact parity, bit-identical repeats, and
  prefix-slice agreement.
- **SC-002**: Baseline committed and reproducible — per-stage cost
  curves and footprints spanning at least three orders of magnitude
  up to at least one million gates exist in the bench documentation
  suite; a re-run on the pinned environment lands within the report's
  stated variance, demonstrated once in the report's own reproduction
  section.
- **SC-003**: Attribution complete — every named suspect carries a
  measured verdict grounded in committed size-doubling ratios, the
  one-million-gate costs are ranked mutation-side, compile-side, and
  evaluation-side, and the report contains zero extrapolated numbers.
- **SC-004**: The gate holds — any subsequent vectorization, threading,
  GPU, or structural-rework proposal for the logic domain is acceptable
  or rejectable by citing the committed baseline alone; and everything
  that shipped before this feature — goldens, replays, refusals,
  benches, round-trips — passes unchanged at the current scale.

## Assumptions

- "One million gates" means at least 1,000,000 live gates on the
  measured path; the current fixed pool (1024 slots, narrow slot
  identifiers, fixed-size by-value storage, fixed caller scratch) is
  the substrate that must widen. How it widens — an engine-wide
  capacity lift versus a capacity-parameterized form of the same
  production code — is a planning decision; this spec binds only the
  constraints: honest measurement of the shipping algorithms (FR-002),
  loud overflow, standing contracts, and byte-identical behavior at
  the current scale.
- User-facing capacity policy (whether the interactive verbs accept
  beyond-current-cap nets immediately or keep a loud documented cap
  until the optimization follow-ons land) is planning scope; the
  default is whichever the enablement makes safe, and any user-visible
  extension carries its documentation duties in the same change
  (FR-009).
- The stage set for the sweep is the domain's real pipeline: growth,
  topology compile, scalar solve, word evaluation, shared-net path,
  sequential stepping, optimizer passes. Serialization (save/load)
  scale profiling is optional planning scope — a named follow-on if
  the baseline ever shows I/O costs matter to the growth story.
- The named suspects come from the current algorithms as read today;
  they are candidates to confirm or clear, never to pre-convict — a
  suspect's innocence is a legitimate and recorded outcome (FR-005,
  edge case on cleared suspects).
- Growth motifs follow the existing bench precedent (opaque layered
  construction) plus the mixed-feature motif so the 011–014 features
  stay measurable at scale; motif shapes are planning decisions
  required only to be opaque, deterministic, and documented in the
  report.
- The sweep's environment is the project's bench configuration on the
  baseline host and compiler; benchmarks never link the assertion
  policy; the repetition and variance policy is stated in the report
  and applied consistently.
- The committed numbers belong to the pinned environment; other hosts
  re-run the shipped sweep and record beside, never edit in place
  (edge case on foreign environments).
- Vectorization, threading, and GPU work are explicitly out of scope
  here — they are the named follow-ons this baseline gates (FR-007,
  FR-008).
- No interactive surface is required to reach the target: the
  deliverable is headless measurement. GUI/TUI panels gain nothing in
  this feature, and the standing byte-parity rules cover any surface
  that later consumes the capacity.
- The follow-on optimization tasks (the "kill list") exist only after
  this baseline ranks the scans to kill; their scope, order, and
  per-scan acceptance criteria come from the committed report's
  ranking, not from this spec.
