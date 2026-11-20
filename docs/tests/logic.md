# Logic domain

The combinational and sequential logic slice: gate vocabulary, netlist
editing, topology and cycle rules, scalar and bit-parallel evaluation,
the optimizer, the hand-placed sequential stepper, the workspace store,
the schematic layout, and the `logic` CLI verbs. Sequential truth comes
from hand-derived tables (DFF/JK/T/DLATCH/SR), never from the
implementation, and failing edits leave the netlist untouched.

## test_logic

**What it checks**: combinational kind parsing and fan-in rules
(`AND`/`NAND`/`XOR`/`NOT`/`BUF`, bad fan-in refused with the netlist
untouched), slot recycling with never-reused per-kind counters
(`AND3`, never `AND1` again), case-insensitive pin parsing and resolution
errors, wire rules (occupied destination refused, unlimited fan-out,
output-to-output rejected), cycle rejection (self-loop, 2-cycle, closing
edge refused), remove-detach fan-out accounting, hand-derived
single-gate and 8-wide `eval_gate` goldens, scalar `solve` goldens
(single AND, NAND chain with compiled topo order, fan-out, 8-wide,
3-input parity cascade), dangling/X/Z arity refusals, `ignore`/`use`
interface narrowing with LOW tie-off, `reset`, `noexcept` hot paths,
bit-parallel `eval_block_word` column definitions vs the row convention
plus full bit-==scalar sweeps, word argument errors, and the O0/O1/O2
optimizer (parse, input purity, double-negation/BUF bypass, const-fold,
identity, ignored-cone survival, De Morgan fusion, NAND/NOR families
with tied-inverter minimization, multi-output order, const pool,
40-XNOR8 O2 overflow refused while O0/O1 pass).

**Purpose**: pins the combinational core every sequential extension
builds on: names, wiring, topology, evaluation, and optimization must be
bit-exact before flops mean anything.

**Guards against**: fan-in/name regressions, counter reuse, silent
topology or parity drift, optimizer equivalence breaks, and word-path
divergence from scalar evaluation.

**How to run**:

```sh
ctest --test-dir build -R "^logic$"
./build/test_logic
```

**Prerequisites**: standard build only.

**Boundaries**: sequential kinds, state, stepping, and CLI verbs live in
the `test_logic_seq_*` binaries; persistence lives in
`test_logic_yaml`; geometry lives in `test_logic_layout`.

## test_logic_layout

**What it checks**: pure layered geometry over the compiled topology:
empty canvas extents, single-gate column/row placement, chain columns
with straight Manhattan routes, fan-out stacking with deterministic
`+8` lane stagger, multi-input spread with diamond endpoints, deep-chain
columns, rebuild determinism, sequential anchors (role→row numerators,
clock-wedge and latch-notch marks, face letters, D/CLK anchor sharing,
Q-mid vs NQ-row-5 anchors, shared-clock routes, NQ-source routes,
dangling-clock anchors), and legibility (no box overlap, per-output
fan-out accounting behind the junction-dot rule).

**Purpose**: the schematic renderer consumes these coordinates; layout
must be deterministic pixels from the same netlist, not per-run art.

**Guards against**: column/stacking drift, overlapping trunks from lost
lane stagger, wrong sequential anchor rows, and fan-out miscounts behind
junction dots.

**How to run**:

```sh
ctest --test-dir build -R logic_layout
./build/test_logic_layout
```

**Prerequisites**: standard build only.

**Boundaries**: SDL/ImGui rasterization is not covered; engine math only,
verified headless.

## test_logic_yaml

**What it checks**: the versioned YAML workspace store: canonical v1
bytes, byte-identical save→load→save round-trips for ignore marks,
fan-in, stored (not honored) delays, flop inits, ties, clock domains,
and all five sequential kinds (T/JK/DLATCH/SR plus DFF), superset
tolerance (comments, quoting, block style), retired v1/v2 plus foreign
version refusal with atomic (untouched) failure, bad-`init` refusals,
`clocks:` derivation-equality refusals with `line N:` diagnostics,
malformed-file `line N:` diagnostics (multi-driver, unknown gate/pin,
bad kind/key, unknown section, combinational cycle), shared-net
round-trips (marks + repeated `to:` rows incl. zero-attachment marks,
save→load→save byte-identical with identical replay outputs; loud
rows for unknown `shared:` keys, duplicate marks, dead pins, cap
overflow, and marked-plus-tied pins), and shipped-example
goldens (`nand_pair`, `fanout`, `parity3`, `tieoff`,
`two_clock_counter`, `gated_async_counter`, `jk_t_latch_chain`,
`tied_demorgan` with a 64-row exhaustive check, `shared_bus` with
consensus/agree goldens).

