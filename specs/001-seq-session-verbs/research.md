# Research: Sequential Session Verbs

**Feature**: `specs/001-seq-session-verbs/spec.md` | **Date**: 2026-10-31

All Technical Context unknowns resolved from repo context (no external
research needed; every decision below cites the existing source that
pins it). No NEEDS CLARIFICATION markers remain.

## D1: Clock model — global edge vs named selective clocks

- **Decision**: One subset-step engine core: `step N` advances all flops
  (global, = today's `seq_step` semantics); `step <domain> N` advances
  only flops whose CLK/EN input is driven by the named net. Both share
  the same snapshot/commit routine; the domain filter selects the flop
  subset. A clock domain is the set of flops sharing one CLK/EN driver
  net, named by that net's source pin (e.g. `BUF1.Y`); flops with
  ignored/dangling CLK pins belong to the implicit `clk` domain.
- **Rationale**: The engine stepper is explicitly global-clock
  (`seq_step.hpp`: "Single rising edge over the whole netlist (global
  clock — CLK/EN pins are validated Low/High but do not gate; the edge
  itself is the call")). The spec (FR-001, Story 1 scenario 2) requires
  named clocks with selective advance, so a pure-global verb would fail
  the spec. Subset stepping is deterministic under the same slot-order
  rules (snapshot selected flops in slot order, commit in slot order,
  global combinational re-settle), so it preserves Constitution II while
  meeting the spec. Un-driven-CLK flops in the implicit domain keep the
  existing test circuits (which `ignore` CLK pins) working unchanged.
- **Alternatives considered**:
  - Global-only `step N` with the clock name as decoration — rejected:
    silently advancing unrequested flops violates spec scenario 2 and
    Constitution I (surprising behavior).
  - Separate per-clock `SeqState` pools — rejected: splits one pool
    convention, breaks serial uniqueness across pools, and shared
    combinational cones make per-pool states incoherent.

## D2: Sequential gate creation (`add DFF` / `add DLATCH`)

- **Decision**: Extend the creation path additively: allow
  `add_gate(nl, kDff/kDlatch)` (exactly 2 inputs, `D+CLK`/`D+EN` per
  `valid_fanin`), with never-reused per-kind counters stored by
  extending `next_counter` coverage to ordinals 16/17 (array growth,
  zero-init safe, same wrap-skip-0 rule). The `add` verb accepts the two
  new kinds with the same `--inputs` validation shape (fixed at 2).
- **Rationale**: `add_gate` still refuses sequential kinds (SEQ1 staging
  pin, `netlist.hpp`: "no Netlist slot may hold them yet … Loud until
  then"; pinned by `test_logic_seq_kinds.cpp:125`). Engine tests
  hand-place flops (`place_seq` in `test_logic_seq_step.cpp`), which no
  verb can reach — so without this, no session story is exercisable.
  Counter derivation on YAML load comes from gate IDs (`yaml_store.cpp`
  parses `KIND+counter`), so no schema field is added and v1 files load
  unchanged; saving a workspace with `DFF1` rows re-imports via the same
  ID rule (loader must already accept kind bytes 16/17 — verified as a
  task precondition, fallback: extend the kind parser, append-only).
- **Alternatives considered**:
  - A separate `flop` creation verb — rejected: duplicates `add`'s
    slot/counter/wire/validation machinery for two kinds; Constitution V
    forbids the extra abstraction.
  - External counters beside `Netlist` — rejected: splits the
    per-kind-counter convention and the YAML ID-derivation rule across
    two owners (see Complexity Tracking in plan.md).

## D3: Poke state lives at the CLI edge, not in the engine

- **Decision**: Poke values are a process-wide sticky map beside the
  workspace (`src/cli/commands/logic.cpp`, same lifetime as the existing
  `workspace()`): `(slot,input) → driven LogicValue`, keyed post-tie to
  class leaders. Each `step` builds its `block_inputs` vector from the
  map (unset pins default Low, echoing `solve`'s convention in
  `solve.cpp`), so `seq_step`'s signature and stateless-input contract
  are untouched. `reset` clears the map; `save`/`load` do not persist it
  (session stimulus, like shell history — documented in verb usage).
- **Rationale**: `seq_step` takes inputs as a caller-provided span every
  call — persistence across steps is inherently a caller concern, and
  `Netlist` must never reshape for GUI/CLI needs (docs/logic.md §12).
  The `solve --from PIN=0|1` spelling (`solve.cpp`) is the established
  pin-value convention, so `poke` reuses `PIN=<0|1|release>` parsing and
  the `parse_pin` + tie-leader resolution code path verbatim.
- **Alternatives considered**:
  - Forcing flops into `Netlist` (extra driver fields) — rejected:
    reshapes the engine struct for a CLI need; violates §12 and V.
  - Non-sticky poke (per-step `--from`-style only) — rejected: spec
    Story 2 requires values that hold across advances; re-typing full
    stimulus every cycle is not scriptable.

## D4: Peek output format

- **Decision**: One line per flop in slot order:
  `DFF1.Q=1 (serial 0)`; block scope `peek DFF1` prints that block's
  flops; bare `peek` prints all live flops in slot order. Empty (no
  sequential gates) prints a defined `no flops` line with success exit.
  `Q` values come from `SeqState.q` after `init_seq_state` re-sync
  (idempotent, keeps serials stable — `seq_state.hpp` ownership notes).
- **Rationale**: Slot order is the repo-wide determinism convention
  (topo compile, tie leaders, snapshot/commit all slot-ordered); serials
  are the stable cross-session identity (mirrors `Signals` serials), so
  printing both gives humans names and scripts stable ids. Matches the
  `solve` output idiom (`PIN=value` lines, `list` naming via
  `support::pin_text`/`gate_name`).
- **Alternatives considered**:
  - Serial order instead of slot order — rejected: serials are
    allocation order, which equals slot order only until removals; slot
    order is the deterministic enumeration used everywhere else.
  - Binary blob/CSV output — rejected: spec demands human-readable
    session text; CSV export already exists via `export` if needed.

## D5: Cycle counters are CLI-edge display state

- **Decision**: Per-domain cycle counts live beside the poke map
  (domain name → `uint64`), incremented by successful `step` calls,
  reported as `clk: cycle 5` after every advance (including zero-cycle
  advances, which report without stepping). `reset` clears them.
- **Rationale**: The engine has no cycle-count concept (edges are calls;
  `SeqState` holds Q, not time) and must not grow one for display needs
  (Constitution V; `Snapshot`/`State` must not grow for observer needs —
  same rule as link `Frame`). Counters are session diagnostics like the
  poke map, so they share its lifetime and non-persistence.
- **Alternatives considered**: Engine-side counters in `SeqState` —
  rejected (observer state in the hot pool; also breaks the
  `init_seq_state`-is-idempotent contract with extra mutation).

## D6: One-shot / session / pipe parity and error contract

- **Decision**: Verbs implement the standard `CommandSpec` shape
  (`kName/kDescription/kUsage/run/complete`) and are appended to
  `LogicRegistry`, so one-shot (`logic step …`), in-session (`step …`),
  and piped-stdin lines dispatch through the same `run` (registry note
  in `logic_registry.hpp`: "Verbs dispatch identically one-shot and
  in-session"). Usage errors use `cli::usage_error` (exit 2, state
  untouched — including poke map and cycle counters); runtime/step
  failures use `cli::runtime_error` (exit 1). `complete()` covers verb
  names, clock-domain names, pin names (`support::collect_*` helpers),
  and `--opt=` value forms, per AGENTS.md §9.
- **Rationale**: Directly required by spec FR-006/FR-007/FR-009 and the
  repo's CLI contract (`cli/status.hpp`, AGENTS.md §8/§9). No new
  mechanism; the `Shell`/subshell plumbing already routes all three
  surfaces to one registry.
- **Alternatives considered**: None — this is the only conforming shape;
  recorded here so tasks.md need not re-derive it.

## D7: Named sequential pins (D/CLK/EN/Q) with numeric aliases

- **Decision**: One alias table in `netlist.hpp` beside
  `valid_fanin`/`is_sequential_kind`: DFF inputs `D`=0/`CLK`=1, DLATCH
  inputs `D`=0/`EN`=1, outputs `Q` (+`NQ`, see D8). `parse_pin` accepts
  the names (case-insensitive, matching the `resolve_gate` precedent)
  for sequential kinds only; numeric `.1`/`.2`/`.Y` keep working for all
  kinds; names on combinational gates are usage errors. Canonical
  display uses names for sequential pins: CLI `support::pin_text` and
  the YAML writer emit `DFF1.D`/`DFF1.CLK`/`DFF1.Q`.
- **Rationale**: `parse_pin` is the single funnel every surface uses —
  the `wire` text API, `solve --from`, `tie`/`untie`, `ignore`/`use`,
  and the YAML loader (`yaml_store.cpp` parses every wire/tie pin
  through it) — so one table updates all verbs, completion
  (`collect_pins`), and file I/O at once with no per-verb mapping code
  to drift. Keeping numerics as aliases preserves existing engine tests
  and any piped scripts written against `.1`/`.Y`.
- **Alternatives considered**:
  - Per-verb name→index maps — rejected: N funnels that drift; a new
    verb forgetting the map reintroduces numeric-only behavior.
  - Numeric-only (status quo) — rejected: the user explicitly requested
    readable names; `DFF1.1` vs `DFF1.D` is a real usability gap for
    session/pipe users.
  - Renaming stored indices — rejected: indices are structural (arrays,
    tie classes, YAML); names are presentation and belong at the
    parse/display edges.

## D8: `NQ` negated output as a second wireable output

- **Decision**: `NQ` is a second addressable output on sequential kinds
  only, always read as NOT Q with zero stored state:
  `PinRef`/`BlockPin` gain an output selector (Q vs NQ, default Q so
  all existing constructions are unaffected); one additive per-sink
  polarity array in `Netlist` (zero-init = non-inverted = today's
  behavior exactly), set by `wire_pins` from the source ref and cleared
  by `unwire`/`remove_gate`; every input-resolution site (scalar solve
  path, bit-parallel word path, `seq_step` phase-1/snapshot/phase-2 via
  `resolve_step_input`) applies the inversion after `resolve_net`;
  `output_fanout` keeps Q-fanout semantics with a new additive
  `nq_fanout`; block outputs enumerate dangling Q then dangling NQ per
  gate in slot order; `ignore` gains a per-NQ flag (new additive field,
  Q behavior unchanged); YAML wires persist the source text (`from:
  DFF1.NQ`, no schema bump — still `version: 2`, strict loader learns
  the suffix through `parse_pin`); slot-level `wire(nl, from, to, input)`
  stays non-inverted, `wire_pins(PinRef, PinRef)` carries polarity.
  `list`/`draw`/`collect_pins`/completion expose both outputs; `peek`
  prints Q and NQ.
- **Rationale**: NQ is pure NOT Q, so no `SeqState` change (pool,
  serials, snapshot/commit rules untouched — Constitution II holds by
  construction); polarity-at-sink keeps one `gate_out` value per slot so
  no eval loop changes shape, only each resolution site gains one
  conditional inversion at its existing funnel. Additive fields keep
  zero-initialized `Netlist{}` behaviorally identical for all
  combinational work (Constitution I/V).
- **Alternatives considered**:
  - NQ as a separate pseudo-gate — rejected: breaks serial identity,
    doubles topology bookkeeping, and contradicts the request (`DFF1.NQ`
    on the same block).
  - Second `gate_out` value per slot — rejected: reshapes every eval
    loop and pool-adjacent code for a value computable at read time.
  - Baking the inverted value in at wire time — rejected: Q changes on
    every step, stored values would go stale; inversion must be live.
