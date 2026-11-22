# Research: Scale-Profiling Baseline to One Million Gates (015)

Phase 0 consolidation. Facts below are measured/read from the current
tree (read-only research; compiler-probe sizes verified with GCC 13
under `/tmp/opencode`). Every decision carries its rationale and the
alternatives it beat.

## Resolutions of Technical Context unknowns

Two items were genuinely open entering planning; both are resolved
here:

1. **Enablement architecture** (spec Assumption 1 deferred it to
   planning): resolved as capacity-parameterized templating with pinned
   production aliases — **D1/D4** below.
2. **Committed baseline numbers vs. the house habit of never committing
   machine-dependent numbers** (`docs/benchmarks/index.md` states pages
   never commit them): resolved as a sanctioned, explicitly-worded
   carve-out for this one page — **D7** below.

## Measured facts the decisions stand on

- **Current shape** (compiler-verified): `sizeof(Gate)=40`,
  `sizeof(Netlist)=159,840 B`; `kMaxGates=1024` (types.hpp:217),
  `kMaxInputs=8`, `kMaxSharedExtra=3`, `kNoDriver=0xFFFF`
  (types.hpp:292). Slot ids are `std::uint16_t` everywhere — a **hard
  65,534-slot ceiling inside `Netlist` itself** (drivers[], tie_rep,
  topo_order), independent of scratch threading.