**Purpose**: the workspace file is the durable artifact; bytes must be
canonical, reloads netlist-equal, and every refusal must name its line
and leave the bundle untouched.

**Guards against**: serialization drift, silently accepted foreign or
legacy files, clock-domain mismatch, and init/polarity loss across
save/load.

**How to run**:

```sh
ctest --test-dir build -R logic_yaml
./build/test_logic_yaml
```

**Prerequisites**: standard build only; example fixtures resolve via the
`WENGINE_LOGIC_EXAMPLE_DIR` compile definition (absolute source dir).

**Boundaries**: stepping semantics are not covered here (see the
`test_logic_seq_*` binaries); delay values persist but are not honored
by evaluation.

## test_logic_shared

**What it checks**: allowlisted multi-driver nets end to end — the
authoring gate (unmarked occupied refusal with the netlist untouched,
mark/wire/unshare lifecycle, loud cap, tie refusal, extras-aware cycle
rejection), span resolution through the shipped funnel (consensus,
conflict→X, Q+NQ self-contention, permutation invariance, empty→Z,
marked-single == unmarked-single, solve end-to-end with state untouched
on failure), loud contention counting (exactly one per contended
resolution, null-channel byte-identity, determinism, saves byte-identical
with/without history), named-edge removal and gate-sweep release
(promotion, compaction, polarity-aware fanout), and optimizer soundness
(O0/O1 freeze shared feeders with unchanged results and idempotence,
O2 refuses naming shared nets with the input untouched).

**Purpose**: pins the 011 seam (mark lifecycle, span assembly,
diagnostic channel) through the session verbs to the YAML round-trip,
so buses stay explicit, visible, and regression-safe.

**Guards against**: silent overwrites, implied multi-driver modes,
truncated spans, invisible contention, serialization of session
diagnostics, optimizer span corruption, and word-path
misrepresentation.

**How to run**:

```sh
ctest --test-dir build -R logic_shared
./build/test_logic_shared
```

**Prerequisites**: standard build only.

**Boundaries**: CLI verb goldens live in `test_cli_args`; the YAML
schema contract lives in `test_logic_yaml`; span unit tables live in
`test_logic`.

## test_logic_buffer

**What it checks**: enable-gated buffers plus per-net pulls end to
end — kind identity (byte 8, `TBUF` naming, exactly-2 fan-in,
never-reused counters, `T`-prefix fallthrough) and the independently
derived EN×D truth table; the netlist seam (full wire validation,
enable gating identical to an ordinary driver, released-Z loud failure
with the netlist uncorrupted, X/Z/dangling seams loud with state and
timing untouched); pull mapping (all-Z → idle under `up`/`down`, loud
without a pull; driven/contention/unknown spans inert under every
setting; permutation invariance forward and reverse); termination
(terminated inputs invisible to interface derivation and `poke`,
idle resolve with no column); tie/pull and ignore/pull disjointness
both directions with the netlist untouched; pull lifecycle
(unwire-to-dangling keeps the pull, remove/reset clear it);
exhaustive gated spans (every enabled/disabled × ordinary mix up to
the cap, every pull setting, order-invariant values and counts against
the independent reference); optimizer soundness (O0/O1 preserve gated
buses, freeze buffers and pulled sinks — collapse, drop, bypass, and
fusion guards — with idempotence; O2 refuses naming buffers/pulls with
the input untouched); word-path guards (`truth` refuses live buffers,
pull-only streams); YAML round-trip (tbuf rows, `pulls:` section,
canonical order, byte-identical replay); and TUI parity (executor vs
Shell byte-identical over the new vocabulary).

**Purpose**: pins the 012 seam (buffer eval, pull mapping,
termination, optimizer freeze) through the session verbs to the YAML
round-trip, so usable buses stay explicit, visible, and
regression-safe.

**Guards against**: silent float defaults, swallowed contention,
desynchronized flat-keyed pulls behind optimizer rewrites, word-path
misrepresentation of Z, and headless/TUI divergence.

