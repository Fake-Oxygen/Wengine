# Logic-gate module (`logic`) — design guidelines

Status: draft scaffold (2026-10-23). No behavior wired yet; see Plane epic
`logic-gates` (parent) + LOGIC-* children for build order.

## 1. What ships in v1

- In-process `logic` module: add/list/wire/reset logic gates, solve the
  combinational block, print/stream its truth table, optimize it
  (De Morgan → NAND-only + constant folding + dead-gate elimination),
  draw it (interactive ImGui canvas with zoom/pan, `--out bmp` file
  export), and
  save/restore the workspace as versioned YAML.
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
  plus V2 sequential `DFF` (=16, edge-triggered D+CLK) and `DLATCH`
  (=17, level-sensitive D+EN), each exactly 2 inputs (SEQ1 vocabulary;
  creation/stepping arrive in SEQ2+).
- `kMaxInputs = 8`. `NOT`/`BUF` take exactly 1 input; `DFF`/`DLATCH`
  take exactly 2; the rest 2..8.
- Identity: per-kind monotonic counters, never reused
  (`AND1, AND2, …`, `NOT5, …`), slots recycle — same convention as
  signal serials (AGENTS.md §8). Display names use kind+counter;
  internals use dense slot indices.
- Pins: `<NAME>.Y` (output) or `<NAME>.<1..k>` (input), e.g. `AND2.Y`,
  `NOR1.1`. Parsing is a pure `parse_pin(text) -> Result<PinRef>`.
- Wires: single-driver rule (one wire per destination input, like real
  hardware); re-wiring an occupied input is a loud usage error, never a
  silent overwrite. Fan-out is unlimited (one output → many inputs).
  Cycles are rejected at `wire` time via incremental DFS (combinational
  only in v1; sequential/clocked gates are a follow-up epic, not v1).
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
  any block output). Always safe, always on.
- O1: De Morgan push (`NOT(AND)→NAND`, `NOT(OR)→NOR`, bubble pushing) —
  canonical form, still mixed gates.
- O2 (`--to nand`): full NAND-only (or `--to nor`) conversion via De
  Morgan + duality. Gate count may grow; the report prints
  before/after counts so the trade-off is visible, never hidden.
- Optimizer is pure: `optimize(netlist, opts) -> Netlist` (new object,
  input untouched). Loud on overflow (>existing slot cap); never
  silently drops gates.
- Interface caveat: folding drops dead pins, O2 duplicates pins for
  inversion (`NAND(a,a)` is NOT), and gates are renamed — so the
  post-optimize truth table matches the old one under pin replication,
  not row-for-row (e.g. `OR(AND(0,b),c)` ≡ `c` becomes `AND(x,y)` over
  duplicated pins `[c,c]`). The `optimize` verb prints a loud
  before→after interface report whenever pins change; re-run `truth`
  on the new pins and compare replicated assignments via `solve`.
- Ties through the optimizer: O0/O1 preserve classes whose pins survive
  (input-dropping rebuilds classes around shifted indices, De Morgan
  fusion copies each source pin's class to its fused counterpart,
  constant-collapse detaches the folded gate's pins with survivor
  re-election, DCE detaches via `remove_gate`). The O2 fresh build drops
  ties (each fresh dangling pin starts singleton) and reports the pin
  change through the same loud interface report — re-tie post-hoc with
  `tie`. The optimizer never auto-ties: O2 output for untied nets is
  unchanged.

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
- Schema `version: 2` header (v1 files without `ties:` load via a
  fallback loader as untied workspaces); the loader accepts 1..2 with a
  loud found-version refusal otherwise (same rule as link wire versions;
  V3 bumps with a fallback loader, §12). Sections:
  `gates: [{id: AND1, kind: and, inputs: 2, ignore: [2]}]`,
  `wires: [{from: AND2.Y, to: NOR1.1}]`,
  `ties: [{a: NAND1.1, b: NAND1.2}]` (one entry per non-leader tied
  member: `a` = class leader, `b` = member, ordered by `b` in slot
  order, so the bytes are canonical), `meta: {created, tool}`.
  Strict: unknown keys/sections are loud errors (never silently
  ignored) — forward-compatible sections arrive as versioned schema
  bumps, not silent tolerance. `save`/`load` round-trip is byte-identical
  (pinned test). Runnable example workspaces live in `examples/logic/`
  (`nand_pair`, `fanout`, `parity3`, `tieoff`, `tied_demorgan` — each
  header shows its run command and expected output); `test_logic_yaml`
  loads every file, checks its hand-derived goldens, and pins
  save/load/save stability.

## 10. Module verb list (v1)

```text
add <KIND> [--inputs N]     list [--wires] [--iface]   wire <FROM.Y> <TO.i>
unwire <TO.i>               remove <GATE>              ignore <PIN> / use <PIN>
tie <A.i> <B.j>             untie <PIN.i>              reset [--force]
solve [--in bits] [--from pin=0|1 ...]
truth [--csv p] [--limit N] optimize [--to nand|nor] [--level O0|O1|O2]
draw [--out f.bmp]          save <f.yaml> / load <f.yaml>
export --truth t --schema s [--bmp b]   help [verb]   back
```

All verbs: `kName/kDescription/kUsage/run/complete` + module-registry
entry + `static_assert` size check (same recipe as AGENTS.md §9).

## 11. Non-goals (v1) — each with a named V2 slot in §12

Sequential/clocked logic (flip-flops, clocks), tri-state buses,
multi-driver nets, timing/delay simulation, Verilog/VHDL import,
PNG/JPG codecs, GUI panel (separate epic, consumes the same headers).

## 12. V2 extension map (no redesign: every item below is a named slot)

V1 ships only combinational single-driver zero-delay logic, but the
scaffolding already reserves each V2 feature's place. Rule for all of
them: additive only — `Netlist` never reshapes, kind bytes are
append-only, schema bumps carry fallback loaders.

- **Value system.** Nets carry `LogicValue {kLow, kHigh, kHighZ,
  kUnknown}` today; v1 produces/consumes only Low/High and rejects
  Z/X loudly. Tri-state buses later just start producing Z — no type
  change, no call-site churn.
- **Drivers.** Every input resolves through `resolve_net(span of
  driver values)`, which today enforces single-driver. Multi-driver /
  wired-OR / tri-state resolution later replaces one function body;
  the `drivers[]` arrays and `wire` verb already store fan-in lists.
- **Sequential / clocks.** `GateKind` values 0..7 are combinational;
  sequential kinds append at ≥16, pinned: `DFF`=16 (D+CLK,
  edge-triggered), `DLATCH`=17 (D+EN, level-sensitive) — never
  renumber, never reuse persisted bytes. Clocked state lives in a **parallel pool beside
  the netlist** (`logic::SeqState` in `seq_state.hpp`: per-slot Q + valid +
  never-reused serials, synced by `init_seq_state`, cleared by `reset_seq_state` —
  same pattern as `Signals` beside `Particles`), stepped
  by a `SequentialSolver::step(state, dt)` behind the existing
  `ISolver` seam — the combinational evaluator is untouched, and
  `RunConfig` gains one nested section struct per AGENTS.md §8.
  Cycle validation is flop-aware: `validate_sequential` runs Kahn over
  combinational edges only (drivers into DFF.D/CLK excluded, Q outputs
  break loops), and `wire()` / the `wire` verb take that path whenever
  sequential gates are present (pure-comb netlists keep the old path).
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
- **Schema evolution.** `version: 2` (clocks/timing sections) loads v1
  files via a fallback loader and refuses forward versions loudly —
  the same convention as `world` serialization (`v5` + fallbacks).
