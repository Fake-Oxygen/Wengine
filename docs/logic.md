# Logic-gate module (`logic`) — design guidelines

Status: draft scaffold (2026-10-23). No behavior wired yet; see Plane epic
`logic-gates` (parent) + LOGIC-* children for build order.

## 1. What ships in v1

- In-process `logic` module: add/list/wire/reset logic gates, solve the
  combinational block, print/stream its truth table, optimize it
  (De Morgan → NAND-only + constant folding + dead-gate elimination),
  draw it (interactive ImGui canvas with zoom/pan, `--out bmp` file
  export),
  save/restore the workspace as versioned YAML, and step sequential
  logic: `add DFF`/`DLATCH` with named pins, `poke` stimulus,
  `step` clock advances, `peek` flop readback (all headless).
- Generic reusable sub-shell framework (`cli/subshell.hpp`): `logic` is
  the first consumer, future modules reuse it unchanged.
- CLI-only in v1. Every feature is reachable from the CLI (REPL +
  headless one-shot). GUI arrives later against the same engine headers.

## 2. Architecture

```text
include/wengine/logic/types.hpp    GateKind, LogicValue, Netlist (SoA, header-only)
include/wengine/logic/eval.hpp     topological order + bit-parallel combinational eval
include/wengine/logic/optimize.hpp De Morgan / NAND-only / fold / DCE (pure, returns new Netlist)
include/wengine/logic/truth.hpp    bounded truth-table streaming (never unbounded print)
src/logic/yaml_store.cpp           versioned workspace save/load (the ONE TU allowed
                                   to know yaml-cpp exists; engine headers stay yaml-free)
include/wengine/cli/subshell.hpp   generic module sub-shell (prompt, dispatch, scoped completion)
src/cli/commands/logic.cpp         LogicSpec + module registry wiring (thin adapter, owns iostream)
```

Dependency direction (extends AGENTS.md §4, never reversed):

```text
logic -> units? (no: pure bits, no dimensions), core/status (Status/Result only)
cli   -> logic, core   (adapter only)
```

Engine stays header-only (`INTERFACE`, like solvers). Only the raster
TUs (`src/cli/logic_draw_sdl.cpp` for the headless BMP export,
`src/cli/logic_draw_imgui.cpp` for the interactive canvas) and the YAML
file I/O TU are `.cpp` — same justification as `src/link/*.cpp`
(syscalls / display calls cannot live in headers). Layout math (layered
columns + Manhattan wires) is a pure header function with a unit test;
both raster paths consume it without changing it.

## 3. CLI sub-shell contract (generic, reused by future modules)

- Top-level `logic` command has **two modes**:
  - Bare `logic` in the REPL → enters the module session: prompt becomes
    `[logic]> `, only the module registry completes, `help` lists only
    module verbs. `back` (plus `exit`/`quit` inside the session) leaves
    the session; only the outer `exit` quits the app.
  - `logic <verb> [args...]` headless / piped → one-shot dispatch to the
    **same** module registry, no session entered. This keeps scripts,
    pipes, `--tui` parity, and tests working without a TTY.
- Implementation: `cli/subshell.hpp` owns a `SubShell` runner
  (prompt string + `span<const CommandDef>` + session state pointer).
  `Shell` owns a session stack; module entry is handled in `Shell`
  (like `help`/`exit` builtins), **not** inside `Spec::run`, because
  `run(ArgCursor&, ostream&)` has no `istream` access. `Spec::run` with
  empty args returns a dedicated `kEnterSubShell` request code the Shell
  interprets — no signature change to the `Registry` concept.
- Completion scoping: inside the session, `complete_line` is called with
  the **module** registry, never `CliRegistry`. Pinned by a test
  (`complete_line("[logic] ...", module_registry)` == session behavior).
- TUI: no TUI changes needed — one-shot `logic <verb>` lines execute via
  the existing `Shell::execute` path byte-identically (same rule as all
  commands). Interactive session entry under `--tui` is rejected loudly
  until a TUI session-pane task is scheduled (do not block v1 on it).
- Determinism: session vs one-shot dispatch is a pure routing choice;
  engine results are identical either way (same Netlist → same eval).

## 4. Netlist model (engine)