**How to run**:

```sh
ctest --test-dir build -R logic_buffer
./build/test_logic_buffer
```

**Prerequisites**: standard build only.

**Boundaries**: kind-table pins live in `test_logic_seq_kinds`; CLI
verb goldens and registry pins live in `test_cli_args`; the YAML
schema contract (round-trip + refusals) lives in `test_logic_yaml`;
the `buffer_bus` example golden lives in `test_logic_yaml`
(`test_examples`).

## test_logic_delay

**What it checks**: propagation-delay annotation end to end —
value-exactness goldens hand-derived from the contract (bare ps,
`s sec ms us ns min h ps` suffixes via pure integer arithmetic to the
full u64 range, every refusal class: negative, sub-picosecond,
overflow, unknown/cross-dimension suffix, malformed grammar); the
engine seam (live-slot validation, zero-field clear refusal,
zero-as-legal-write, every-kind legality); verb lifecycle
(`set-delay` canonical-ps echo, `list` present-only annotation,
last-write-wins, zero-is-clean, usage refusals with state untouched,
un-annotated byte-identity); inertness (solve/step/truth/export-truth
byte-identical with every gate heavily annotated, including over
pulled/tied nets; the export schema differs exactly in `delay_ps`);
clear lifecycle (`unset-delay` success line, `no delay set` refusal,
unknown/stray loud, `set-delay X 0` equivalence, removal recycles
clean); save/load round-trip (verb-written value in `delay_ps`,
identical replay, cleared-file clean, pre-feature `tieoff.yaml`
with stored 1500/250 loads and solves as before); optimizer policy
(O0/O1 preserve in place with no note, O2 resets with the
conditional note iff ≥1 annotation existed, optimize-of-optimize
idempotent, zero-annotation runs silent); world binary checkpoint
round-trip of the u64 column; and TUI parity (executor vs Shell
byte-identical over the new vocabulary).

**Purpose**: pins the 013 annotation seam (exact parse, field
write/clear) through the session verbs to the YAML and binary
round-trips, so stored delays stay exact, visible, and inert.

**Guards against**: silent rounding of stored delays, sub-picosecond
truncation, evaluation paths reading the annotation, optimizer
silently dropping stored values, and headless/TUI divergence.

**How to run**:

```sh
ctest --test-dir build -R logic_delay
./build/test_logic_delay
```

**Prerequisites**: standard build only.

**Boundaries**: registry-size and completion pins live in
`test_cli_args` (25 → 26 → 27 with the verbs, `want_s`/`want_u`
vectors); the YAML schema contract (required key, u64 validation,
`tieoff` fixture pins) lives in `test_logic_yaml`.

## test_logic_seq_kinds

**What it checks**: sequential kind vocabulary: append-only ordinals
(`kDff = 16` … `kSrlatch = 20`), display names with `?` for unknown,
case-insensitive parsing of `DFF`/`DLATCH`/`JK`/`T`/`SRLATCH` with
near-miss refusals (`S`, `JK1`, `DFF2`, padded/embedded spaces),
name round-trips, fixed fan-ins (`DFF`/`DLATCH`/`T`/`SRLATCH` exactly 2,
`JK` exactly 3, combinational rules unchanged), `is_sequential_kind`
classification, per-kind never-reused counters, and named-pin resolution
(`D/CLK`, `J/K/CLK`, `T/CLK`, `S/R`, `Q/NQ` with polarity flags) with
kind-foreign names refused.

**Purpose**: kind bytes are append-only wire vocabulary; parsing, fan-in,
and names must be exact before state or stepping can trust them.

**Guards against**: ordinal renumbering, fan-in loosening, counter reuse,
and cross-kind pin aliasing.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_kinds
./build/test_logic_seq_kinds
```

**Prerequisites**: standard build only.

**Boundaries**: state pooling, cycle rules, and stepping live in the
sibling `test_logic_seq_*` binaries.

## test_logic_seq_state

**What it checks**: the sequential state pool beside the netlist: fresh
power-on (all Low, zero serials), attach (sequential slots get serials
in order, combinational gates ignored), re-init retaining Q and serial,
detach retiring serials with a monotonic `next_serial`, slot recycle
minting a fresh serial with power-on Low, reset starting a new lifetime,
and combinational `solve` bit-exactness with the pool present.

**Purpose**: serials are the stable identity for subscriptions, CSV
`sig_id` columns, Frame channels, and replay; slots recycle, serials
never do.

**Guards against**: serial reuse across despawn/respawn, stale-Q leaks
after recycle, and pool presence changing combinational evaluation.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_state
./build/test_logic_seq_state
```

