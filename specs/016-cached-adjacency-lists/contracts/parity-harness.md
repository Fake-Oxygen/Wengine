# Contract: Parity Harness Protocol (parity-harness.md)

**Feature**: [spec.md](../spec.md) FR-015/003/005/007/017..023 | **Research**: D9 | **Model**: [data-model.md](../data-model.md) §ParityCorpus

How old-vs-new agreement is proven before, during, and after the
implementation. Constitution Principles I/III operationalized for this
feature.

## 1. The oracle

1.1. Before any production header changes, the shipped scan
implementations are extracted **verbatim** (no rewrites, no "equivalent
simplifications") into `tests/logic_scan_reference.hpp`, test-only,
compiled only into `test_logic_adjacency.cpp`:

- Kahn per-pop full-pool successor drain (today's `drain_successors`);
- DFS probe successor discovery (today's `reaches` inner scan);
- tie member/group/re-star full-pool scans (`tie_class_members`,
  `tie_group_size`, `untie_pin` survivor sweep, `tie_pins` re-star,
  `detach_slot_ties`, `reindex_kept_ties`);
- `drives_shared` full-pool predicate;
- all-pairs structural dedup loop (today's `dedup` double loop);
- lowest-free-slot linear scan (today's `add_gate` scan);
- full-pool `rewire_users` sweep and `remove_gate` sink sweep.

1.2. The oracle links `wengine_core` and asserts under the `test_`
prefix assertion policy like every test binary. It is deleted only
when a subsequent, separately-requested cleanup removes it — not as
part of this feature's "done".

## 2. The corpus (deterministic, committed, reproducible)

2.1. Net families, each with fixed construction parameters (no
runtime randomness):

- layered combinational growth (the 015 motif-0 shape) at small sizes;
- shared nets: marked inputs, primary + up to cap extras, NQ polarity
  mixes, mark-then-wire AND wire-then-mark orders, promote-on-unwire;
- tie classes: cross-class merges, leader removal re-election,
  non-leader untie, two-member dissolve, slot detach on removal,
  input-drop compaction reindex;
- sequential nets: flop feedback (legal) and pure combinational
  cycles (refused), T/JK/SR/D members, edge-capture paths;
- defects: dangling primaries, dangling extras, extra-closing cycles.

2.2. Replay scripts interleave mutations and queries in fixed order
(add/wire/unwire/remove/tie/untie/retag/reset/load-roundtrip), the
FR-009 stale-answer pin.

## 3. The comparison levels (what "agrees" means)

For every corpus net and every replay step, after every operation:

3.1. **State equality** — full netlist comparison: per-slot liveness,
  kind, input count, drivers, extra attachments + polarity, tie star
  table, marks, pulls, ignored masks, fanout counters, per-kind
  counters, live count.

3.2. **Verdict equality** — every `core::Status`/boolean answer
  (wire accept/refuse, cycle verdicts, tie refusals, validate
  results, dedup merge decisions, removal results).

3.3. **Output equality** — emitted topological order and level arrays
  (kAll and kCombOnly), combinational stepper order, block interface
  readout where touched.

3.4. **Allocation equality is NOT asserted via addresses** — repeated
  identical sequences must be bit-identical in all of 3.1–3.3;
  allocation-freedom is evidenced by the bench footprint rows
  (Constitution IV), not by value-level tests.

3.5. **Slot-assignment replay** — every add in every script lands in
  the oracle's slot (FR-017); compared per operation, not just at end
  state.

3.6. **Per-sweep dedup state** — after each dedup sweep (not just at
  fixpoint), for nets designed so multiple sweeps fire.

## 4. Standing-suite obligations

4.1. The existing `test_logic*` suite passes unmodified (SC-004);
  any golden inside it that changes is a defect, not an update.

4.2. New tests register `add_executable` + `target_link_libraries` +
  `add_test`; the `test_` prefix inherits the assertion policy; the
  suite builds without optional dependencies (no GUI/ROS anywhere).

4.3. The full-registry/size `static_assert`s elsewhere in the tree
  are re-checked at the tasks phase (AGENTS.md §9 duty); shipped
  identity pins (widths/sentinels/caps) must remain true (research
  D13).

## 5. Documentation duties

5.1. README module table + test list rows for the new test binary;
  `docs/tests/logic.md` entry (six-field CS1 anchor);
  `docs/audit_catalogs.py` stays zero-missing/zero-ghost;
  Doxygen `@brief`/params/`@file` role paragraphs ship with any new
  public helper headers in the same change.