- `GateKind`: `AND OR NAND NOR XOR XNOR NOT BUF` (append-only enum;
  `XNOR` = `NOT(XOR)` internally, `BUF` needed as an optimizer output)
  plus sequential `DFF` (=16, edge-triggered D+CLK), `DLATCH`
  (=17, level-sensitive D+EN: transparent/frozen, see below), `JK`
  (=18, edge-triggered J+K+CLK), `T` (=19, edge-triggered T+CLK) and
  `SRLATCH` (=20, level-sensitive S+R: set/reset/hold, S=R=1 illegal).
  `add DFF`/`add DLATCH`/`add JK`/`add T`/`add SRLATCH` create them with
  per-kind never-reused counters, same as combinational gates.
  `add DFF` makes a bare 2-input flop by default; `add DFF --inputs 5`
  makes the full control set (D/CLK/RST/SET/EN: async reset/set + clock
  enable) for counters/FSMs; `add JK` makes a fixed 3-input flop,
  `add T` and `add SRLATCH` fixed 2-input latches/flops.
- `kMaxInputs = 8`. `NOT`/`BUF` take exactly 1 input; `DLATCH`
  takes exactly 2; `DFF` takes 2 or 5; `T` takes exactly 2; `JK` takes
  exactly 3; `SRLATCH` takes exactly 2; the rest 2..8.
- Identity: per-kind monotonic counters, never reused
  (`AND1, AND2, …`, `NOT5, …`), slots recycle — same convention as
  signal serials (AGENTS.md §8). Display names use kind+counter;
  internals use dense slot indices.
- Pins: `<NAME>.Y` (output) or `<NAME>.<1..k>` (input), e.g. `AND2.Y`,
  `NOR1.1`. Parsing is a pure `parse_pin(text) -> Result<PinRef>`.
  Sequential gates additionally accept named pins (case-insensitive):
  `DFF1.D`, `DFF1.CLK`, `DFF1.Q`, `DLATCH1.D`, `DLATCH1.EN`, `DFF1.NQ`,
  `JK1.J`, `JK1.K`, `JK1.CLK`, `JK1.Q`, `JK1.NQ`, `T1.T`, `T1.CLK`,
  `T1.Q`, `T1.NQ`, `SRLATCH1.S`, `SRLATCH1.R`, `SRLATCH1.Q`,
  `SRLATCH1.NQ`,
  plus `DFF1.RST`, `DFF1.SET`, `DFF1.EN` on full-control (5-input) flops;
  numeric aliases (`DFF1.1`, `DFF1.Y`, `DFF1.3`) keep working, and canonical
  display (`list`, YAML, step/poke/peek reports) uses the named forms.
  Named pins on combinational gates are usage errors. Control names on
  bare 2-input flops are rejected like out-of-range numerics, as are
  kind-foreign names (`D` on a JK flop, `J` on a DFF).
- Every flop exposes a second output `NQ` (negated Q, read live as NOT Q
  with zero stored state). `NQ` wires like any output (`wire DFF1.NQ
  AND1.1`); sinks read the inverted value at every evaluation (scalar,
  bit-parallel, and sequential stepping). `output_fanout` keeps Q-only
  semantics; a parallel `nq_fanout` plus per-sink polarity track NQ
  edges. Dangling `NQ` is ignored by default (stays out of listings
  and block outputs until explicitly `use`d or wired — most flops
  never use it); `ignore`/`use` accept `DFF1.NQ` via a parallel ignore
  flag. `peek` prints both (`DFF1.Q=1 NQ=0 (serial 0)`) regardless of
  the flag, since it reads clocked state directly.
- DFF control set (minimum viable flop for counters/FSMs): 5-input
  flops (`D`=0, `CLK`=1, `RST`=2, `SET`=3, `EN`=4) resolve with total
  priority `RST (async) > SET (async) > EN-hold (sync) > D capture`;
  simultaneous RST+SET reads Q=0 (reset wins). `RST`/`SET` take effect
  immediately (no clock advance; `poke`/`wire`/`tie`/`load` apply them at
  once and `step` applies them before edging). `EN=0` holds Q across
  edges; unconnected RST/SET read 0 (inactive) and unconnected EN reads
  1 (enabled), so bare and dangling-control flops behave exactly as
  before. Async forces are global (every live flop, slot order);
  EN-hold/D-capture stay per stepped clock-domain member.
  Level latches (no clock advance required, no clock domain): 2-input
  DLATCH (`D`=0, `EN`=1) is transparent while `EN` is active (Q follows
  D at once) and frozen while `EN` is released (Q holds across input
  changes and advances; re-asserting EN resumes tracking); 2-input
  SRLATCH (`S`=0, `R`=1) sets on `S` alone (Q=1), resets on `R` alone
  (Q=0), and holds on neither (memory). `S=R=1` is illegal and refused
  loudly with state unchanged on every entry path (direct drive, wired
  net, tied class, loaded file). Both resolve outside advances
  (`poke`/`wire`/`tie`/`load` settle them at once; `step` re-settles
  before edging), hold through `step` per the rule, read dangling pins
  as 0 (hold), and power on at Q=0.