**Prerequisites**: standard build only.

**Boundaries**: stepping tables and runner integration live in
`test_logic_seq_step` and `test_logic_seq_runner`.

## test_logic_seq_cycles

**What it checks**: cycle validation with the Q-boundary: empty and
acyclic nets validate, hand-poked combinational cycles and self-loops
fail, flop feedback loops (`DFF Q → AND → DFF D`, clock-driven and
DLATCH variants) validate through the public `wire()` API, reverse-order
(T Q/NQ fan-out first, feedback last) acceptance independent of wiring
order, live-but-uninvolved flops never shielding real combinational
cycles, unchanged combinational wire discipline, and
`has_sequential_gates` tracking add/remove lifetime.

**Purpose**: sequential feedback is legal only across the Q-boundary;
combinational cycles stay refused even with flops present.

**Guards against**: cycle-check holes that admit combinational loops, or
over-strict checks that reject legal flop feedback or order-dependent
YAML loads.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_cycles
./build/test_logic_seq_cycles
```

**Prerequisites**: standard build only.

**Boundaries**: stepping and observation goldens live in
`test_logic_seq_step` and `test_logic_seq_runner`.

## test_logic_seq_step

**What it checks**: scalar single-step sequential evaluation against
hand-derived tables: the 2-bit counter (00→01→10→11→00, two cycles
bit-exact), the shift register proving end-of-step parallel update, the
swap test proving snapshot isolation (no in-place cascade), DLATCH
level-transparent/frozen behavior with determinism replay, SR
set/reset/hold with S=R=1 refused loudly on direct, wired, and tied
paths, mixed edge/level nets sharing one stepper with selective-domain
holds and NQ==NOT Q, T and JK excitation tables through sync counters,
subset stepping holding non-members, and X/Z plus arity refusals leaving
committed Q untouched.

**Purpose**: the stepper is the sequential contract; each kind follows
its own resolver from the same old-Q vector under one reported edge.

**Guards against**: cascading (non-parallel) updates, level/edge rule
confusion, illegal-state commits, and wedged state after refusals.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_step
./build/test_logic_seq_step
```

**Prerequisites**: standard build only.

**Boundaries**: runner/driver selection and CSV observation live in
`test_logic_seq_runner`; CLI verbs live in the `cli_*` binaries.

## test_logic_seq_runner

**What it checks**: `SequentialSolver` plus the configured-run driver:
4-step counter, T-divider (÷2) and cascaded ÷4, latch hold, bad-`dt`
rejection with untouched state, 10-step determinism replay, driver
sequence/identical-replay/bad-config (zero steps, bad `dt`, wrong
solver, missing section, stimulus mismatch, non-binary stimulus)
rejections, empty-net runs, seeded vs default power-on inits,
multi-clock selective vs all-domains advance with member-list rejections,
derived-domain selection composed with inits, snapshot order/recycle
(serials stable, recycled slots mint fresh), `fill_snapshot` parity,
CSV reread (step/time/serial/Q exact plus malformed-log refusals),
mid-run replay parity, mid-run SR S=R=1 abort (index frozen, prior
entries intact, failure latches), and full-control DFF RST>SET>EN>D
priority parity.

**Purpose**: proves the engine path (solver + `Runner` + driver +
snapshot/CSV) agrees bit-for-bit with the hand-derived references.

**Guards against**: solver/driver divergence, selection bypassing clock
domains, mid-run illegality committing state, and snapshot/CSV drift.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_runner
./build/test_logic_seq_runner
```

**Prerequisites**: standard build only (writes a temp CSV under `/tmp`).

**Boundaries**: interactive `Shell` verbs live in the `cli_*` binaries;
YAML persistence lives in `test_logic_yaml`.

## test_logic_seq_controls

**What it checks**: the DFF full control set (opt-in 5-input fan-in,
RST/SET/EN named and numeric pins, bare-flop rejections), the 32-row
hand-derived RST>SET>EN>D truth table, no-advance async apply, EN hold,
zero-migration (bare nets behave as before), wired reset, `Shell`
session flows (reset/preset/stall/counter-seed/conflict-matrix/determinism
replay/wired-control/zero-live-controls), YAML control round-trip, and
`complete` plus per-kind help text for JK/T/SR sessions.

**Purpose**: async controls must compose with stepping, clock subsets,
and persistence without changing bare-flop behavior.

**Guards against**: priority inversions, control pins dropped as dead,
YAML control-shape loss, and session/help drift for new kinds.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_controls
./build/test_logic_seq_controls
```

