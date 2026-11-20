# Contract: CLI Verb Surface — Buffer + Pulls (012)

**Seam**: `include/wengine/cli/logic_registry.hpp`,
`src/cli/commands/logic/*.cpp`. One command domain (`logic`) — no new
top-level command. Interactive/headless byte parity by construction
(`TuiExecutor` → `cli::Shell`; no TUI change, no logic form exists).

## `add` — gains `TBUF`

- `add TBUF [--inputs 2]` (only 2 is accepted; other counts fail with
  the existing `bad fan-in for kind` usage error, exit 2). Usage text
  gains the kind and its pin contract (`TBUF exactly 2: pin 1 = data,
  pin 2 = enable, active-high; released enable drives high-Z`).
- All three kind spellings sync: `add` usage/error want-strings
  (add.cpp), `kCompleteKinds` (completion), and `parse_gate_kind`'s
  want-string (pins.hpp) — `...|NOT|BUF|TBUF|DFF|...`.
- Completion: `logic add T` completes `T`, `TBUF` (registry order);
  `add TBUF --` completes `--inputs`, `--help`, `-h`.

## `pull <TO.i> <up|down>` — new verb (LogicRegistry 23 → 24)

- Sets the pull on the destination input pin (wired or dangling, but
  never tied, ignored, or an output). Prints confirmation naming the
  net and level (e.g. `pull BUS.1 up` → `pulled BUS.1 up`).
- Refused loudly (usage, exit 2): dead/out-of-range pins, output
  pins, tied inputs ("untie it first"), ignored inputs ("use first"),
  unknown level spellings, stray positionals. Errors change no state.
- Completion: input pins in first position; `up`/`down` in second;
  `--help`/`-h` in flag position.

## `unpull <TO.i>` — new verb (LogicRegistry 24 → 25)

- Clears the pull on a pulled input; prints confirmation naming the
  input. Refused loudly (usage, exit 2) on unpulled inputs ("no pull
  set"), tied/ignored/dead pins, and stray positionals — mirroring
  `unshare`'s lifecycle refusals.
- Completion: input pins; `--help`/`-h`; no options with values.
- Registered per the module recipe: spec structs in
  `logic_registry.hpp`, appended to `LogicRegistry` in registry order
  (`... LogicUnshareSpec, LogicPullSpec, LogicUnpullSpec,
  LogicRemoveSpec ...` — exact position pinned in tasks),
  `.cpp` files added to `wengine_cli` sources, `LogicSpec::kUsage`
  verb list updated.
- Registry pins to sync in `tests/test_cli_args.cpp`:
  `LogicRegistry::all().size() == 25` (was 23), per-verb `find`
  blocks, `verbs.size()` asserts, the `p`-prefix completion vector
  gains `pull`, the `u`-prefix vector gains `unpull`, in registry
  order.

## `wire` — unchanged

- Pulled destinations wire normally (no flag, no extra refusal);
  post-wire async/level settle observes resolved idle values as
  ordinary data. Success lines keep their shape (buffer/pull-free
  flows byte-identical).

## `list` — pull inspectability

- Per pulled input, the gate/input listing annotates the pull
  (net + level, beside the shared-mark annotations). Buffer gates
  list via `gate_kind_name` automatically (`TBUF<n>`).
- Buffer/pull-free workspaces list byte-identically to today.

## `solve` / `step` — Z rendering, contention unchanged

- Value lines render high-Z block outputs explicitly as `Z`
  (new arm beside `0`/`1`); pre-012 netlists cannot produce Z
  outputs, so their bytes are unchanged.
- The `contention:` line keeps its 011 shape and zero-inclusive
  presence rule; pull settings never alter it (counts are computed
  pre-pull-map). `reset` clears counts; pulls persist like marks.

## `truth` / `export` — loud refusal on buffers

- Over a netlist containing any live `kTbuf` gate: loud failure
  naming the construct and pointing at scalar `solve` (exit 1
  runtime failure, never a silent misrepresentation). Pull-only
  netlists stream unchanged.

## `optimize` — O2 refusal extension

- O2 refuses loudly when live buffers or pulls exist (workspace
  untouched, O0/O1 remain); the refusal names the construct.
  O0/O1 succeed with buffer gates frozen and pulled inputs skipped
  (resolution results unchanged; optimize-of-optimize idempotent).

## Completion coverage (FR-015)

- `logic pull ` completes input pins; `logic pull NET ` completes
  `up`/`down`; `logic unpull ` completes input pins; both complete
  `--help`/`-h` in flag position.
- `logic add ` completes `TBUF` among kinds.

## Documentation duties (constitution: README is a living spec)

- README: verb sentence gains `pull`/`unpull` and the `add TBUF`
  flow; a runnable gated-bus example block; test list gains the new
  binary; module table's `logic` row mentions enable-gated buffers
  and per-net pulls.
- `docs/logic.md`: §10 verb grammar gains `pull`/`unpull` + TBUF
  (pins 1=data/2=enable); §9 single-driver rule gains the
  termination note (pulled dangling inputs are not block inputs);
  §11 non-goals drops the tri-state/pull gate entries; §12 Drivers
  slot ships buffer authoring + pull mapping + the O2 follow-on;
  schema notes record the additive `pulls:` decision (research D6).
- `docs/tests/logic.md`: page for the new binary; catalog audit stays
  zero-missing/zero-ghost.
