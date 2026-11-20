# Research: Propagation-Delay Annotation Verbs (013)

**Date**: 2026-11-20
**Inputs**: specs/013-delay-annotation-verbs/spec.md, codebase research
(engine seam, CLI surface, parse machinery, YAML store, optimizer,
list/completion, tests), docs/logic.md §12 (the reserved Timing slot),
specs/012 artifacts (verb/seam/schema conventions).

All resolved decisions below were deferred-to-planning in the spec's
Assumptions; each now has a concrete, precedent-anchored choice.

---

## D1 — Verb identity: `set-delay` / `unset-delay`, registry 25 → 27

**Decision**: the write verb is `set-delay <NAME> <VALUE>` — the name
docs/logic.md §12 already reserves ("an evaluator that honors it plus a
`set-delay` verb") — and the clear verb is `unset-delay <NAME>`,
completing the module's `un<verb>` pair convention (`unwire`, `untie`,
`unshare`, `unpull`). Both are registered as spec structs in
`logic_registry.hpp` and inserted into `LogicRegistry` after
`LogicUnpullSpec` and before `LogicRemoveSpec` (the same cluster
position `pull`/`unpull` took in 012), growing the registry 25 → 27.
Hyphenated verb names are ordinary string tokens to the dispatcher
(`Registry::find` string compare, completion prefix match, TUI argv
composition) — no machinery change. `LogicSpec::kUsage`'s static
session-verb list (commands.hpp:644-645) gains `set-delay unset-delay`
after `unpull`.

**Rationale**: the reserved name is a standing documented commitment —
honoring it costs nothing and keeps §12's promise; the un- pair is the
module's established set/clear shape, so `unset-delay` needs no new
vocabulary.

**Alternatives considered**:
- `delay`/`undelay`: prettier prefix completion, but abandons the
  documented reservation; rejected.
- Single verb with value `0` as clear: works (and `set-delay X 0`
  remains legal, equal to clearing), but the spec requires an explicit
  clear path and the paired-verb precedent is stronger; rejected as the
  sole clear path.
- A `--delay` option on `add`: out of scope per the spec (place, then
  annotate); rejected.

## D2 — Value grammar: exact whole picoseconds, integer arithmetic only