- JK/T flop kinds (classic counters/dividers without DFF emulation):
  3-input JK flops (`J`=0, `K`=1, `CLK`=2) resolve per edge from pre-edge
  Q as `J=0,K=0` hold / `J=0,K=1` reset (Q=0) / `J=1,K=0` set (Q=1) /
  `J=1,K=1` toggle; 2-input T flops (`T`=0, `CLK`=1) hold on `T=0` and
  toggle on `T=1`. Both are synchronous-only (no async pins; hold is
  `J=K=0` / `T=0`), ride the same stepper and verbs as DFF/DLATCH
  (poke/wire/peek/save-load, NQ polarity, dangling-reads-inactive, X/Z
  loud), and power on at Q=0. Level latches ride the same verbs with
  immediate (no-advance) visibility and the S=R=1 refusal above.
- Wires: single-driver rule (one wire per destination input, like real
  hardware); re-wiring an occupied input is a loud usage error, never a
  silent overwrite. Fan-out is unlimited (one output → many inputs).
  Cycles are rejected at `wire` time via incremental DFS that is
  flop-aware: edges into sequential-gate inputs never close a cycle
  (flop Q outputs break loops), so legal feedback (`DFF Q -> comb ->
  D`) wires in D-input-first order while pure combinational cycles
  still fail loudly.
- Block interface: derived on demand, never stored — dangling inputs and
  dangling outputs become the block's inputs/outputs in slot order,
  minus pins marked `ignore`. `ignore <pin>` / `use <pin>` toggles the
  mark. `list` shows gates + wires + derived interface + ignored pins.
- Tied inputs (input-input shorts as first-class nets): `tie A.1 B.2`
  merges two dangling input pins into one net, `untie A.1` removes one
  pin from its class. Equivalence classes are stored as a star in
  `Netlist::tie_rep` (0 == singleton, else leader+1); the canonical rep
  is the lowest (slot, input) and enumeration is slot order, so the
  interface counts one block input per class (leaders only). Both pins
  must dangle and be non-ignored — tying (or untying) wired/ignored/
  output pins, or tying a pin to itself, fails loudly; tying is
  idempotent within a class. `wire`/`ignore` on a tied pin fail loudly
  (untie first). `remove` detaches the gate's pins (survivors re-elect
  the lowest survivor), `reset` clears every class, and `list` shows one
  `A.1 = B.2` line per group. Standard patterns like NAND-with-pinned-
  together-inputs (NOT via NAND(a,a), AND via double inversion) are
  therefore directly usable: the tied truth table matches the replicated
  untied truth (each tied column fans out to its members).

## 5. Evaluation (perf core)

- Topology compiled once per mutation (`wire`/`add`/`remove` dirty flag):
  Kahn's algorithm over slot order → deterministic level order, cycle
  check included. Steady-state `solve` never re-sorts.
- Bit-parallel truth evaluation: pack up to 64 input assignments into one
  `uint64_t` word per net; each gate is one branchless word op
  (`& | ^ ~`) over the whole column. Cost is
  `O(gates × 2^n / 64)` word-ops, allocation-free after warmup
  (caller-owned scratch, same pattern as `fill_snapshot`).
  Scalar single-assignment `solve` reuses the same level order with a
  small stack array (no heap).
- `NOT`/`BUF` fold into the word op (no extra pass). `XOR` with >2
  inputs is defined as odd-parity (documented; matches 74-series cascade
  semantics only for 2 inputs — the usage text says so).
