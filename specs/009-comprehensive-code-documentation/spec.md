# Feature Specification: Comprehensive Code Documentation

**Feature Branch**: `009-comprehensive-code-documentation`

**Created**: 2026-11-19

**Status**: Draft

**Input**: User description: "Complex, comprehensive, extensive, auto generated documentation for the code. How it works from the perspective of developers, API usage/workings and documentation from the perspective of the end user who wants to just use this engine. Documentation should cover everything from actual engine code to tests/becnhmarks/examples/gui/tui/etc. For every test there need to be documentation explaining what it is checking, what's the purpose of it, what is it guarding against, etc. I want extremely well documentated code."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Understand how the engine works without source archaeology (Priority: P1)

A developer — a new contributor, a maintainer revisiting an area, or a
reviewer assessing a change — opens the documentation and comes away
understanding how the engine works as a whole: what every module is
responsible for (quantities and units, solvers and part-specific chip
calculators, simulation state, the fixed-step runner, snapshots and
output sinks, signal generation and routing, the inter-process link
bus, the logic domain, the robotics bridge, and the command-line,
terminal, and graphical front ends), how one simulation step flows end
to end from configuration through state mutation to emitted output,
and which rules the code must always obey (dependency direction,
determinism guarantees, units discipline, error vocabulary) — plus
where to start when extending each documented extension point (a new
solver, a new command, a new sink, a new signal kind, a new panel or
view).

**Why this priority**: This is the first-listed ask ("how it works
from the perspective of developers") and the foundation every other
document hangs from. Today that knowledge lives only in scattered
guides and inside the original authors' heads; without a coherent
internals narrative, every other audience document risks drifting
from the code, and every onboarding repeats the same source
archaeology.

**Independent Test**: A developer who has never touched the codebase
reads only the developer guide and correctly answers sampled questions
such as "what happens during one simulation step, end to end", "where
does a new solver plug in", and "why must an output sink never see
live mutable state" — delivering comprehension value with no other
document required.

**Acceptance Scenarios**:

1. **Given** a developer new to the project, **When** they read the
   developer guide top to bottom, **Then** they can name every
   module's responsibility and describe the end-to-end life of a
   simulation (configuration → state → stepping → snapshot → sinks →
   outputs).
2. **Given** a proposed change to one module, **When** the developer
   looks that module up in the guide, **Then** they find its
   collaborators, the rules it must obey, and the consequences of
   violating them.
3. **Given** a determinism-sensitive behavior (fixed-step ordering,
   stable signal serials, snapshot timing), **When** the developer
   reads its section, **Then** the rule and its rationale are both
   stated.

---

### User Story 2 - Run the engine as a plain end user, documentation only (Priority: P1)

Someone who "just wants to use this engine" — a student, an engineer,
or a script author with no intention of reading source — goes from
zero to productive using only documentation: prerequisites and build,
a first headless run, the interactive terminal, every command family
and what each is for (generic stepped runs, signal sources, chip
calculators, the logic domain), recording and reading output files,
the graphical application's panels and workflows (solving, stepped
simulation control, signal batches, graphs, the 2D canvas, the 3D
viewport, external sources), and diagnosing failures from documented
exit codes and error messages.

**Why this priority**: The end user is explicitly requested as a
distinct audience. The knowledge exists today but is spread across a
top-level README written for repository-literate readers and
per-topic guides that assume context; a plain user has no single
guided path from install to confident daily use.

**Independent Test**: On a clean machine meeting the stated
prerequisites, a user follows the guide start to finish and produces
(a) a completed build, (b) a running simulation, and (c) an output
file they can interpret — with every command they are told to run
working exactly as written.

**Acceptance Scenarios**:

1. **Given** a clean machine with only the documented prerequisites,
   **When** the user follows the getting-started path, **Then** they
   build and run a first simulation successfully without consulting
   any source file.
2. **Given** the user wants one specific capability (record a run to
   an output file, view a waveform, drive the interactive terminal,
   attach an external source in the graphical app), **When** they
   look it up, **Then** step-by-step instructions exist and execute
   as written.
3. **Given** a command fails (bad arguments versus a runtime
   failure), **When** the user consults the guide's error section,
   **Then** the documented exit codes and error vocabulary let them
   diagnose the failure without reading code.