**Prerequisites**: standard build only.

**Boundaries**: base stepping tables live in `test_logic_seq_step`;
per-verb CLI goldens live in the `cli_*` binaries.

## test_logic_seq_cli_pins

**What it checks**: named sequential pins (`D/CLK/Q/NQ`, `D/EN`)
case-insensitively both directions with wrong-kind and out-of-range
rejections, NQ negated-output wiring through the text API with
per-output fan-out accounting (`output_fanout` vs `nq_fanout`,
`driver_inv` polarity), NQ ignored-by-default interface counts with
`ignore`/`use` round-trips, NQ read-time values (power-on plus
post-step Q/NQ pairs), wired-NQ inversion through scalar and
bit-parallel solve, and a YAML round-trip carrying NQ sources, named
pins, and the `ignore_nq` key.

**Purpose**: NQ is read-time NOT Q with its own fan-out and ignore bit;
parse, wire, solve, and persistence must all agree on polarity.

**Guards against**: NQ/Q fan-out confusion, polarity loss across
save/load, and word-path polarity divergence from scalar solve.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_cli_pins
./build/test_logic_seq_cli_pins
```

**Prerequisites**: standard build only.

**Boundaries**: interactive `step`/`poke`/`peek` verbs live in the
sibling CLI binaries; clock-domain selection lives in
`test_logic_seq_runner`.

## test_logic_seq_cli_step

**What it checks**: the `step [DOMAIN] [N]` verb over session-built nets
driven through `Shell::execute` plus direct `LogicStepSpec::run` calls:
toggle advance with Q/NQ deltas and cycle counters, one-shot/direct
text parity, selective vs global domain advance, error changelessness
(negative counts, unknown clocks, reset-restarted counters, no-flop
runtime error), 100-step determinism replay, NQ ignored by default,
mixed-kind/JK-counter/T-divider sessions, DLATCH/SR latch sessions with
illegal-file workspace preservation, mixed edge/level sessions,
reload-run determinism, selective step after reload, shipped-example
sessions (`two_clock_counter`, `gated_async_counter`,
`jk_t_latch_chain`), and completion.

**Purpose**: the interactive stepper must report the same cycles and
deltas as the engine while leaving state untouched on every error.

**Guards against**: counter drift, domain leakage (implicit-domain
leaks into driven clocks), error-path state mutation, and one-shot vs
spec text divergence.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_cli_step
./build/test_logic_seq_cli_step
```

**Prerequisites**: standard build only; example fixtures resolve via
`WENGINE_LOGIC_EXAMPLE_DIR`.

**Boundaries**: poke/peek verbs have their own binaries; YAML store
mechanics live in `test_logic_yaml`.

## test_logic_seq_cli_poke

**What it checks**: the `poke PIN=0|1|release` verb: drive/release/list
with numeric-alias overwrite messaging, one-shot/direct text parity,
`--from` multi-drive plus `--clear` census, and changeless errors
(unknown pin, output pin, bad value, combinational named pin, wired pin,
duplicate `--from`, ignored pin), ending with poke-into-step
composition (a held poke latches on the next edge).

**Purpose**: pokes are the session stimulus map; every rejection must
leave the held map — and the next step — unchanged.

