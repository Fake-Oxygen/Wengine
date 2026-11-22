# Specification Quality Checklist: Cached Adjacency Lists and Maintained Indexes for Logic-Domain Hot Paths

**Purpose**: Validate specification completeness and quality before proceeding to planning
**Created**: 2026-11-22 (amended same session after the requested high-cost-path investigation)
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, APIs) — data-structure choices explicitly deferred to planning (Assumptions); anchors used are the project's own benchmark/test vocabulary (stage names, corpus, seam terms), consistent with the 015 spec precedent
- [x] Focused on user value and business needs — every story frames the developer-visible outcome (authoring/wiring/tying/teardown/optimizing at scale, identical verdicts, measured evidence)
- [x] Written for the feature's audience — engine developers; this is an engine-internal performance feature, matching the repo's established spec style (see Notes)
- [x] All mandatory sections completed — User Scenarios & Testing, Requirements, Success Criteria; Key Entities included

## Requirement Completeness

- [x] No [NEEDS CLARIFICATION] markers remain — open design points (cache representations, free-slot index form, shared-feeder state form, dedup key buckets) recorded as planning decisions in Assumptions instead
- [x] Requirements are testable and unambiguous — each FR names a verifiable condition (bit-identical outputs, cost-proportional bounds, loud refusal, corpus parity, identical slot allocation)
- [x] Success criteria are measurable — before/after benchmark numbers per stage, bit-identity of outputs, no-regression bound
- [x] Success criteria are technology-agnostic — expressed as cost-growth behavior and output identity, not internal mechanics
- [x] All acceptance scenarios are defined — 6 stories, all scenarios in Given/When/Then form
- [x] Edge cases are identified — 13 edge cases covering slot recycling, dangling drivers, extra-edge cycles, dual edge filters, single-mutation cost, pool growth, degenerate nets, optimizer mutations, free-slot-index staleness, shared-feeder/ mark disagreement, dedup key collisions, mid-pipeline staleness
- [x] Scope is clearly bounded — caches/indexes are engine-internal derived state; no CLI/GUI/TUI/serialization surface changes (Assumptions, FR-013); ruled-out paths named so planning does not re-litigate
- [x] Dependencies and assumptions identified — 015 baseline record as before-numbers source; mutation seam funneling; per-pin caps; representations deferred

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria — FR-to-story mapping: FR-002/003 ↔ US1, FR-004/005 ↔ US2, FR-006/007 ↔ US3, FR-008–012 ↔ US4, FR-017/018 ↔ US5, FR-019–023 ↔ US6, FR-001/013/014 ↔ cross-cutting, FR-015/016 ↔ testing/docs duties
- [x] User scenarios cover primary flows — compile, wire-time probe, tie ops, lifecycle/measurement, authoring add/remove, optimizer pipelines; each independently testable and a viable standalone slice
- [x] Feature meets measurable outcomes defined in Success Criteria
- [x] No implementation details leak into specification — FRs contract cost bounds and output identity, never structure

## Notes

- Items marked incomplete require spec updates before `/speckit.clarify` or `/speckit.plan`
- Audience deviation (deliberate): the checklist item "written for non-technical stakeholders" is interpreted here as "written for the feature's stakeholders" — this spec, like the 015 precedent it follows, targets engine developers; domain vocabulary (Kahn compile, tie classes, workspace) is the project's own documented glossary, not leaked implementation detail.
- The original three hot spots named in the input were verified in source before writing: per-pop full-pool scan in the Kahn drain (logic/topo.hpp drain_successors), per-pop full-pool successor discovery in the reachability walk (topo.hpp reaches), and full-pool member scans in tie operations (tie_set.hpp untie/detach/reindex/enumeration) plus the per-wire probe call site (netlist.hpp wire cycle check).
- Scope extension (same session): four more hot-path families were source-verified and appended as US5/US6 — per-add lowest-free-slot scan (netlist.hpp add_gate), per-remove full-pool sink sweep (netlist.hpp remove_gate), per-gate shared-feeder full-pool predicate (optimize_barrier.hpp drives_shared, called per gate by collect_barrier_roots/bypass and per matching pair by the dedup loop), and optimizer pass scans (all-pairs dedup in optimize_o2_dedup, per-rewire full-pool user sweeps in optimize_passes, per-pass whole-net recounts). Linear-once and bounded paths (steady-state eval walks, per-step phases, layout, bounded exact pass, delay scheduling, reset/load) were investigated and deliberately excluded, with the exclusion rationale recorded in the spec's Assumptions per the project's measured-performance rule.
- The critical anti-regression requirement (FR-008: no full rebuild per single-edge mutation) exists because the baseline harness comment explicitly frames the per-wire probe as "the honest steady-authoring cost" — a rebuild-per-wire cache would move the quadratic cost rather than remove it.
- FR-017's identical-slot-allocation requirement exists because the mutation seam's pool-growth comment explicitly pins allocation order ("allocation order matches the old fixed pool for identical op sequences") — a faster allocator that shifts slot assignment would break golden outputs.
- FR-019's requirement that the shared-feeder predicate never clear a gate the marks still implicate exists because a wrong feeder verdict would silently break the 011 driver-side freeze (folding through a shared span).
