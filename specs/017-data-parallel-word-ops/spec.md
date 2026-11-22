# Feature Specification: Data-Parallel Word Ops for Bit-Parallel Evaluator

**Feature Branch**: `017-data-parallel-word-ops`

**Created**: 2026-11-22

**Status**: Draft

**Input**: User description: "What: Data-parallel word ops for the bit-parallel evaluator with portable fallback. Why: Word eval is the measured hot path; SIMD multiplies its 64-wide parallelism without changing semantics."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Truth evaluation runs faster with identical tables (Priority: P1)

A developer evaluates a combinational net's truth table through the
bit-parallel word evaluator (the `truth` verb, the word benchmark, and
every internal caller that loops over 64-row slices). Today each slice
is evaluated one 64-bit word at a time: one gate produces one word per
pass through the level order. After this feature, the evaluator
processes several independent 64-row slices together in each pass
(data-parallel across slices), so one traversal of the level order
produces several slices' worth of outputs instead of one. Every word
the evaluator emits is bit-identical to today's scalar-word path on
the same slice: same rows, same lane convention, same outputs, same
masking of partial words.

**Why this priority**: The task's Why names this path the measured hot
spot, and the 015 baseline exists precisely to gate this work: word
eval multiplies 64-wide parallelism today, and data-parallel
evaluation multiplies it again with no semantic change. Everything
else in this spec is a constraint on this story.

**Independent Test**: Can be fully tested by evaluating a corpus of
nets (all combinational gate kinds, mixed fan-in, tied inputs, ignored
dangling inputs, partial-word input counts, large slice counts) slice
by slice before and after the change and asserting every emitted word
matches bit-exactly — plus one benchmark run showing reduced
per-gate-word cost. Delivers value alone: faster truth at scale.

**Acceptance Scenarios**:

1. **Given** any net from the generated corpus evaluated over all of
   its slices, **When** each slice's output words are compared before
   and after the change, **Then** every word is bit-identical.
2. **Given** the committed word-evaluation benchmark on the pinned
   environment, **When** re-measured after the change, **Then**
   per-gate-word cost is lower than the committed baseline by a
   stated, committed factor, with before/after numbers recorded.
3. **Given** a net with fewer than 6 block inputs (partial final word),
   **When** evaluated, **Then** don't-care lanes above the row count
   are zero exactly as before.
4. **Given** repeated evaluation of the same slices, **When** re-run,
   **Then** outputs are bit-identical (determinism contract intact).

---

### User Story 2 - Every host gets correct words, accelerated or not (Priority: P1)

A developer builds and runs the project on any supported host —
including hosts where the accelerated data-parallel path cannot run.
The evaluator always produces the same words: where the accelerated
path applies it is selected automatically, and everywhere else the
portable scalar-word path answers with identical results. No new
required dependency, no build break on the baseline compiler, and no
user-facing switch the user must learn to get correct results.

**Why this priority**: Correctness on every host is the precondition
for claiming a speedup on one host — a faster path that fails to build
or diverges on another host is a regression, not an optimization.

**Independent Test**: Can be fully tested by building with the
accelerated path disabled (and on the baseline compiler/host) and
asserting the full truth corpus still matches the accelerated build
word-for-word — delivering portable correctness with no benchmark
story involved.

**Acceptance Scenarios**:

1. **Given** a build with the accelerated path disabled, **When** the
   truth corpus is evaluated, **Then** every word matches the
   accelerated build exactly.
2. **Given** the baseline compiler and default build presets,
   **When** the project configures and builds, **Then** the build
   succeeds with zero new warnings and no new required dependency.
3. **Given** a host where the accelerated path does not apply,
   **When** evaluation runs, **Then** results are still correct and
   the fallback is automatic — the user takes no action.

---

### User Story 3 - Nothing outside speed moves (Priority: P2)

The pre-feature suite is the regression pin. Scalar-vs-word parity,
the four-value refusal (shared/multi-driver and buffer nets stay on
the scalar path), sequential-gate fallthrough, input-column
convention, output ordering, loud error vocabulary (undersized
scratch, out-of-range slice, topology defects), save/load round-trips,
and example replays all behave exactly as before — only the
per-gate-word cost changes.

**Why this priority**: The domain's standing contract preserves
documented behavior unless the task explicitly changes it, and this
task explicitly changes nothing but speed.

**Independent Test**: Can be fully tested by running every
pre-feature golden, refusal case, parity check, and round-trip and
asserting identical outputs and verdicts — no benchmark story
required.

**Acceptance Scenarios**:

1. **Given** any clean 0/1 net in the corpus, **When** the scalar solve
   and the word evaluator are compared slice by slice, **Then** they
   agree bit-exactly exactly as before.
2. **Given** a net carrying shared-net marks or buffer gates,
   **When** word evaluation is attempted through the guarded entries,
   **Then** it is refused loudly through the standing failure
   vocabulary — the accelerated path never silently folds a
   four-value net into 1-bit lanes.
3. **Given** a net with sequential gates, **When** word-evaluated,
   **Then** sequential outputs follow the standing fallthrough
   convention exactly as before.
4. **Given** undersized scratch, a mismatched output count, or an
   out-of-range slice index, **When** evaluation is requested,
   **Then** the call fails loudly with the standing error — never a
   partial result.
5. **Given** any pre-feature golden, replay, or round-trip,
   **When** run after the change, **Then** its output is identical to
   before the change.

---

### User Story 4 - The win is measured and nothing else regresses (Priority: P2)

The speedup claim is evidenced, not asserted. The committed benchmark
record gains before/after numbers for the word-evaluation stage on
the standing growth ladder (warmup separated, checksummed outputs,
opaque inputs, per-size footprint), and the full baseline sweep shows
no other stage regressed beyond measurement noise.

**Why this priority**: The project's performance rule admits only
measured optimization; without committed evidence this feature is
indistinguishable from appearance-based tuning.

**Independent Test**: Can be fully tested by running the shipped
benchmark entries before and after and reading the committed record:
the word-evaluation curve moved, every other stage stayed within
noise. Delivers value alone: the evidence trail.

**Acceptance Scenarios**:

1. **Given** the shipped word-evaluation benchmark, **When** run on
   the pinned environment before and after, **Then** both numbers are
   committed beside the baseline with environment, repetition, and
   variance stated.
2. **Given** the full baseline sweep re-run after the change,
   **When** stage costs are compared, **Then** no stage other than
   word evaluation regressed beyond measurement noise.
3. **Given** any timed evaluation run, **When** measured, **Then**
   inputs stay opaque and outputs stay checksummed, so no measured
   cost is constant-folded away.

---

### Edge Cases

- What happens on a **partial final word** (fewer than 6 block
  inputs)? Don't-care lanes are zero-masked on outputs exactly as
  today; the accelerated path must mask lanes it never computed.
- What happens when the **slice count is not a multiple of the
  data-parallel width** (tail slices)? Tail slices evaluate correctly
  through the portable path or masked lanes — never dropped, never
  padded into the outputs.
- What happens with **very large slice indices** (nets with 31+
  block inputs accept every slice)? Index arithmetic stays exact; no
  truncation or wrap changes which rows a word covers.
- What happens when **four-value content reaches the evaluator**
  (shared marks, buffer gates, X/Z drivers)? The standing loud
  refusal at the guarded entries fires before any data-parallel work;
  the accelerated path never observes X/Z.
- What happens with **sequential gates in the level order**? They
  keep the standing fallthrough (zero contribution); the accelerated
  path must not invent values for state elements.
- What happens with **undersized scratch or mismatched outputs**?
  Loud refusal before any work, through the standing failure
  vocabulary.
- What happens with **empty or degenerate nets** (no gates, no
  inputs, no outputs)? Verdicts and outputs match the scalar-word
  path exactly.
- What happens when the **accelerated path is unavailable at build
  or run time**? The portable path answers automatically with
  identical words; availability never changes semantics.
- What happens when **inputs could be constant-folded**? Timed
  construction stays opaque (runtime-seeded, checksummed) per the
  standing bench rules.
- What happens on **a different host or compiler than the pinned
  environment**? The committed numbers belong to the pinned
  environment; other hosts re-run the shipped entries and record
  beside, never edit in place.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The word evaluator MUST be able to evaluate several
  independent 64-row slices together in one traversal of the compiled
  level order, so one pass produces several slices' outputs.
- **FR-002**: Every output word from the data-parallel path MUST be
  bit-identical to the portable scalar-word path on the same slice:
  same row convention (input `j` from bit `(r>>j)&1`), same lane
  convention (lane `b` holds row `w*64+b`), same output order, same
  partial-word masking.
- **FR-003**: A portable scalar-word path MUST remain and MUST be
  selectable without code changes: builds that cannot use the
  accelerated path produce identical words through the portable path
  alone.
- **FR-004**: Path selection MUST be automatic at build or run time
  and MUST NOT change semantics: availability of the accelerated path
  affects only speed, never words, verdicts, or ordering.