---

### User Story 3 - Every test explained: what it checks, why, what it guards against (Priority: P1)

A maintainer or reviewer opens one catalog and finds an entry for
every test artifact in the repository — every test binary and every
test-support script or probe. Each entry states what the test checks,
why it exists, what regression class it guards against (a determinism
break, a serialization drift, a silently dropped route, a front-end
model diverging from the engine, an interactive-mode regression, and
so on), how to run it, any environment prerequisites (a real
terminal, the robotics stack, embedded hardware), and what the test
deliberately does not cover.

**Why this priority**: This is the most emphatically stated
requirement in the input ("for every test there need to be
documentation..."). Today this knowledge lives only in the test
authors' heads; without it, reviewers cannot tell what a passing
suite actually proves, and deleted or loosened assertions go
unnoticed.

**Independent Test**: Pick any test artifact at random; its catalog
entry answers what it checks, its purpose, what it guards against,
and how to run it — and a reader who has never seen the test can
predict which regressions it would catch.

**Acceptance Scenarios**:

1. **Given** any test binary or test-support script, **When** the
   reader opens its catalog entry, **Then** they find what it checks,
   why it exists, what it guards against, and how to run it.
2. **Given** a reviewer assessing a subsystem's protection (for
   example, the inter-process transport, or the interactive forms),
   **When** they read that subsystem's catalog entries, **Then** they
   can enumerate which regression classes are guarded and which are
   explicitly out of that test's scope.
3. **Given** a test requiring a special environment, **When** the
   reader checks its entry, **Then** the prerequisites are stated
   along with what is and is not verifiable without them.

---

### User Story 4 - Consume the engine's API with confidence (Priority: P2)

A developer embedding the engine into another application — writing
a graphical panel, a link bus reader, a custom output sink, or a new
solver — uses per-module API documentation to do it correctly the
first time: each public interface's purpose, the physical
quantity/unit conventions at its boundaries, ownership and lifetime
rules, failure behavior and the required caller response, the
determinism obligations inherited from the engine, and a minimal
correct usage pattern to copy.

**Why this priority**: Explicitly requested ("API usage/workings").
It ranks below the three P1 stories only because its audience is
smaller, and because the API documentation coheres best once the
module map from User Story 1 exists to anchor it.

**Independent Test**: An embedder writes a minimal consuming program
guided only by the API documentation — run a solver for a number of
steps with an output sink attached, or attach a reader to a live
bus — and it works as documented.

**Acceptance Scenarios**:

1. **Given** any public engine module, **When** a consumer reads its
   API section, **Then** every public type and operation carries
   documented purpose, unit conventions, ownership, and failure
   behavior.
2. **Given** the documented usage patterns, **When** they are
   followed verbatim, **Then** the resulting consumer code works as
   documented.
3. **Given** an operation that can fail, **When** it is documented,
   **Then** the failure mode and the caller's required response are
   both stated.

---

### User Story 5 - Run and interpret every benchmark and example (Priority: P2)

A performance-minded contributor finds each benchmark explained —
the path it measures, why that path matters, how to run it, how to
read its output, and the pitfalls that make numbers lie (including
benchmarks that accidentally measure nothing because the work was
simplified away before running). Each shipped example is likewise
explained: what it demonstrates, how to build and run it, and its
expected observable outcome — including the helper receiver and
plotting scripts and the embedded-hardware example.

**Why this priority**: Falls squarely inside "everything from ...
benchmarks/examples ... etc.", and it makes the project's
measured-performance-only principle actionable. It serves
contributors rather than the primary use-and-understand flows, so it
follows the P1 stories.

**Independent Test**: Run any benchmark or example following only
its documentation; the run succeeds, and the reader correctly states
what was and was not measured.

**Acceptance Scenarios**:

1. **Given** any benchmark, **When** the reader opens its entry,
   **Then** what it measures, why it matters, how to run it, and how
   to interpret it are all stated.
2. **Given** any example, **When** its instructions are followed,
   **Then** it builds, runs, and produces the documented outcome.
3. **Given** a benchmark result, **When** it is read against the
   entry's interpretation guidance, **Then** the reader can say what
   was measured and what was deliberately excluded.