**Guards against**: poking outputs/wired/ignored pins, duplicate-drive
acceptance, and error paths corrupting held stimulus.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_cli_poke
./build/test_logic_seq_cli_poke
```

**Prerequisites**: standard build only.

**Boundaries**: stepping output lives in `test_logic_seq_cli_step`;
stored inits (YAML) live in `test_logic_yaml`.

## test_logic_seq_cli_peek

**What it checks**: the `peek [GATE]` verb at all three scopes:
empty-workspace `no flops` shape, reset-state `Q=0 NQ=1 (serial 0)`
readback, slot-ordered full listing with `flops:` census, per-block
scope filtering, one-shot/direct text parity, serial stability across
steps (values flip, serials do not), sequential-only completion
(`DFF1` offered, `AND1` refused), unknown-gate/extra-token/combinational
errors, and `load` applying stored inits (mixed values visible pre-step,
fresh serials per load).

**Purpose**: peek is the session readback; scopes, ordering, and serial
stability must match the pool contract exactly.

**Guards against**: scope leakage, serial churn across steps, and loaded
inits not reaching the live pool.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_cli_peek
./build/test_logic_seq_cli_peek
```

**Prerequisites**: standard build only (writes a temp YAML under the
system temp dir).

**Boundaries**: stepping and stimulus live in the sibling CLI binaries.

## test_logic_seq_symbol

**What it checks**: the shared sequential symbol model: every
`seq_*` adapter query (`seq_input_row`, `is_clock_pin`,
`is_latch_enable_pin`, `seq_face`) matches the single-owner `KindPin`
table row it claims, DFF box geometry (insets, mid, face, wedge mark,
per-input stub rows equal to anchor queries, Q/NQ anchor points, NQ
visibility and dot predicates from fan-out), the mark vocabulary
(DLATCH notch, JK/T wedges, SR none), and dot derivation from the model
(Q dot on fan-out, never on combinational rows).

**Purpose**: one model feeds every raster adapter; the paint seam must
read the same table as the engine, never a copy.

**Guards against**: symbol/table drift, wrong wedge/notch marks, and
junction-dot rules diverging from fan-out accounting.

**How to run**:

```sh
ctest --test-dir build -R logic_seq_symbol
./build/test_logic_seq_symbol
```

**Prerequisites**: standard build only.

**Boundaries**: pixel placement lives in `test_logic_layout`; SDL/ImGui
rasterization is verified manually.

## test_logic_barrier_fixture.hpp

**What it checks**: the shared flop/latch opaque-barrier fixture:
hand-placed sequential builders (closed 2-bit counter, counter-plus-glue
with DLATCH, acyclic pipeline, JK/NQ polarity trap, full-control 5-input
DFF), display-name resolution, the strict vs shape-only sequential-set
oracles (kind, pin count, flags, wired shape, Q/NQ polarity, driver
names where valid), per-output fan-out census, and fixed-step run
parity with stimulus projection across optimizer rebuilds.

**Purpose**: the optimizer must treat sequential gates as opaque: it may
rewrite combinational glue but never rename, drop, rewire, or
re-time a flop or latch — this fixture is the shared proof both sides
include.

**Guards against**: optimizer passes deleting or renaming sequential
gates, polarity/fan-out loss across rebuilds, and exact-simplify traps
that read flop outputs as constants.

**How to run**:

```sh
ctest --test-dir build -R "^logic$"
./build/test_logic
```

**Prerequisites**: standard build only; header-only, included by the
optimizer barrier tests (not a standalone binary, so no own CTest
name — run the including binary).

**Boundaries**: rendering and persistence are not covered; functional
equivalence beyond 10-step parity belongs to the runner goldens.

## logic_pty_smoke.py

**What it checks**: the live logic session on a real PTY: `log`+Tab
completes to `logic `, bare `logic` swaps the prompt to `[logic]> `,
session `help` lists the module verbs, `b`+Tab completes to `back `,
`back` leaves with the leave banner, top-level `back` is a loud unknown
command, and `exit` quits with status 0.

**Purpose**: raw-mode line editing and scoped completion only engage on
a TTY; pipes cannot prove the prompt swap or the Tab vocabulary switch.

**Guards against**: prompt-swap regressions, session-scoped completion
leaking top-level verbs, and history setup breaking PTY launch.

**How to run**:

```sh
ctest --test-dir build -R logic_pty_smoke
python3 tests/logic_pty_smoke.py --bin ./build/PhysicsSimApp
```

**Prerequisites**: the built `PhysicsSimApp` plus a real PTY via
`python3 pty.fork` (no display needed); isolated history via a temp
`XDG_CACHE_HOME`. Without a PTY-capable host the CTest fails at spawn
rather than silently skipping.

**Boundaries**: full verb goldens live in the `test_logic_seq_cli_*`
binaries; this script proves the session shell, not solver numerics.