**Decision**: one parse helper in `cli/args.hpp` —
`parse_time_ps(text) -> core::Result<std::uint64_t>` — documented as the
exact whole-picosecond time parse. Grammar and semantics:
- **Bare token** (no suffix): strict decimal-integer picoseconds via
  `std::from_chars` into u64 — full field range, exact; anything with a
  sign, dot, or exponent in the bare form refuses loudly ("bare form is
  integer picoseconds; use a suffix for other spellings"). Negative
  signs refuse as "delay must be non-negative".
- **Suffixed token**: split via the existing `split_number_suffix`
  (args.hpp:240), then a strict plain-decimal grammar on the numeric
  part — `^[0-9]+(\.[0-9]+)?$` (signs and exponents refuse loudly) —
  and the suffix spelling must match a `scales_for<Time>` row
  (case-sensitive, exactly the owned spellings: `s sec ms us ns min h
  ps`). The matched row's scale must classify into the exact decimal
  factor set — `1, 60, 3600` (integers) or `10^-3, 10^-6, 10^-9,
  10^-12` — by double-literal comparison against the canonical
  representations of those values (every current Time row classifies; a
  future unclassifiable row makes the helper refuse that suffix loudly
  rather than guess). The picosecond value is then computed with
  **pure integer arithmetic**: `value = num × factor × 10^(12+exp10)`
  over an implied `10^fracdigits` divisor, evaluated in unsigned
  `__int128` with an explicit u64-overflow check; a nonzero divisor
  remainder refuses loudly ("sub-picosecond value"), as does any
  intermediate overflow ("value exceeds the field").
- No floating point participates anywhere. Exactness holds to the full
  u64 field range for both forms.

**Rationale**: the field stores whole picoseconds and the constitution
forbids silent rounding of stored physical values — a value like
`1.5ps` (sub-ps) or `6000h` (overflow) must refuse, and a value like
`1.5h` → 5,400,000,000,000,000 ps must be exact to the last digit. Only
integer arithmetic gives all three (exact, loud refusal, full range)
with a trivial proof. The classification step reads the owned table's
values rather than duplicating its spellings, keeping the single-source
discipline (cli/args.hpp owns no suffix spellings; units/scales.hpp
does).

**Alternatives considered**:
- **Floating-point recovery** (`parse_time` double result × 1e12, then
  round-to-whole with an error-budget magnitude cap): every suffixed
  value above the cap must refuse, the error budget at the cap runs to
  ~0.44 of the recovery window (margin < 1.3×), and — decisively — the
  sub-ps remainder test still needs exact integer arithmetic to
  distinguish 1.5ps from 1.50...01ps without guessing; rejected.
- Reject suffixed forms entirely (bare ps only): contradicts spec
  FR-002, which commits to the owned time-suffix table; rejected.
- Extending `SuffixScale` rows with exact exponent fields: an
  engine-table surgery for one verb, touching every dimension's rows;
  rejected for v1 (the classification in the helper reaches the same
  exactness reading the same table).

## D3 — Engine seam: `set_gate_delay` / `clear_gate_delay` in netlist.hpp

**Decision**: two functions beside `set_pull` (netlist.hpp:361) and
`clear_pull` (netlist.hpp:393), same shape and Doxygen discipline:
- `set_gate_delay(Netlist&, slot, value_ps) -> Status`: validates a
  live slot (dead/out-of-range → `kInvalidArgument`, netlist
  untouched), then assigns the field. Legal on every live gate kind —
  combinational and sequential alike (spec FR-008: per-kind timing
  semantics belong to the future delay evaluator, and nothing stored
  today constrains it). Idempotent: re-set replaces (last write wins).
- `clear_gate_delay(Netlist&, slot) -> Status`: validates a live slot
  and refuses loudly when the field is zero (the `clear_pull`
  precedent — an un-annotated gate has nothing to clear). The verb maps
  that failure to `usage_error("no delay set on <NAME>")`, exit 2.
- No validation of the magnitude beyond the u64 type: the field's range
  is the contract (spec FR-003's bound is the field itself).

**Rationale**: the verbs-follow-engine-seams pattern (every 012 verb
calls an engine fn) keeps validation parity between the interactive
verbs and the YAML replay path by construction; the live-slot guard is
the only invariant the engine owns here.

**Alternatives considered**:
- Direct field write in the verb TU: skips the seam the module's other
  verbs keep; rejected.
- Kind restrictions (combinational only): contradicts the spec's
  uniform-annotation default and would encode semantics the future
  evaluator owns; rejected.

## D4 — Inspectability: `list` gate-line annotation, nonzero only

**Decision**: the gate line in `list` (list.cpp:46 renders
`NAME KIND inputs=N fanout=F [nqfanout=N]`) gains ` delay=<N>ps` iff
the stored value is nonzero — the pull-annotation precedent (012 D8:
present-only, so un-annotated workspaces list byte-identically). No new
section, no new status line; `unset-delay`/`set-delay X 0` remove the
annotation from the listing by making the value zero.

**Rationale**: one surface, present-only: the annotation is visible
exactly when it exists, and every pre-feature flow keeps its bytes.

**Alternatives considered**:
- Always printing `delay=0ps`: changes bytes on every existing golden
  and listing for no information; rejected.
- A dedicated `delays` verb/section: new vocabulary where the gate line
  already identifies the owner; rejected.

## D5 — Persistence: zero store work — the schema already carries it

**Decision**: no YAML store change and no checkpoint change. Verified
in the tree: save writes `delay_ps: <N>` on **every** gate row
(yaml_store_save.cpp:142 — the canonical 6-field comb shape), the row
checker requires the key (yaml_store_check.cpp:22/:40 — missing key is
a loud "needs 6 fields" error) and validates it as u64 (:184 —
negative/non-numeric is a loud "bad delay_ps"), replay stores it
(yaml_store_replay.cpp:48), and the world binary checkpoint round-trips
it (serialize.hpp:352/:791). The tieoff example fixture already carries
nonzero rows (`delay_ps: 1500`, `delay_ps: 250` — pinned by
test_logic_yaml.cpp:861-862), so no new example file is needed either:
there is no new construct to fixture. New tests pin the *verb-written*
round-trip (set → save → load → identical field → replay identical);
loader refusals for the key are already loud and stay green unchanged.

**Rationale**: the feature's persistence story is verification, not
authoring — the V2 hook reserved the wire format along with the field.
Re-opening the store would be churn with zero semantic gain.

**Alternatives considered**:
- A `delays:` top-level section: duplicate of per-gate data already in
  the canonical row; rejected.
- Retiring/re-bumping the workspace marker: nothing is reinterpreted;
  rejected.

## D6 — Optimizer: passes unchanged; one conditional O2 report note

**Decision**: no optimizer pass changes. The field's optimizer fate is
already documented and pinned (optimize.hpp:63: O0/O1 preserve delays
in place; O2 resets on reshape — the reset site is
optimize_o2_emit.hpp:195 `fresh.propagation_delay_ps = 0`). The single
addition is observability, mirroring the existing conditional
"interface changed" note (optimize.cpp:110): when level O2 runs and a
pre-scan of live gates found ≥1 nonzero delay, the report gains a line
naming the reset (e.g. `note: O2 reshaped the network; reset the
propagation delay on N gate(s) (docs/logic.md §12)`); zero-annotation
netlists print nothing extra (byte-identical). The pre-scan is a cold
slot-order loop in the verb; no engine change.

**Rationale**: spec US3 acceptance 5 — the reset must not be silent in
the level's reported contract. Making O2 *carry* delays instead would
change documented behavior beyond the requested scope (and the spec
pins the standing contract); the note closes the surprise without
touching semantics.

**Alternatives considered**:
- Carrying delays through the O2 reshape: a behavior change the spec
  explicitly declines (FR-009 "exactly as documented"); rejected, named
  as a possible follow-on with the verbatim-carry O2 emitter.
- A refusal when annotated (like TBUF netlists): delays are inert —
  refusing would block optimizing the other 90% of the netlist;
  rejected.

## D7 — Completion: gate names first, nothing for the value

**Decision**: both verbs complete `--help`/`-h` in flag position
(`complete_opts`) and gate names in the first positional via the
existing `collect_gate_names` (common.hpp:48). The value position
completes nothing — delay values are free-form, not an enumeration
(the `pull` verb completes `up`/`down` because those *are* its whole
value vocabulary; there is no delay vocabulary to offer, and suggesting
fragments like `ns` would compose malformed tokens).

**Rationale**: matches `add`'s numeric `--inputs` (completes nothing)
and avoids inventing a suggestion list that the parse would then have
to honor.

**Alternatives considered**:
- Completing unit suffixes (`ps ns us ms s min h`): suggestions that
  are not values; rejected (usage text shows examples instead).

## D8 — Tests, registry sync, docs duties

**Decision**:
- **New binary** `tests/test_logic_delay.cpp` (registered
  `add_executable` + `target_link_libraries` + `add_test`; `test_`
  prefix inherits the assertion policy): value goldens derived by
  hand decimal arithmetic (1500 → 1500; `1.5ns` → 1500; `250ps` → 250;
  `2us` → 2,000,000; `5ms` → 5,000,000,000; `0.001ns` → 1; `1.5h` →
  5,400,000,000,000,000; bare `18446744073709551615` → the exact u64
  max); refusal goldens (`-1`, `1.5ps`, `6000h`, bare `1.5`,
  `1500V`, `1e3ns`, unknown gate, stray positional, `unset-delay` on
  an un-annotated gate); verb lifecycle (set → list annotation → re-set
  last-write-wins → unset → list clean → `set 0` ≡ cleared);
  inertness (solve/truth/step outputs identical pre/post annotation,
  including on a fully-annotated netlist); optimizer (O0/O1 preserve;
  O2 resets with the note printed iff annotations existed;
  optimize-of-optimize idempotent); save/load/replay round-trip;
  removal (remove the gate, recycle the slot, annotation starts at 0);
  sequential-gate annotation (DFF legal, stored, inert).
- **Registry sync** in `tests/test_cli_args.cpp`: size assert :689
  (25 → 27), `verbs.size()` :816 (27), help-completion count :822
  (27), `want_s` :1762 gains `"set-delay"` in registry order (before
  `solve`), `want_u` :1766 gains `"unset-delay"` after `unpull`,
  `find("set-delay")/find("unset-delay")` blocks beside :698.
- **Docs**: README — verb sentence gains `set-delay`/`unset-delay`, a
  runnable example block, test list gains the binary, module table's
  `logic` row mentions delay annotation. `docs/logic.md` — §10 verb
  grammar gains both verbs (grammar + exactness rules); §12 Timing slot
  text ships the annotation verbs and keeps the evaluator as the named
  follow-on. `docs/tests/logic.md` — page for the new binary; catalog
  audit (`docs/audit_catalogs.py`) stays zero-missing/zero-ghost.
  Doxygen comments ship with every new public API
  (`parse_time_ps`, both engine fns, both verb specs) in the same
  change.
- **TUI/GUI**: no change (parity by construction — no logic form
  exists; GUI is a separate epic).

**Rationale**: the module's standing contracts — registry
`static_assert`s, README living-spec duties, Doxygen-with-API,
completion coverage — are enumerated once so the tasks phase can walk
them; the test content list pre-answers "what is the independent
reference" for every class.

## D9 — Performance: nothing to measure, and that is the claim

**Decision**: no benchmark change and no performance claim. The
evaluator never reads `propagation_delay_ps` (grep-verified: the only
readers are save/check/replay, the optimizer reset, and display); the
new code paths are the two verbs, the list annotation branch (taken
only for nonzero values), and the completion path — all cold. The
inertness test doubles as the no-hot-path proof: byte-identical
solve/truth outputs with every gate annotated.

**Rationale**: constitution IV forbids unmeasured optimization *and*
unmeasured claims; this feature makes neither. Recording the reasoning
pre-empts a review-time "where is the bench" question.

---

## Resolved NEEDS-CLARIFICATION ledger

None remain. The spec deferred: verb naming (D1 — the docs-reserved
`set-delay` plus the un- pair), value grammar and unit acceptance
(D2 — bare ps + owned-table suffixes, integer-exact), the clear path
(D1/D3 — `unset-delay` refusing on un-annotated gates), optimizer
treatment (D6 — documented behavior preserved, reset made visible),
and display/persistence surfaces (D4/D5). Technical Context carries no
NEEDS CLARIFICATION markers.

## Facts that bind the implementation (from research)

- `Gate::propagation_delay_ps` exists (types.hpp:333, u64, default 0);
  `add_gate` zeroes it (netlist.hpp:103) and the dead-slot/reset clear
  zeroes it (netlist.hpp:764) — FR-010's "no dangling value" is
  already true; tests pin it rather than build it.
- The YAML schema already owns the field: required key on every row
  (yaml_store_check.cpp:22 `kWant`, :40 missing-key error), u64
  validation (:184 "bad delay_ps"), save on every row
  (yaml_store_save.cpp:142), replay (yaml_store_replay.cpp:48);
  binary checkpoint column shipped (serialize.hpp:352/:791). The
  tieoff fixture already loads 1500/250 (test_logic_yaml.cpp:861).
- Verb seam template: `set_pull` (netlist.hpp:361) and `clear_pull`
  (netlist.hpp:393) — live-slot validation, `[[nodiscard]] Status`,
  Doxygen; verb-side mapping of engine failure to `usage_error` exit 2
  (unpull.cpp:35-37 "input has no pull set").
- Parse machinery to reuse: `split_number_suffix` (args.hpp:240),
  `parse_scaled_impl` (args.hpp:261) — read for the pattern, not
  called by the exact path; `parse_time` (args.hpp:402) stays the
  float path for other callers; the new `parse_time_ps` is integer
  arithmetic over the same `scales_for<Time>` table.
- Registry pins to sync: `LogicRegistry` order ends `... PullSpec,
  UnpullSpec, [insertion point] RemoveSpec, ...` (logic_registry.hpp
  :1155-1159); test_cli_args.cpp :689 (size), :816 (verbs.size()),
  :822 (help completion), :1762 `want_s`, :1766 `want_u`.
- `list` gate line renders `NAME KIND inputs=N fanout=F` (list.cpp:46,
  `nqfanout=` at :48) — the annotation joins the line, nonzero only.
- The conditional-note precedent is optimize.cpp:110 ("note: interface
  changed..."); the O2 reset site is optimize_o2_emit.hpp:195; the
  documented policy text is optimize.hpp:63 and docs/logic.md §12.
- Gate-name completion exists (`collect_gate_names`, common.hpp:48);
  completion option lists are local constexpr arrays (kCompleteKinds,
  add.cpp:93) — D7 adds none.
- `LogicSpec::kUsage`'s static session-verb list (commands.hpp:644-645)
  is hand-maintained and gains both verbs.
- Evaluation reads none of the field (grep inventory: types, save,
  check, replay, serialize, optimize reset, tests) — the inertness
  claim is verifiable at review time by the same grep plus the pinned
  test.