- Tied inputs share one column: scalar `solve` feeds the class leader's
  value to every member pin, and bit-parallel eval materializes one
  input-column word per class (members read the leader's word), so both
  paths stay bit-exact under the same row convention.
- Benchmark before optimizing further: `bench_logic`
  (ns/gate-word over a live Netlist, opaque depth/width args). SIMD
  intrinsics only on measured hot paths, never speculative.

## 6. Optimize

Levels, explicit and composable (`optimize [--to nand] [--level O1]`):

- O0: constant folding (`AND(x,0)→0`) + double-negation + identity
  (`AND(x,1)→x`, `OR(x,0)→x`) + dead-gate elimination (unreachable from
  any block output) + exact truth-table simplification for small blocks
  (n≤10, m≤16, no hidden outputs): detects semantic constants
  (contradictions like `x & ~x == 0` that structural folding misses) and
  dead inputs (inputs that never affect any output), collapses constants
  to canonical form and ties dead inputs off to 0. Always safe, always on.
  Never grows the net (gates_after <= gates_before).
- O1: O0 + De Morgan push (`NOT(AND)→NAND`, `NOT(OR)→NOR`, bubble pushing) —
  canonical form, still mixed gates.
- O2 (`--to nand`): full NAND-only (or `--to nor`) conversion. Small blocks
  (n≤8, m≤8, no hidden outputs) use exact two-level resynthesis
  (Quine-McCluskey + greedy cover, SOP for NAND / POS for NOR) with shared
  input inverters and globally tied dangling pins (one block input per
  variable, so the interface stays stable instead of duplicating pins for
  inversion); the caller picks the smaller of structural De Morgan vs
  minimized (gates, then inputs), so repeated O2 toggles (NAND<->NOR) stay
  near-optimal instead of 4x per toggle (e.g. `OR` 1→3→2→3→2 stable, not
  1→3→8→17). Large blocks fall back to structural De Morgan + duality with
  vocabulary-preserving cleanup (shared wired-gate dedup + internal
  double-inverter cancellation + DCE, all keep all-NAND/all-NOR and never
  merge sinks). Constant gates reuse a shared pool (internal share, sinks
  get dedicated outputs, so 1-gate constants stay at 1-2 gates, not 4+).
  Gate count may still grow for large/complex nets; the report prints
  before/after counts so the trade-off is visible, never hidden.
- Optimizer is pure: `optimize(netlist, opts) -> Netlist` (new object,
  input untouched). Loud on overflow (>existing slot cap); never
  silently drops gates.
- Interface caveat: folding drops dead pins, exact simplification drops
  dead inputs and collapses constants, O2 structural may duplicate pins
  for inversion (`NAND(a,a)` untied is NOT with two pins) while O2
  minimized ties them (one pin per variable, may permute input order) —
  so the post-optimize truth table matches the old one under pin
  replication/permutation, not row-for-row (e.g. `OR(AND(0,b),c)` ≡ `c`).
  The `optimize` verb prints a loud before→after interface report whenever
  pins change; re-run `truth` on the new pins and compare replicated/
  permuted assignments via `solve`.
- Ties through the optimizer: O0/O1 preserve classes whose pins survive
  (input-dropping rebuilds classes around shifted indices, De Morgan
  fusion copies each source pin's class to its fused counterpart,
  constant-collapse detaches the folded gate's pins with survivor
  re-election, exact dead-input elimination dissolves dead classes to
  ignored, DCE detaches via `remove_gate`). The O2 structural fresh build
  drops ties (each fresh dangling pin starts singleton) and reports the pin
  change through the same loud interface report — re-tie post-hoc with
  `tie`. The O2 minimized build auto-ties (one class per variable, so the
  interface stays at n inputs, not 2n); structural output for untied nets
  is otherwise unchanged.

## 7. Truth table

- `truth [--csv path] [--limit N]`: streams rows in input-slot order.
  The terminal prints a pretty aligned table (`A | B | Y` header,
  `-+-` separator, centered bits, truncation footer naming shown/total
  counts); `--csv` writes raw `0/1` CSV (header `in...,out...`) to a
  file plus a one-line `wrote N rows` summary. Default cap: block
  inputs ≤ 16 (65 536 rows); larger needs explicit `--limit`/`--csv`
  (refuse loudly, never print millions of rows to the terminal).
  Bit-exact vs `solve` (same word evaluator, same row convention).
- `solve [--in <bits>] [--from <pin>=<0|1> ...]`: single-assignment eval;
  unset block inputs default `0` loudly (the command echoes the assumed
  vector). Exit 0 with `Y=...` lines.

## 8. Draw + export

- `draw [--out schematic.bmp]`: layered topological layout (inputs left,
  outputs right, one column per level, deterministic pure function) +
  Manhattan wires. Two backends share the layout: the interactive window
  is a Dear ImGui canvas over the SDL2 2D renderer (GPU-anti-aliased
  vectors, embedded TTF labels, wheel-zoom + middle-drag pan, F fits,
  R resets to 1:1, Esc closes; close == stop, like `sig run --plot`
  — unsupported/no-display fails loudly with a rebuild hint), while
  file export stays on the SDL software surface + `SDL_SaveBMP`
  (headless-safe, byte-deterministic; PNG/JPG only via a later
  single-header writer task if a real consumer needs it — do not vendor
  image codecs for v1). The canvas is independent of `WEngineGui`: it
  uses the vendored imgui `sdlrenderer2` backend (SDL2 only, no Vulkan),
  and the TTF is an embedded DejaVu Sans subset (Bitstream Vera license,
  see below) so no runtime font file is ever located.
- `export --truth t.csv --schema s.yaml [--bmp s.bmp]`: one command,
  three artifacts, loud per-artifact counts. `import`/workspace restore
  is `load <file.yaml>` (round-trip byte-identity pinned by test).

- **Decision: embedded DejaVu Sans subset for canvas labels** (`src/cli/
  fonts/schematic_font.hpp`, generated — do not hand-edit): the ImGui
  canvas needs real vector text (pixel fonts do not survive zoom), and
  a runtime font-file lookup would be a new failure mode plus
  byte-fragile BMPs across machines. The subset (U+0020–00FF, hinting
  kept, ~57 KB → ~240 KB C array) is generated from the system
  DejaVuSans.ttf with fontTools (`pyftsubset --unicodes="U+0020-007F,
  U+00A0-00FF"`), loaded via `AddFontFromMemoryTTF`. DejaVu changes are
  in the public domain; Bitstream's copyright + permission notices ship
  with any copy (see the header + this section).

## 9. YAML workspace (vendored yaml-cpp — 4th third-party dep)

- **Decision: yaml-cpp as a git submodule under `external/yaml-cpp`
  (pinned 0.8.0, MIT), following the exact `external/lua` path:**
  `wengine_yaml` (`STATIC`, no project warning flags, `SYSTEM`
  includes) plus one runtime TU `src/logic/yaml_store.cpp` — the
  single TU allowed to know yaml-cpp exists (same seam as
  `src/signal/lua_solver.cpp`). Engine headers stay yaml-free, so unit
  tests link `wengine_core` alone and never pay the dep. This record is
  the dependency decision (per the README policy: no fifth dep without
  its own record).
- Rationale over hand-rolled: workspaces are user-edited artifacts, so
  real YAML (comments, quoting, multiline, anchors later) beats a
  canonical-subset parser users will fight; the submodule pin +
  from-source build keeps it auditable and warning-clean.
- Schema `version: 1` header, new generation (006): the retired
  first- and second-generation files are refused loudly (naming the found
  version), never loaded — no fallback loader, no migration. The loader
  accepts only `version: 1` files carrying all six sections, with a loud
  found-version refusal otherwise. Sections:
  `gates: [{id: AND1, kind: and, inputs: 2, ignore: [2]}]`,
  `wires: [{from: AND2.Y, to: NOR1.1}]`,
  `ties: [{a: NAND1.1, b: NAND1.2}]` (one entry per non-leader tied
  member: `a` = class leader, `b` = member, ordered by `b` in slot
  order, so the bytes are canonical),
  `clocks: [{name: BUF1.Y}]` (one entry per derived clock domain, in
  first-seen slot order — source-pin spelling, or bare `clk` for the
  implicit dangling-clock domain; membership stays derived from the
  topology and is validated by set-equality on load),
  `meta: {created, tool}`.
  Strict: unknown keys/sections are loud errors (never silently
  ignored) — forward-compatible sections arrive as versioned schema
  bumps, not silent tolerance. `save`/`load` round-trip is byte-identical
  (pinned test). Sequential gates persist with per-row `init: 0|1`
  power-on values (default 0 when absent from hand-written files):
  `save` captures live pool Q as the file's inits, and `load` starts a
  new pool lifetime powered on to them. Named pins persist in wire rows
  (`from: DFF1.NQ`, `to: DFF1.D`, `to: DFF1.RST`) plus an optional
  `ignore_nq` gate key defaulting to false. DFF rows carry `inputs: 2`
  (bare) or `inputs: 5` (full control set).
  Runnable example workspaces live in `examples/logic/`
  (`nand_pair`, `fanout`, `parity3`, `tieoff`, `tied_demorgan` plus the
  advanced sequential references `two_clock_counter`,
  `gated_async_counter`, `jk_t_latch_chain` — each header shows its run
  command and expected output); `test_logic_yaml` loads every file,
  checks its hand-derived goldens, and pins save/load/save stability.

## 10. Module verb list (v1)

```text
add <KIND> [--inputs N]     list [--wires] [--iface]   wire <FROM.out> <TO.in>
unwire <TO.i>               remove <GATE>              ignore <PIN> / use <PIN>
tie <A.i> <B.j>             untie <PIN.i>              reset [--force]
solve [--in bits] [--from pin=0|1 ...]
truth [--csv p] [--limit N] optimize [--to nand|nor] [--level O0|O1|O2]
draw [--out f.bmp]          save <f.yaml> / load <f.yaml>
export --truth t --schema s [--bmp b]   help [verb]   back
step [DOMAIN] [N] [--cycles N]   poke <PIN>=<0|1|release> [--from ...] [--clear]
peek [GATE]
```

Sequential session verbs (all headless: REPL session, one-shot, pipes,
scripts — zero GUI dependency):
`step` advances one clock domain (edge flops sharing a CLK driver net;
dangling CLK pins form the implicit `clk`) or all edge flops globally,
reporting the cycle count, per-flop Q transitions, and settled outputs.
Level latches belong to no domain: they settle outside advances (visible
in the next `peek` with no `step`) and their frozen Q survives any
advance; a latch-only workspace reports Q lines with no cycle line.
`poke` drives/releases dangling block inputs (sticky across steps);
`peek` prints flop/latch Q+NQ with stable serials in slot order. Poked
or wired RST/SET force Q at once (visible in the next `peek` with no
`step`); poked or wired latch nets settle at once the same way, while
an illegal SR net (S=R=1 via drive, wire, tie, or file) is refused as
a usage error with the workspace kept;
`step` applies pending async controls and the level settle before
edging, and forced/set moves report in the same `DFF1.Q: a->b` form as
captures. Poke maps and cycle counters are session state beside the
workspace, cleared by `reset`, never serialized. Wire D-inputs before Q
fanout (the incremental cycle check trips on Q-first wiring).

All verbs: `kName/kDescription/kUsage/run/complete` + module-registry
entry + `static_assert` size check (same recipe as AGENTS.md §9).

## 11. Non-goals (v1) — each with a named V2 slot in §12

Tri-state buses,
multi-driver nets, timing/delay simulation, Verilog/VHDL import,
PNG/JPG codecs, GUI panel (separate epic, consumes the same headers).

## 12. V2 extension map (no redesign: every item below is a named slot)

V1 ships only combinational single-driver zero-delay logic, but the
scaffolding already reserves each V2 feature's place. Rule for all of
them: additive only — `Netlist` never reshapes, kind bytes are
append-only, schema bumps carry fallback loaders (a generation may still
be retired without a loader by explicit decision — see Schema evolution
below).

- **Value system.** Nets carry `LogicValue {kLow, kHigh, kHighZ,
  kUnknown}` today; v1 produces/consumes only Low/High and rejects
  Z/X loudly. Tri-state buses later just start producing Z — no type
  change, no call-site churn.
- **Drivers.** Every input resolves through `resolve_net(span of
  driver values)`, which today enforces single-driver. Multi-driver /
  wired-OR / tri-state resolution later replaces one function body;
  the `drivers[]` arrays and `wire` verb already store fan-in lists.
- **Sequential / clocks (shipped).** `GateKind` values 0..7 are combinational;
  sequential kinds append at ≥16, pinned: `DFF`=16 (D+CLK,
  edge-triggered; 2 inputs bare, or 5 with the RST/SET/EN control set),
  `DLATCH`=17 (D+EN, level-sensitive: EN=1 transparent, EN=0 frozen),
  `JK`=18 (J+K+CLK, edge-triggered; exactly 3 inputs),
  `T`=19 (T+CLK, edge-triggered; exactly 2 inputs),
  `SRLATCH`=20 (S+R, level-sensitive: set/reset/hold, S=R=1 illegal) —
  never renumber, never reuse persisted bytes. DFF priority is total:
  `RST (async) > SET (async) > EN-hold (sync) > D`, reset wins
  simultaneous assertion; controls resolve in slot order with X/Z loud.
  JK excitation is total: `J=0,K=0` hold / `J=0,K=1` reset / `J=1,K=0`
  set / `J=1,K=1` toggle from pre-edge Q; T excitation is total: `T=0`
  hold / `T=1` toggle; both resolve in slot order with X/Z loud.
  Level rule is total: DLATCH `EN=1` follows D, `EN=0` holds; SRLATCH
  `S=1,R=0` sets, `S=0,R=1` resets, `S=0,R=0` holds, `S=1,R=1` is
  refused loudly with state unchanged on every entry path (direct drive,
  wired net, tied class, loaded file); latches resolve in slot order
  with X/Z loud, outside advances, and belong to no clock domain.
  Clocked state lives in a **parallel pool beside
  the netlist** (`logic::SeqState` in `seq_state.hpp`: per-slot Q + valid +
  never-reused serials, synced by `init_seq_state`, cleared by `reset_seq_state` —
  same pattern as `Signals` beside `Particles`), stepped per edge by
  `seq_step`/`seq_step_filtered` (global edge or one named clock domain
  over edge kinds only; level latches re-settle at every step top;
  the combinational evaluator only gained NQ-polarity reads at existing
  resolution funnels). Cycle validation is flop-aware: `validate_sequential` runs Kahn over
  combinational edges only (drivers into sequential-gate inputs
  excluded — all five DFF inputs, both DLATCH inputs, all three JK
  inputs, both T inputs, and both SR inputs; Q outputs
  break loops), and `wire()` / the `wire` verb take that path whenever
  sequential gates are present (pure-comb netlists keep the old path).
  Session verbs `step`/`poke`/`peek` expose control and readback
  headlessly; the engine path is shipped too: a header-only
  `SequentialSolver` (`kName="logic"`) behind the `ISolver` seam steps the
  singleton pool through `core::Runner` with end-of-step value-at-T+dt
  stamps, a nested `logic::SequentialRunConfig` section (topology +
  stimulus + multi-clock member list, honored only for solver `logic`)
  aggregated into `core::RunConfig`, extended `Snapshot` rows
  (`seq_serial`/`seq_value`/`seq_output` in slot order), format-v6
  checkpoints with v1–v5 fallbacks, one `link::Frame` adapter
  (`seq.count` + `seq.<serial>.Q`) plus frame-source registry entry, one
  tidy `LogicCsvSink`, and `sim run --solver logic` with
  `--seq-file/--seq-clocks/--seq-from` options.
- **Timing / delay.** Every gate already stores `propagation_delay_ps`
  (default 0; v1 eval is zero-delay, loader stores any value). Delay
  simulation later reads the field — no schema or struct change, only
  an evaluator that honors it plus a `set-delay` verb.
- **Verilog / VHDL import.** Importers are pure `Netlist` builders
  (`parse_verilog(text) -> Result<Netlist>`) in their own TUs, reusing
  the same validation (`wire` single-driver/cycle checks) as the CLI
  verbs. No new engine concepts: a file is just another author of the
  same struct. New TU, never a `Netlist` change.
- **PNG / JPG codecs.** Draw already splits pure layout math (header,
  tested) from raster output. File export goes through one
  `write_image(layout, path, format)` seam whose v1 backends are BMP
  (SDL_SaveBMP) and CSV; a single-header writer (e.g. stb-style,
  vendored like any other dep with its own record) adds backends
  without touching layout or the `draw`/`export` verbs.
- **GUI panel.** Consumes `wengine/logic` headers directly plus one
  `link::Frame` adapter + one frame-source registry entry (AGENTS.md
  §4 views rule); views stay untouched. The CLI session registry is
  the verb vocabulary the panel mirrors — no engine change for GUI.
- **Schema evolution.** `version: 1`, new generation (006): retired
  first- and second-generation files are refused loudly with no fallback
  loader — the only legacy behavior is the version-naming refusal. The
  same loud-refusal convention as `world` serialization applies to
  forward versions; unlike `world` (`v6` + fallbacks), the workspace
  keeps no backward-compatibility loaders by explicit decision.
