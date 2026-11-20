# Contract: CLI Verb Surface — Delay Annotation (013)

**Seam**: `include/wengine/cli/logic_registry.hpp`,
`src/cli/commands/logic/*.cpp`. One command domain (`logic`) — no new
top-level command. Interactive/headless byte parity by construction
(`TuiExecutor` → `cli::Shell`; no TUI change, no logic form exists).

## `set-delay <NAME> <VALUE>` — new verb (LogicRegistry 25 → 26)

- Writes the whole-picosecond delay on the named live gate (any kind).
  Value grammar and refusals per
  [engine-seam.md](engine-seam.md) ("Exactness contract"); bare
  integers mean picoseconds, suffixed forms convert exactly through
  the owned time table.
- Success line names the gate and the canonical value:
  `set AND1 delay=1500ps` (canonical ps echo regardless of the token
  form used).
- Refused loudly (usage, exit 2, workspace untouched): unknown gate
  name, missing/extra operands, every value refusal class
  (non-negative/whole-ps/field-bound/unknown-suffix/grammar). Zero is
  accepted and equal to clearing.
- Usage text (`help set-delay`) carries the grammar, examples
  (`set-delay AND1 1500` / `set-delay AND1 1.5ns`), the bare-means-ps
  rule, and the standing inertness note ("stored only; evaluation
  stays zero-delay — docs/logic.md §12; a future delay simulator reads
  it").
- Completion: `--help`/`-h` in flag position; gate names in the first
  positional (`collect_gate_names`); **nothing in the value position**
  (values are free-form, not an enumeration — research D7).

## `unset-delay <NAME>` — new verb (LogicRegistry 26 → 27)

- Clears the annotation on the named live gate; success line
  `cleared AND1 delay`. Refused loudly (usage, exit 2) when no
  annotation exists ("no delay set on AND1" — the `unpull` lifecycle
  precedent), on unknown gates, and on stray positionals.
- Completion: gate names; `--help`/`-h`.

## Registration (the module recipe)

- Spec structs in `logic_registry.hpp` (`kName`, `kDescription`,
  `kUsage`, `run`, `complete`, Doxygen per struct) —
  `LogicSetDelaySpec`, `LogicUnsetDelaySpec`; appended to
  `LogicRegistry` **after `LogicUnpullSpec`, before
  `LogicRemoveSpec`** (the 012 insertion precedent).
- `.cpp` files (`set_delay.cpp`, `unset_delay.cpp`) added to
  `wengine_cli` sources in CMakeLists.txt.
- `LogicSpec::kUsage` static session-verb list (commands.hpp:644-645)
  gains `set-delay unset-delay` after `unpull`.

## Registry pins to sync in `tests/test_cli_args.cpp`

- `:689` `static_assert(LogicRegistry::all().size() == 27)` (was 25).
- `:816` `verbs.size() == 27`; `:822` help-completion count 27.
- `:1762` `want_s` gains `"set-delay"` **before** `"solve"` (registry
  order: the insertion point precedes `solve`).
- `:1766` `want_u` gains `"unset-delay"` after `"unpull"`.
- `find("set-delay")` / `find("unset-delay")` blocks beside the
  existing per-verb asserts (~:698).

## `list` — annotation inspectability

- Gate lines gain ` delay=<N>ps` iff the stored value is nonzero
  (beside `inputs=`/`fanout=`/`nqfanout=` on the same line,
  list.cpp:46). Un-annotated workspaces list byte-identically to
  today (present-only annotation — the 012 pull-display precedent).

## `optimize` — one conditional note (documented O2 reset made visible)

- After an O2 run whose pre-scan found ≥1 nonzero delay, the report
  gains: `note: O2 reshaped the network; reset the propagation delay
  on N gate(s) (docs/logic.md §12)` — mirroring the conditional
  "interface changed" note (optimize.cpp:110). Zero-annotation runs
  print nothing extra (byte-identical).
- O0/O1 behavior unchanged: annotations preserved in place, no note.

## Documentation duties (constitution: README is a living spec)

- README: verb sentence gains `set-delay`/`unset-delay`; a runnable
  example block (place → annotate → `list` shows `delay=1500ps` →
  solve unchanged); test list gains `test_logic_delay`; module
  table's `logic` row mentions delay annotation.
- `docs/logic.md`: §10 verb grammar gains both verbs (grammar,
  exactness rules, zero-value/clear equivalence); §12 Timing slot
  ships the annotation verbs and keeps the delay-honoring evaluator
  as the named follow-on.
- `docs/tests/logic.md`: page for the new binary; catalog audit
  (`docs/audit_catalogs.py`) stays zero-missing/zero-ghost.
- Doxygen comments ship with the new public APIs in the same change
  (`parse_time_ps`, `set_gate_delay`, `clear_gate_delay`, both verb
  spec structs).

## Out of the verb surface (recorded)

- No TUI form, no GUI surface (parity by construction; GUI is a
  separate epic). No `--delay` option on `add` (place, then
  annotate). No completion suggestions for values. No new example
  file (tieoff.yaml already carries nonzero `delay_ps` rows — see
  [workspace-yaml.md](workspace-yaml.md)).