---

### User Story 6 - The code explains itself in place (Priority: P2)

A developer reading the source directly — in an editor, with no
prose document open — finds the code extremely well documented where
it matters: public interface definitions state their contracts
(purpose, units, ownership, failure modes, invariants); non-obvious
logic (floating-point evaluation order, lock-free snapshot reads,
terminal raw-mode re-entry quirks, protocol framing details)
explains itself at the site; nothing depends on tribal knowledge.
These are strictly explanatory changes: build output and test results
are identical afterward.

**Why this priority**: "I want extremely well documented code" is
the closing emphasis of the request, and the source is the final
source of truth that never drifts from itself. It ranks below the
P1 stories because audience-organized documents deliver value sooner
to the three named audiences, and because in-code work is safest
done module by module after those documents pin down the intended
contracts.

**Independent Test**: Open any public interface file; a new reader
understands its contract without leaving the file, and the full test
suite passes with outcomes identical to the pre-change baseline
(comments only).

**Acceptance Scenarios**:

1. **Given** any public interface definition, **When** it is read
   alone, **Then** its contract documentation (purpose, units,
   ownership, failure behavior) is present at the definition site.
2. **Given** tricky logic (floating-point evaluation order,
   lock-free snapshot reads, terminal raw-mode re-entry, protocol
   framing bytes), **When** it is read, **Then** a comment at the
   site explains the why, not just the what.
3. **Given** the completed in-code documentation pass, **When** the
   project is built and the full suite runs, **Then** every outcome
   is identical to the pre-change baseline.

---

### User Story 7 - One index, no contradictions, a refresh path (Priority: P3)

Any reader — end user, developer, or reviewer — enters the
documentation at a single index organized by audience and reaches
the right document within a few navigation steps. The new suite
cross-references the repository's existing documentation corpus
instead of forking or contradicting it. The suite documents how it
was produced and how to regenerate or refresh it when the code
changes, so "auto generated" means a repeatable process rather than
one-off prose that silently rots.

**Why this priority**: Pure leverage: navigation, consistency, and
freshness multiply the value of the P1/P2 content, but only make
sense once that content exists.

**Independent Test**: From the index, a reader answers a sampled
question from each of the three audiences within three navigation
steps, and a consistency audit finds zero contradictions between the
suite and the pre-existing corpus.

**Acceptance Scenarios**:

1. **Given** any of the three audiences, **When** they open the
   index, **Then** a labeled path leads to their documentation.
2. **Given** a topic already covered by an existing repository
   document, **When** the new suite touches that topic, **Then** it
   cross-references the canonical source rather than restating a
   divergent copy.
3. **Given** the codebase changes after delivery, **When** the
   documentation must catch up, **Then** the documented regeneration
   process tells the maintainer how to refresh the affected parts.

---

### Edge Cases

- What happens when documentation and code disagree? During
  generation, contradictions are resolved toward actual behavior and
  reported as findings (a documentation/behavior mismatch may be a
  defect worth its own task); documentation must never paper over a
  suspected bug by describing it as intended. After delivery, the
  documented regeneration process is the refresh path for drift.
- What about topics the repository already documents (the top-level
  README, the glossary, and the per-topic guides for signals, the
  terminal UI, logic, delivery guarantees, the robotics bridge,
  hardware-in-the-loop, and domain outputs)? The suite
  cross-references and supplements them; it never forks their
  content. Where a topic needs deeper coverage, the new document
  links to the existing one and extends it.
- How are artifacts documented that cannot run everywhere (tests
  needing a real terminal session, the robotics bridge, embedded
  hardware, windowed graphical verification)? Their entries state
  the prerequisites, what is verifiable without the environment, and
  what the entry's claims rest on (for example, observed behavior
  during a manual run).
- What about test-support artifacts that are not regular test
  binaries (probes and scripted terminal checks)? They are part of
  the test fabric and receive catalog entries like any test.
- What about vendored third-party code? Out of scope: the suite
  documents project-owned code only.
- What happens when an explanatory edit risks changing behavior
  (editing slips, macro pitfalls)? The requirement is comment-only
  edits verified by an unchanged build and identical test outcomes;
  anything that cannot stay comment-only is split out as a reported
  finding instead of being smuggled in.
