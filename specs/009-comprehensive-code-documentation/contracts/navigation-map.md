# Contract: Navigation & Index

**Feature**: `specs/009-comprehensive-code-documentation/spec.md` | **Date**: 2026-11-19

The indexing contract behind "not required to hunt" (User Story 7, SC-008)
and FR-009's extend-don't-fork rule.

## NC1. Landing page: five audience portals

`docs/index.md` presents labeled portals, one paragraph each:

| Portal | Audience | Entry |
|--------|----------|-------|
| Using WEngine | run simulations, read outputs | `user/getting-started.md` |
| Developing WEngine | change/extend the engine | `dev/architecture.md` |
| API Reference | embed the engine in another app | `api/index.md` |
| Quality — Tests & Benchmarks | reviewers, maintainers | `tests/index.md`, `benchmarks/index.md` |
| Examples | everyone | `examples/index.md` |

README.md and GLOSSARY.md are linked prominently from the landing page
as repository documents (not copied — NC5).

## NC2. Toctree shape

- Sidebar sections mirror the portals; ≤ 3 navigation levels below the
  landing page (furo's sweet spot, research R2).
- Page files are `kebab-case.md`; titles are sentence-case.
- Every page joins exactly one toctree slot; orphan pages are build
  warnings treated as failures (BC4) — no page can become unreachable.

## NC3. Existing corpus placement (linked once, never restated)

| Guide | Toctree slot |
|-------|--------------|
| `docs/signals.md` | Developing → Signal subsystem (design + decision log) |
| `docs/delivery.md` | Developing → Link transports (delivery guarantees) |
| `docs/ros2.md` | Developing → Integrations |
| `docs/zephyr-hil.md` | Developing → Integrations |
| `docs/tui.md` | Using → Terminal UI |
| `docs/logic.md` | Using → Logic domain |
| `docs/domain-outputs.md` | Using → Outputs |

Files stay at their current paths (AGENTS/README link stability,
research R3). New pages **link into** them for depth instead of
duplicating their content; contradictions discovered become findings
(FR-010).

## NC4. Anchors and deep links

- Test entries: `## test_<exact-ctarget-name>` sections (SC-001 audit
  identity).
- Benchmark entries: `## bench_<name>`; example entries: `## <file>`
  (schema in `catalog-schemas.md`).
- Dev-guide subsystem pages deep-link to the catalog anchors when they
  claim "this is guarded by test X" — the claim must be one click
  verifiable.

## NC5. Cross-link rules

- Canonical content wins: the topic guides stay canonical for their
  subject; new pages summarize one paragraph and link.
- No page restates README's build/module tables; link them.
- Search expectations: site search covers all Sphinx sources; it does
  not cover out-of-root README/GLOSSARY (known limitation, research
  R3) — the landing page links both explicitly.

## NC6. The 3-step rule (SC-008)

For each audience, a sampled question must reach its answer in ≤ 3
navigation steps from the landing page, verified in the quickstart:

- User: "how do I emit a CSV of a run?" → portal → outputs page →
  command section.
- Developer: "where does a new solver plug in?" → portal → extension
  points page → solver section.
- Reviewer: "what does `test_link_protocol` guard?" → portal → domain
  page → entry anchor.