- **FR-005**: The standing guards MUST fire before any data-parallel
  work: four-value nets (shared marks, buffer gates), X/Z content,
  and sequential fallthrough behave exactly as today, through the
  same failure vocabulary.
- **FR-006**: Slice-index validation MUST be unchanged: out-of-range
  slices (input counts below 31) fail loudly; slice arithmetic for
  large indices stays exact.
- **FR-007**: Scratch and output-size validation MUST be unchanged:
  undersized scratch or mismatched output counts refuse loudly
  before any work, through the standing failure vocabulary.
- **FR-008**: Steady-state evaluation MUST remain allocation-free and
  exception-free; scratch stays caller-owned with the standing size
  conventions; no new per-evaluation heap use.
- **FR-009**: Existing public entry points and their signatures MUST
  be preserved; no consumer outside the logic domain changes
  (callers keep looping over slices as today).
- **FR-010**: Determinism MUST hold: repeated evaluation of the same
  slices is bit-identical, and enumeration order stays pinned to the
  standing slot-order convention.
- **FR-011**: The change MUST be measured: before/after
  per-gate-word numbers from the shipped benchmark on the pinned
  environment, with warmup separated, checksummed outputs, opaque
  inputs, and per-size footprint; no other baseline stage regressed
  beyond noise.
- **FR-012**: A parity corpus MUST be added covering all
  combinational gate kinds, mixed fan-in, tied inputs, ignored
  dangling inputs, partial-word input counts, sequential fallthrough,
  refusal classes, and degenerate nets — asserting data-parallel vs
  portable agreement on every slice plus scalar-vs-word agreement.
- **FR-013**: The build MUST gain no new required dependency and
  MUST stay warning-clean on the baseline compiler; optional
  acceleration degrades to the portable path, never to a configure
  failure.

### Key Entities *(include if feature involves data)*

- **64-row word slice**: the unit of bit-parallel truth — word `w`
  covers rows `[w*64, w*64+63]`, lane `b` holds row `w*64+b`; the
  row/lane convention shared with scalar solve and the `truth` verb.
- **Word-evaluation pass**: one traversal of the compiled level
  order producing output words; after this feature one pass may
  produce several slices' outputs instead of one.
- **Portable scalar-word path**: the standing one-word-at-a-time
  evaluation; the correctness reference and the automatic fallback.
- **Data-parallel path**: the accelerated evaluation of several
  independent slices together; a pure speedup with identical words.
- **Path selection**: the automatic build/run-time choice between
  the two paths; speed-only, never semantic.
- **Benchmark record**: the committed before/after per-gate-word
  numbers with environment pinning, variance policy, and footprint.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every slice of every net in the committed corpus
  evaluates bit-identically before and after the change, across both
  paths and against the scalar solve.
- **SC-002**: On the pinned environment, the shipped word-evaluation
  benchmark reports a lower per-gate-word cost after the change by a
  stated, committed factor, with the full procedure reproducible
  from the committed record.
- **SC-003**: The full pre-feature logic test suite passes
  unmodified, and every refusal, parity, and round-trip case in the
  corpus produces identical verdicts and outputs.
- **SC-004**: A build with the accelerated path disabled produces
  word-for-word identical truth output to the accelerated build on
  the whole corpus, and the default build succeeds on the baseline
  compiler with no new required dependency and no new warnings.
- **SC-005**: The full baseline sweep re-run shows no stage other
  than word evaluation regressed beyond the stated measurement
  noise, with per-size footprints reported.

## Assumptions

- The 015 scale-profiling baseline (its word-evaluation benchmark,
  committed numbers, and growth-ladder conventions) exists and is
  the before-record this feature compares against.
- The word evaluator's row/lane conventions, input-column
  derivation, output ordering, partial-word masking, and loud-error
  vocabulary are frozen by the standing contracts; this feature
  changes only how many slices one pass computes.
- The exact data-parallel width, instruction-set targeting, and
  compile-time vs run-time dispatch mechanism are planning
  decisions; this spec contracts only the observable guarantees
  (identical words, automatic fallback, no new dependency).
- The caches from the adjacency feature (016) are an orthogonal
  substrate: this feature consumes compiled level order as it finds
  it and must neither assume nor break cache invariants.
- Slice-level parallelism (several independent words together) is
  the contracted direction; bit-level parallelism stays 64-wide
  per word and gate semantics stay exactly as documented.
- No CLI, GUI, TUI, wire-format, or serialization surface changes:
  the evaluator's callers and artifacts are untouched.
- Benchmarks keep opaque, non-constant-foldable inputs and
  checksummed outputs per the standing bench rules; benchmarks
  never link the assertion policy.