- What happens if a component cannot be documented within delivery
  due to sheer size? Coverage is auditable by construction (catalog
  entries versus inventories), so any gap becomes a visible,
  explicitly marked "not yet documented" entry rather than a silent
  hole.
- What about components that are announced but not yet shipped (for
  example, pending transports or control planes)? They are documented
  as the code behaves today, with their pending status stated — never
  documented as if already shipped.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The suite MUST include a developer guide explaining
  every engine module's responsibility, collaborators, and governing
  rules (dependency direction, determinism guarantees, units
  discipline, error vocabulary), the end-to-end life of a simulation
  from configuration to emitted output, and the entry path for each
  documented extension point (a new solver, a new command, a new
  sink, a new signal kind, a new panel or view).
- **FR-002**: The suite MUST include an end-user guide that takes a
  reader from a clean machine meeting stated prerequisites to a
  running simulation with inspectable output, covering build, the
  interactive terminal, every command family and its purpose, the
  graphical application's features and workflows, output
  interpretation, and error/exit-code diagnosis; every instruction
  MUST be executable exactly as written.
- **FR-003**: The suite MUST include API usage documentation for
  every public engine module: its public surface, the
  quantity/unit conventions at its boundaries, ownership and
  lifetime rules, failure behavior and the required caller response,
  inherited determinism obligations, and at least one minimal
  correct usage pattern per module.
- **FR-004**: For EVERY test artifact (every test binary and every
  test-support script or probe), the suite MUST provide a catalog
  entry stating what it checks, why it exists, what regression class
  it guards against, how to run it, environment prerequisites, and
  its deliberate coverage boundaries. Catalog completeness MUST be
  auditable against the full test-artifact inventory.
- **FR-005**: For EVERY benchmark, the suite MUST provide an entry
  stating the measured path, why that path matters, how to run it,
  how to interpret its output, and known interpretation pitfalls
  (including benchmarks whose work is simplified away before
  running, measuring nothing).
- **FR-006**: For EVERY shipped example (including helper receiver
  and plotting scripts and the embedded-hardware example), the suite
  MUST provide an entry stating what it demonstrates, how to build
  and run it, and its expected observable outcome.
- **FR-007**: The graphical and terminal-interactive applications
  MUST be documented for both audiences: a user walkthrough
  (features, workflows, what each control does) and a developer
  explanation (internal layering, how the application attaches to
  the engine, and why external-source failures stay on the display
  side).
- **FR-008**: In-code documentation MUST reach contract level at
  every public interface definition site, and non-obvious logic MUST
  carry an explanatory comment at the site; all such edits MUST be
  explanatory-only, with build output and test outcomes identical to
  the pre-change baseline.
- **FR-009**: The suite MUST provide a single audience-oriented index
  as its entry point, and every document MUST cross-reference the
  repository's existing documentation corpus rather than duplicate
  or contradict it.
- **FR-010**: Contradictions discovered between existing
  documentation and actual behavior MUST be resolved toward actual
  behavior and reported as findings; the suite MUST NOT enshrine
  suspected defects as intended behavior.
- **FR-011**: The suite MUST document how it was generated and how to
  regenerate or refresh it after code changes, making the
  "auto-generated" property a repeatable process rather than one-off
  prose.
- **FR-012**: Coverage MUST span all project-owned deliverables —
  engine modules, the command-line, terminal, and graphical
  applications, tests, benchmarks, examples, and build
  configurations — and MUST exclude vendored third-party code.
- **FR-013**: Every command, example, and usage pattern the suite
  presents as factual MUST be verified to work as documented before
  delivery.

### Key Entities *(include if feature involves data)*

- **Documentation Suite**: the complete generated set; the top-level
  deliverable, organized by audience (end user, developer, API
  consumer, reviewer).
- **Developer Guide**: the narrative explanation of engine internals —
  module responsibilities, step lifecycle, invariants, extension
  points (User Story 1).
- **End-User Guide**: operational documentation for people who run
  simulations without modifying the engine (User Story 2).
- **API Reference**: per-module contract documentation for consumers
  of the engine's public interfaces (User Story 4).
