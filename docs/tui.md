# TUI (ncurses) — design + decision log

`PhysicsSimApp --tui` opens a terminal UI with the full CLI command set.
Status: scaffolding (executor seam + minimal loop); pretty per-command
forms and async runs are roadmap tasks in Plane (parent: TUI epic).

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

## 3. Scaffolding scope (done)

- `include/wengine/tui/exec.hpp` — header-only executor seam (Shell +
  completion delegation), no ncurses.
- `include/wengine/tui/run.hpp` + `src/tui/run.cpp` — `wengine_tui`
  STATIC TU: header bar, scrollback pane (cap 2000 lines), input line
  (arrows/Home/End/Backspace/Ctrl-U), Up/Down history, Tab completion
  (single/prefix/list, mirroring the REPL), F1 help, F10/Ctrl-Q quit,
  status bar with last exit code, color when available with a no-color
  fallback, `KEY_RESIZE`-safe redraw.
- Non-TTY `--tui` is refused loudly (exit 1); `--tui` with extra args is a
  usage error (exit 2), matching the stray-positional rule.

## 4. Known scaffolding limits (roadmap, not bugs)

- One command's displayed output is capped at 500 lines (loud truncation
  note). Full logs belong in `--emit-csv` files.
- Long batch runs block input while computing (same as the REPL); async
  runs with progress + step-boundary cancel are a roadmap task.
- No mouse, no per-command forms yet, no scrollback search/save yet.

## 5. Manual verification

No input-automation tooling exists (same as the GUI panels): run
`./build/PhysicsSimApp --tui` on a real TTY and check help display,
completion, history recall across restarts, resize, and a long-run
truncation note. A PTY-driven smoke test is a roadmap task.
