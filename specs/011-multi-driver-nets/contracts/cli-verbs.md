# Contract: CLI Verb Surface — Shared Nets (011)

**Seam**: `include/wengine/cli/logic_registry.hpp`,
`src/cli/commands/logic/*.cpp`. One command domain (`logic`) — no new
top-level command. Interactive/headless byte parity by construction
(`TuiExecutor` → `cli::Shell`; no TUI change, no logic form exists).

## `wire` — gains `--shared`

```
wire <FROM.out> <TO.in> [--shared]
```

- Without the flag: exactly today's behavior, byte-identical — an
  occupied **unmarked** destination refuses with the existing
  "input already wired (unwire it first)" usage error (exit 2).
- With `--shared` on an unmarked destination: marks the input and
  attaches (the explicit opt-in); on an already-marked destination:
  accepted idempotently and attaches.
- Plain `wire` (no flag) on a **marked** destination attaches without
  the flag — the mark is the user's prior declaration; the success
  line names the shared state so nothing rides silently, e.g.
  `wired AND1.Y -> BUS.1 (shared: 2 drivers)`.
- All existing wire discipline applies unchanged per attachment:
  pin parsing, direction, tied refusal, cycle refusal (including the
  sequential tentative-probe path, which must probe/revert the
  extra-append), post-wire async/level settle with SR-illegal revert.
- Cap overflow: loud usage error naming the cap, netlist untouched.
- Completion: `--shared` joins `kOpts` (`--help`, `-h`,
  `--shared`); no value form; pin completion unchanged.
- Exit codes: 0 success; 2 usage (occupied-unmarked, cap, stray,
  bad pins); 1 runtime (settle failures), per the module's standing
  convention.

## `unshare <TO.in>` — new verb (LogicRegistry 22 → 23)

- Clears the mark on a marked input holding ≤1 attachment: prints
  confirmation naming the input and remaining attachments.
- Refused loudly (usage, exit 2) while the input holds ≥2 attachments
  ("unwire extras first") and on an unmarked input ("input is not
  shared"); tied inputs are never marked, so they hit the unmarked
  refusal.
- Completion: input pins (`collect_pins` inputs side), `--help`/`-h`;
  no options with values.
- Registered per the module recipe: spec struct in
  `logic_registry.hpp`, appended to `LogicRegistry`, `.cpp` added to
  `wengine_cli` sources, `LogicSpec::kUsage` verb list updated.
- Registry pins to sync in `tests/test_cli_args.cpp`:
  `LogicRegistry::all().size() == 23` (was 22, line ~:689), the
  per-verb `find` block, `verbs.size()` asserts (~:813/:819), and the
  `logic u` completion vector gains `unshare` in registry order
  (~:1763-1764).

## `solve` / `step` — contention readback

- When the workspace contains **any** marked input, `solve` appends a
  contention status line after its value lines; when none are marked,
  output is byte-identical to today (FR-015). Shape (per-net, then
  total; exact wording pinned in tasks/quickstart):
  `contention: <NET>=<N> [<NET>=<N> ...] (total <T>)` — present with
  zeros (`total 0`) so counting is visibly live, never opt-in.
- `step` appends the same line to its per-step report when marked
  inputs exist. Counts accumulate across the session in the
  `SolveDiag` channel.
- `reset` clears the counts with the other session state — never
  serialized, never in `save` output; a saved file is byte-identical
  with or without contention history (FR-009).

## `list` — mark inspectability

- Gate/input listing shows shared marks: per marked input, the mark
  and attachment count (e.g. an annotation beside the gate's input
  list). Unshared netlists list byte-identically to today.

## `truth` — loud refusal

- Over a netlist where any marked input holds ≥2 attachments: loud
  failure naming the construct and pointing at scalar `solve`
  (four-value path); exit 1 runtime failure, never a silent
  misrepresentation. Marked inputs with ≤1 attachment stream
  unchanged.

## Completion coverage (FR-016)

- `logic wire --` completes `--shared` (plus `--help`, `-h`).
- `logic unsh` completes `unshare`; `logic unshare ` completes input
  pins; `logic unshare --` completes `--help`/`-h`.
- No `--opt=` value forms are added.

## Documentation duties (constitution: README is a living spec)

- README: verb sentence gains `unshare` and the `--shared` flow; a
  runnable example block; test list gains the new binary; module
  table's `logic` row mentions allowlisted shared nets.
- `docs/logic.md`: §10 verb grammar gains `unshare` and
  `wire --shared`; §9 single-driver rule text gains the per-input
  opt-in; §11 non-goals drops "multi-driver nets"; §12 Drivers slot
  changes from "netlist wiring reserved" to shipped authoring (funnel
  + allowlist + counting), with the O2 follow-on named.
- `docs/tests/logic.md`: page for the new binary; catalog audit stays
  zero-missing/zero-ghost.