- **Test Catalog**: one entry per test artifact — what it checks,
  purpose, guards, run instructions, prerequisites, boundaries
  (User Story 3).
- **Benchmark Catalog**: one entry per benchmark — measured path,
  rationale, run instructions, interpretation, pitfalls (User Story
  5).
- **Example Catalog**: one entry per example — demonstrated
  capability, run instructions, expected outcome (User Story 5).
- **Documentation Index**: the single audience-oriented entry point
  cross-referencing the whole suite and the pre-existing corpus
  (User Story 7).
- **In-code Contract Comments**: explanatory documentation at
  definition sites inside the source; strictly behavior-neutral
  (User Story 6).
- **Existing Documentation Corpus**: the repository's pre-existing
  documents (top-level README, glossary, per-topic guides); the
  canonical sources the suite extends rather than forks.
- **Regeneration Process**: the documented procedure describing how
  the suite was produced and how to refresh it after code changes
  (User Story 7).
- **Finding**: a discovered documentation/behavior contradiction or
  suspected defect, reported rather than hidden — and not fixed
  within this feature.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: 100% of the repository's test artifacts have Test
  Catalog entries containing what-it-checks, purpose,
  guards-against, and run instructions; a completeness audit
  comparing catalog entries against the test-artifact inventory
  finds zero missing entries.
- **SC-002**: A first-time user, on a clean machine meeting the
  stated prerequisites, completes build → first running simulation →
  inspectable output within 30 minutes using only the End-User
  Guide, encountering zero commands that fail as written.
- **SC-003**: 100% of engine modules have Developer Guide coverage,
  and 100% of public interfaces have contract documentation (API
  Reference plus in-code comments); a coverage audit finds zero
  undocumented modules or public interfaces.
- **SC-004**: 100% of benchmarks and examples have catalog entries
  with purpose, run instructions, and interpretation or expected
  outcome.
- **SC-005**: A developer previously unfamiliar with the codebase
  correctly answers at least 9 of 10 sampled architecture questions
  (step lifecycle, module responsibilities, determinism rules,
  extension-point entry paths) using only the Developer Guide.
- **SC-006**: The documentation accuracy pass (FR-013) reports zero
  broken instructions across the suite.
- **SC-007**: In-code documentation changes leave the full test
  suite with outcomes identical to the pre-change baseline
  (explanatory-only verified end to end).
- **SC-008**: From the Documentation Index, each of the three
  audiences reaches a sampled question's answer within three
  navigation steps, and a consistency audit finds zero contradictions
  between the suite and the existing corpus.

## Assumptions

- "Auto generated" was resolved by user decision during planning
  (2026-11-19): the suite is generated with Sphinx + MyST (authored
  pages) and Doxygen (API extraction from Doxygen-convention source
  comments), rendered as a logically indexed HTML site. The spec still
  mandates outcomes — coverage, accuracy, navigation, and a repeatable
  refresh path — while generator specifics live in the plan
  (`plan.md` / `research.md`).
- The suite lives in the repository, following its established
  documentation format and location conventions, and extends the
  existing corpus rather than replacing it.
- "Every test" includes all test binaries plus test-support artifacts
  (probes and scripted terminal checks). The current inventory is
  roughly seventy test binaries plus support scripts; the catalog
  must match the inventory at delivery time, not this snapshot count.
- "TUI" refers to the project's existing interactive terminal
  application (the REPL-style command interface and terminal UI); it
  is documented as it currently behaves.
- The End-User Guide assumes comfort with running terminal commands
  but zero familiarity with the codebase; the Developer Guide
  assumes professional software-development competence.
- Graphical-application documentation is verified by actually
  running the application (the project's established
  manual-verification method for windowed behavior), since no
  automated interaction tooling exists.
- Environment-specific components (the robotics bridge, the
  embedded-hardware example, real-terminal test behavior) are
  documented with their prerequisites even where they cannot be
  exercised on every machine.
- This feature is documentation-only: suspected defects discovered
  while documenting are reported as findings, not fixed within this
  feature.
- User stories are independently shippable slices: any one delivered
  alone still provides standalone value, per the prioritized-journey
  model above.