- **Scale shape** (extrapolated arithmetic, to be confirmed by the
  harness's own footprint report): `NetlistT<1M>` with u32 slots ≈
  229 MiB; `SeqStateT<1M>` ≈ 10 MB; `TimingScratch` gate-scaled portion
  ≈ 67 MB (untouched in this feature); per-solve scratch ≈ 9 MB;
  optimize-core scratch (pend 16 B × 8M flats) ≈ 128 MB.
- **Touchpoint inventory**: ≈366 `std::uint16_t` slot mentions across 26
  logic headers, all classified pure-engine (capacity-generic-able).
  Monomorphic-safe consumers verified: YAML store TUs, world
  `serialize.hpp` (checkpoint v6 is fixed-stride over `kMaxGates`),
  `world/state.hpp`, `solvers/sequential.hpp`, `configured_run.hpp`,
  `core/run_config.hpp`, all 25 CLI verb TUs, `logic_draw_*` — and
  **zero `wengine/logic` includes in `apps/gui/` and `src/tui/`**
  (verified by grep).
- **Production call-site counts** (wrapper-compatibility headroom):
  `topo::rebuild` 22 sites, `topo::validate` 7, `topo::reaches` 3,
  `solve` 6, `eval_block_word` 5, `compile_comb_order` 1,
  `seq_step_filtered` 3, `optimize` 1, `layout_schematic` 2.
  `test_logic.cpp:808/816` `static_assert`s `noexcept` on
  rebuild/solve/eval_block_word — wrappers must preserve it.
- **Suspect confirmation** (file:line + worst-case class; the sweep
  measures actual motif costs, which may sit below worst case —
  reported honestly, see D6):

  | # | Suspect | Site | Worst case |
  |---|---|---|---|
  | 1 | Kahn per-pop full-pool successor sweep | topo.hpp:159–193 (`drain_successors`), per pop :284/:340 | O(V²·I) per compile; fires on every mutation (dirty flag) **and every sequential step** (seq_step validates) |
  | 2 | Per-add free-slot scan | netlist.hpp:81–86 | O(V²) bulk build |
  | 3 | Per-remove full-pool sweep | netlist.hpp:185–204 | O(V²·I) bulk teardown |
  | 4 | Per-wire reachability probe | netlist.hpp:504 → topo.hpp:446–496 (each popped node scans the pool :459) | O(V²·I) per wire worst; bounded by reachable-set in practice |
  | 5 | All-pairs structural dedup | optimize_o2_dedup.hpp:84–169 (+ `rewire_users` O(V·I) per merge) | O(V²·I) key compares, O(V³·I) with merges |
  | 6 | Full-capacity init per op | eval.hpp:141–149 (resolved/gate_out), eval.hpp:234–235 (`in_pos` rebuilt **per word call**), block_interface.hpp:350–352, seq_step zero loops, `block_input/output_count` scans | O(Cap·I) per solve/word/step — **capacity-proportional, not live-proportional** |
  | + | Secondary full-pool scans (tie ops, timing seeds, `recount_fanout`, exact-solve cones, layout) | see research inventory 7a–7i | O(V·I)–O(V²·I) each, off the headline paths |

- **Scratch-ownership precedent**: `eval_block_word`'s caller-owned
  `gate_scratch` span with loud size `Status` (eval.hpp:227–229;
  `kBitScratchWords = kMaxGates`, block_interface.hpp:543); `kahn_pure`
  already takes caller `order_out`/`level_out`; `drain_successors`
  already takes caller `indeg`/`queue`; `SolveDiag`/`TimingScratch`/
  `TieSnapshot` are caller-owned. The pattern is established; the
  templating rides it (D3).
- **Bench/docs conventions**: bench targets are 2-line CMake entries
  linking `wengine_core` only, no warning flags, never the assertion
  policy (CMakeLists L1062–1109); `bench` preset = Release `-O3
  -DNDEBUG` in `build-bench`; `bench_signal_scale` supplies the
  scale-bench idiom (clamped positional args, argc-seeded xorshift,
  warmup, labeled scenario lines, checksums, `do_not_optimize`);
  `docs/audit_catalogs.py` requires a literal `## bench_<stem>`
  heading in `docs/benchmarks/*.md` (CS3 five-field schema) and exits 1
  on missing/ghost; README duty lines L54–64, L839–855, L992; new test
  needs a CS1 six-field entry in `docs/tests/logic.md`.

## D1 — Enablement architecture: templated capacity + pinned production aliases

**Decision**: generalize the engine domain to
`template <std::size_t kCap>` with a dependent slot type
(`slot_t` = u16 while kCap fits 16 bits, else u32), and keep production
on aliases (`using Netlist = NetlistT<1024>`, `PinRef`, `kMaxGates`)
whose instantiation is **type-identical** to today — pinned by
`static_assert`s (sizes/sentinels) and the unchanged golden suite. The
scale harness instantiates the same source at ≥1M.

**Rationale**: FR-002 forbids measuring a shadow implementation, and
the u16 slot ceiling (65,534) makes "measure the shipping 1024 binary
bigger" impossible; templating is the only route where the measured
code *is* the shipped source. docs/logic.md §12 pins "`Netlist`
never reshapes" — the alias keeps that promise literally. FR-009's
byte-identical duty is discharged structurally (same instantiation),
not by hoping tests catch drift. Serialization (YAML v1, checkpoint v6
fixed-stride), CLI, world/solvers, GUI/TUI all keep compiling against
the aliases with zero source churn — verified by the consumer
inventory.

**Alternatives considered**: (a) engine-wide lift of `kMaxGates` to
1M — rejected: reshapes `Netlist` against the §12 pin, forces a
checkpoint/schema break, and ships a CLI whose every mutation/compile
is the O(N²) footgun this baseline exists to kill (user-facing 1M
belongs after the kill list, D10); (b) macro/compile-definition capacity
knob — rejected: same ripple with none of the type safety, two binaries
with no alias guarantee; (c) heap/vector pool "grow like signals" —
rejected: signal pools are per-slot independent; logic is relational
(whole-pool compile, cycle checks, slot-order determinism pins,
zero-init invariant, capacity-fixed serialization) — a vector pool
silently changes footprint/iteration/serialization guarantees the
logic contracts rely on; (d) shadow harness netlist type — rejected
outright by FR-002.

**Fidelity caveat (stated in the report, never hidden)**: the harness
measures the u32 instantiation; production ships the u16 alias. Same
source, same algorithms; width affects generated loads, not complexity
class. The report's Interpretation field says so, and the alias anchor
points (below) keep one foot in the shipped instantiation.

## D2 — Dependent widths and capacity-derived sentinels

**Decision**: at the templated core, the slot type, topo levels,
fanout counters (`output_fanout`/`nq_fanout` — "unlimited" per
types.hpp:15, so they must not cap at 65,535 when gates can drive
millions), `live_count`, `topo_size`, and block-input ordinals all
take the capacity-dependent width; sentinels derive from capacity
(`kNoDriver`, `kNoLeader = kCap*kMaxInputs`, `kBitScratchWords = kCap`)
with alias values `static_assert`-pinned to today's exact constants
(0xFFFF, 8192, 1024). `kMaxInputs=8` and `kMaxSharedExtra=3` stay
fixed (motif-bounded, not capacity-coupled).

**Rationale**: every one of these overflows 16 bits at 1M (flat
indexes reach 8×10⁶, fanout can legally exceed 65,535 on a shared
clock), and the serialized formats pin the *alias* constants —
capacity-derived sentinels with pinned alias values reconcile both.

**Alternatives**: keeping u16 with loud overflow caps at 65,534 —
rejected: caps the feature's own title target; widening only slots but
not levels/fanouts — rejected: each was verified to overflow at 1M.

## D3 — Scratch ownership: span-taking cores, signature-identical wrappers

**Decision**: templated cores take caller-owned scratch
(`std::span`/pointer+cap) with loud size-`Status` validation before
any work — the `eval_block_word` pattern (eval.hpp:227–229). The
biggest fixed stack objects found (tie_set `survivors[kMaxGates*
kMaxInputs]` 64 KB→64 MB; optimize `pend[...]` 128 KB→128 MB; kahn
`indeg/queue`; eval `resolved/gate_out/in_pos`; seq_step arrays;
optimize's by-value `Netlist` copies 2×160 KB→2×229 MB) all move to
caller buffers/out-params in the cores. Alias-facing wrappers keep
today's exact signatures and `noexcept` (pinned by test_logic:808/816)
with internal fixed stack scratch, so all 22/7/3/6/5… production call
sites and every test compile unchanged.

**Rationale**: the pattern is already established in-file
(`drain_successors` takes indeg/queue; `kahn_pure` takes
order/levels); it preserves the engine's allocation-free,
exception-free hot-path rules at every capacity; and
wrapper-compatibility keeps FR-009's byte-identical duty structural.

**Alternatives**: engine-heap scratch inside the cores — rejected:
breaks the no-allocation hot-path discipline and the header-only
owning conventions; leaving scratch internal per capacity — rejected:
1M-scale frames (9–128 MB) cannot live on any stack.

## D4 — Templating scope map

**Decision**: template the stage-path headers: `types.hpp` (root),
`shared_net.hpp`, `tie_set.hpp`, `block_interface.hpp`, `topo.hpp`,
`pins.hpp`, `netlist.hpp`, `eval.hpp`, `seq_state.hpp`, `seq_step.hpp`,
and the optimize family (`optimize_barrier/passes/exact/o2_*`).
Leave monomorphic on the alias: `timing.hpp` (014's wheel — CLI
invocation-only, not a sweep stage; its `TimingScratch` is the
migration *precedent*), `layout.hpp` (draw stage), `seq_config.hpp`
(embeds the alias `Netlist`; engine run path stays 1024),
`yaml_store.hpp` + store TUs, `clock_domains.hpp` (vector/string
typed already), `four_state.hpp`/`seq_symbol.hpp`/`optimize_types.hpp`
(capacity-free). Text adapters (`wire_text`, `tie_text`, parse_pin
paths) stay alias-monomorphic; the harness builds by slot directly.

**Rationale**: the sweep's stage set (spec FR-003) plus the FR-005
suspect verdicts define exactly which code must scale; everything else
is measurement-irrelevant and carries serialization/CLI pinning. The
verified acyclic include graph (types ← shared_net ← tie_set ←
{block_interface, topo} ← pins ← netlist ← eval ← …) means templating
adds no include edges and no ODR risk.

**Alternatives**: template everything including timing/layout —
rejected: churn with no measured need (against constitution V);
template nothing and lift constants — rejected: see D1(a).

## D5 — Harness: `bench_logic_scale` shape

**Decision**: one new `benchmarks/bench_logic_scale.cpp` following the
`bench_signal_scale` idiom: positional args with clamped/zero-fallback
defaults, argc-seeded xorshift motif builders (bit-identical nets per
args), warmup separated from steady-state timing, `do_not_optimize`,
per-stage labeled output lines (`bench_logic_scale <stage>: …`) with
checksums and peak-footprint report, exit 1 on setup/run failure.
CMake: the 2-line bench block linking `wengine_core` only, inside
`if(BUILD_BENCHMARKS)`, with a one-line comment naming the measured
risks. Ladder: alias anchor points (≤1024, in the shipped u16
instantiation) + the scale instantiation ladder to ≥1M. Default
(no-args) = quick sub-ladder (minutes); the committed full-ladder
invocation (including the 1M point) is documented verbatim in the
report — re-running it reproduces every committed entry.

**Rationale**: FR-002/FR-004 need one honest, reproducible instrument;
the house idiom already exists; budgets and the documented full
invocation reconcile the 1M point's wall cost (D11) with FR-004's
reproduction duty.

**Alternatives**: a ctest-sized sweep — rejected: ctest must stay fast
and benchmarks must never link the assertion policy; per-size
separate binaries — rejected: one catalog entry, one re-run procedure.

## D6 — Stage set, motifs, and honest complexity wording

**Decision**: stages timed: growth-add, growth-wire, compile-rebuild
(forced dirty), steady scalar solve, steady word eval, shared-net
scalar solve, sequential step, optimizer passes (O0 pipeline; dedup
pass timed in isolation for suspect #5's verdict — the pass is a
header function callable directly on a prepared net), remove burst,
reset. That is spec FR-003's seven stages **plus remove/reset**, which
FR-005's suspect list (per-remove sweep; reset full-pool clears)
requires to have a measured home. Motifs: plain layered (primary),
wide, deep-chain, and the mixed motif (shared nets + pulls + seq
elements + delay annotations) so 011–014 features stay measurable;
motifs keep reachable sets bounded (forward-layer construction, the
`bench_logic` precedent) so per-wire probes measure the realistic
path, not a pathological one. The report states both the worst-case
class (from this research table) and the measured actual — a suspect
may measure below its worst case; verdicts cite the measured ratios,
never the asymptotic bound alone. Suspect #6's capacity-vs-live
attribution is measurable via the flat-within-instantiation curve plus
the alias anchor points (1024-cap instantiation at the same live
count).

**Rationale**: FR-005 demands verdicts for all six named suspects;
each needs a timed home and an honest reading.

**Alternatives**: measuring only whole-pipeline walls — rejected:
no attribution; only worst-case citations — rejected: extrapolation
by another name (FR-006).

## D7 — Baseline report home and the committed-numbers carve-out

**Decision**: new page `docs/benchmarks/logic-scale.md` with the CS3
five fields (`## bench_logic_scale`: Measured path / Why it matters /
How to run / Interpretation / Pitfalls) **plus** the committed
Baseline section: environment pin (host class, GCC 13.3, `bench`
preset flags echoed at configure time), variance policy (D8), the
per-stage per-size tables, suspect verdicts with size-doubling
ratios, the 1M-end ranking split mutation/compile/eval-side, the
operational measured-hotspot gate text (FR-007), the verbatim
re-run procedure (D5's full-ladder invocation), and the D1 fidelity
note. `docs/benchmarks/index.md` gets the Pages bullet + toctree
entry **and a one-sentence wording carve-out** (its current "never
commit numbers" sentence gains "except the pinned-environment
baseline page"). Other hosts re-run and record beside, never edit in
place (spec assumption).

**Rationale**: FR-004 makes committed numbers mandatory; the house
habit exists because numbers are machine-dependent — the carve-out
resolves the conflict explicitly instead of silently. The audit
(`audit_catalogs.py`) only checks anchor presence, so the schema
stays clean; README duty lines enumerated by the research (L54–64
run list, L839–855 catalog paragraph, L992 brace list) update in the
same change.

**Alternatives**: numbers in `specs/` instead of `docs/` — rejected:
the bench documentation suite is where bench meaning lives and the
spec's FR-004 names it; a CSV artifact beside the bench — rejected
as the primary (no prose interpretation = no gate text), optional
follow-on if tooling ever wants it.

## D8 — Variance and repetition policy

**Decision**: steady-state stages (solve/word/shared/step): R≥5
repetitions, report **median** plus observed min; one-shot mutation
stages (growth bursts, forced rebuilds, remove/reset, dedup pass):
single measurement per size with the observed spread across the
ladder noted in the report; every table row carries its sample count;
budget states (measured / unmeasured-at-size within budget) are
explicit columns.

**Rationale**: FR-004's reproduction claim needs a stated policy
applied consistently; medians resist scheduler noise on WSL2; one-shot
stages cannot be "repeated" without changing what is measured
(bulk-build time includes its own warmup by definition).

**Alternatives**: minimum-of-N everywhere — rejected: hides jitter
without stating it; fixed wall-clock budgets instead of counts —
rejected: less reproducible across hosts.

## D9 — ctest/bench split and test sizing arithmetic

**Decision**: `tests/test_logic_scale.cpp` (links `wengine_core`,
inherits the assertion policy via the `test_` prefix) pins: alias
identity `static_assert`s (sizes, sentinels, type spellings), scale
instantiation scalar/word parity, prefix-slice agreement,
bit-identical repeat solves, and refusal parity (overflow, cycle,
single-driver) at a **default ctest size of 65,536 gates** with an
env-var override for manual larger runs. The size is chosen
deliberately: the u16 alias addresses 1,024 slots / 8,192 flats, so
65,536 slots (524,288 flats) is far past anything the shipped
instantiation could ever hold — the test proves the widened path
actually carries more than the alias can, not just that the template
compiles. Feasibility arithmetic at 65,536: bulk-add ≈ 6.6×10⁸ slot
probes ≈ ~1 s; one forced compile ≈ 4.3×10⁹ pool visits ≈ ~2–5 s;
steady solves linear — one suite member at a few seconds total is
acceptable. The ≥1M reach itself is the bench's deliverable (SC-001),
run under the committed invocation; the test proves *instantiation
correctness*, the bench proves the *reach*.

**Rationale for the split**: FR-010 (independent parity) is a
correctness duty → ctest with live assertions; SC-001 (1M reach) is
a measurement deliverable → the bench's committed run.

**Alternatives**: full-1M ctest — rejected: O(N²) stages make it
tens of minutes, breaking suite practicality; no scale test at all —
rejected: FR-010 demands asserted parity, not just checksummed
bench output.

## D10 — User-facing capacity policy: nothing user-visible moves

**Decision**: CLI verbs, YAML workspace, checkpoints, `sim run
--solver logic`, GUI/TUI keep today's exact 1024-loud behavior. No
verb gains capacity options. The capacity lift to user surfaces is
the named follow-on, gated on the kill list this baseline produces.

**Rationale**: resolves the spec's Assumption 2 with the safe
default; sequencing honors the task's own "before optimizing" (a
user-facing million-gate CLI whose compile is minutes-per-mutation
is a footgun the baseline would itself indict); FR-009's
extension-duty clause is therefore not triggered — only bench/docs
duties apply. Verified: no GUI/TUI logic includes exist to move.

**Alternatives**: lift CLI caps along with the template — rejected:
premature, unmeasured UX; per-verb capacity flags — rejected: new
vocabulary with no user story.

## D11 — Reach feasibility and budget arithmetic (the honest costs)

**Decision**: the committed 1M point carries these expected-cost
bounds as its budget column (to be replaced by measurements):
bulk-add ≈ 10¹²/2 slot probes (minutes); one forced compile ≈
8×10¹² pool visits (tens of minutes — suspect #1's own verdict);
steady solve/word ≈ 10⁷–10⁸ (sub-second); one sequential step
triggers a validate (compile-class, suspect #1 again) — so the
mixed-motif ladder steps at small sizes and the 1M mixed point runs
construct + solve + word + one step within an explicitly budgeted
window; dedup/O2 at 1M is expected to blow its budget and be
recorded unmeasured-at-size (a loud gap is baseline evidence, per
spec FR-006/edge cases).

**Rationale**: budgets must be set from arithmetic before the first
run so the harness never hangs unbounded; the arithmetic itself is
the pre-measurement form of the suspects — replaced by numbers, not
kept beside them.

**Alternatives**: no budgets (run everything to completion) —
rejected: unbounded wall time, spec mandates loud budgets; skipping
the 1M mixed point — rejected: FR-001 requires stepped mixed nets
at ≥1M on the measured path.

## D12 — Report content model and the operational gate

**Decision**: the page's Baseline section schema (contracts/
baseline-report.md) fixes: environment block, policy block, one table
per stage (sizes × median/min/footprint/budget-state), the verdict
table (suspect × class × ratios × verdict), the ranking, the gate
paragraph ("no SIMD/threading/GPU/structural-rework proposal for the
logic domain proceeds without citing a number from this page; a
proposal naming no measured hotspot is out of order until
re-baselined"), the re-run procedure, and the fidelity note (u32
instantiation, alias anchors).

**Rationale**: FR-007 makes the gate operational only if its citation
target is unambiguous; a fixed schema makes follow-on tasks cite
rows, not vibes.

**Alternatives**: prose-only findings — rejected: not citable per
stage/size; numbers in the spec dir — rejected: see D7.
