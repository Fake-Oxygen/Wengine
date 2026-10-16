# TUI (ncurses) — design + decision log

`PhysicsSimApp --tui` opens a terminal UI with the full CLI command set.
Status: scaffolding + palette/help browser + closed-form guided forms
(TUI-1..3); async runs and output views are roadmap tasks in Plane
(parent: TUI epic).

## 1. Reuse decision (D-TUI-1)

The TUI reuses the CLI command path verbatim and never forks one:

```text
TUI input line -> tui::TuiExecutor -> cli::Shell::execute(line, ostringstream)
                                          -> CliRegistry::find -> Spec::run
               -> captured text rendered into the scrollback pane
Tab            -> cli::complete_line over CliRegistry::all()
history        -> cli::History (same file as the REPL)
```

Consequences:

- TUI output is byte-identical to the REPL/headless CLI by construction;
  `tests/test_tui_exec.cpp` pins executor-vs-`Shell` parity (codes + text).
- New commands work in the TUI with no TUI-side change (same guarantee as
  `sim run --solver`: registry-driven).
- Guided forms added later must only *compose argv strings* for the
  executor; any form that reimplements parsing/validation is a bug.
- `--tui` is a process-launch flag in `apps/cli/main.cpp`, not a registry
  command (a `tui` command inside the REPL would nest terminal modes).

This is deliberately different from the GUI, which links `wengine_core`
directly and needs explicit parity tests per panel. The TUI links
`wengine_cli` (the one sanctioned exception to the apps-never-cli shape).

## 2. Dependency decision (D-TUI-2)

ncurses is a **system package** (`libncurses-dev`), in the same category as
SDL2 + the Vulkan loader for the GUI: never vendored, no version pin to
move. `BUILD_TUI` defaults `ON`; when Curses is missing the CLI still
builds and `--tui` prints a rebuild hint (exit 1). No engine headers
change for TUI work; `wengine/tui/exec.hpp` is ncurses-free.

## 3. Shipped scope

Scaffolding (TUI-1): `include/wengine/tui/exec.hpp` (executor seam),
`include/wengine/tui/run.hpp` + `src/tui/run.cpp` (`wengine_tui` STATIC TU),
`--tui` flag, `BUILD_TUI` option, `tests/test_tui_exec.cpp`, README updates.

Palette + help browser (TUI-2): `include/wengine/tui/palette.hpp`
(registry-derived entries + prefix filter + F1 context token, all
ncurses-free and unit-tested). F2 opens the palette (type to filter,
Enter inserts `<name> ` into the input line, F1 opens help for the
highlighted entry, Esc closes). F1 anywhere opens a scrollable help pane
whose content is `help <cmd>` (or bare `help`) run through the executor --
byte-identical to the CLI, including error shapes. The pane stays pinned
while composing: typing, editing, history recall, and Tab all work with
syntax visible; Enter runs the command and closes the pane onto the output.
F2 from the pane switches back to the palette. Tab multi-candidates render
as a transient reverse-video popup just above the input line (wrapped,
capped with a loud `+N more`, never into the scrollback); any non-Tab key
dismisses it, single/prefix completion behaves as in the REPL.
Viewing help never clobbers the status bar's last-command exit code. TUI chrome strings stay
ASCII-only (C-locale safety: plain ncurses renders non-ASCII bytes as
control notations and miscounts their width -- found via PTY in TUI-2;
CLI output is ASCII-only so panes stay exact). Shared loop notes:
minimal editing, Up/Down shared-history recall, Tab completion mirroring
the REPL, F10/Ctrl-Q quit, `KEY_RESIZE`-safe redraw, non-TTY refusal
(exit 1), `--tui` with extra args is a usage error (exit 2).

Closed-form forms (TUI-3): `include/wengine/tui/forms.hpp`
(ncurses-free composition model + registry-derived `ic` chips/ops,
unit-tested). F3 opens the form browser (all six closed-form families),
Enter opens the highlighted form, Enter inside the form composes one argv
string and submits it through `TuiExecutor` -- the result pane shows the
CLI-identical text beside the fields (also mirrored into the scrollback +
shared history). Field hints come from each spec's `kUsage`/completion
vocabulary; `ic` chips/ops enumerate `IcRegistry` (plus each chip's
`complete`), so new parts appear with no form-side change. Forms never
validate: empty fields are omitted and the CLI owns every error, so
invalid input shows the CLI usage-error text verbatim.
`tests/test_tui_forms.cpp` pins the composition + executor-vs-`Shell`
parity matrix (every option, every `ic` op, plus usage-error shapes).
Verified on a real PTY (F3 browser, ohm prefill submit `R = 22.963`,
Esc back, scrollback mirror).

## 4. Known limits (roadmap, not bugs)

- One command's displayed output is capped at 500 lines (loud truncation
  note). Full logs belong in `--emit-csv` files.
- Long batch runs block input while computing (same as the REPL); async
  runs with progress + step-boundary cancel are a roadmap task.
- No mouse, no scrollback search/save yet. Forms cover the closed-form
  families only (`ohm`/`s0`/`i2c`/`ldo`/`led`/`ic`); `sim`/`sig` builders
  are a roadmap task.

## 5. Manual verification

No input-automation tooling exists (same as the GUI panels): run
`./build/PhysicsSimApp --tui` on a real TTY and check help display,
completion, history recall across restarts, resize, and a long-run
truncation note. A PTY-driven smoke test is a roadmap task.
