#pragma once

/**
 * @file
 * Ncurses TUI entry point. Declared without ncurses types so `apps/cli`
 * stays portable; the implementation lives in `src/tui/` (domain TUs under
 * the `wengine_tui` target, built only when Curses is found
 * (`-DBUILD_TUI=ON`, default ON). The TUI is a front end: it owns
 * presentation only and never simulation state — every submitted line
 * runs through the shared CLI path (`wengine/tui/exec.hpp`), and
 * long/batch runs execute on the async worker (`wengine/tui/async.hpp`).
 * Consumer: `apps/cli/main.cpp` behind the process-launch `--tui` flag
 * (not a registry command: a `tui` command inside the REPL would nest
 * terminal modes).
 */

namespace wengine::tui {

/**
 * @brief Enter the full-screen ncurses session and run it until the user
 *        quits.
 *
 * @return The CLI exit code: 0 ok, 1 runtime/environment failure
 *         (non-TTY refusal, unusable TERM, missing curses at launch),
 *         2 usage error. `--tui` takes no further arguments.
 * @note Full-screen terminal mode engages only on a real, cursor-
 *       addressable TTY: non-TTY stdin is refused loudly (exit 1) and
 *       `TERM=dumb`/`unknown`/empty is refused before `initscr` (exit 1,
 *       verified hang-free via PTY) — raw-mode key handling never runs
 *       against a pipe. `NO_COLOR` still runs full-screen, only without
 *       colors. F10/Ctrl-Q quits; a running async task is cancelled at
 *       its step boundary and joined before exit.
 */
int run_tui();

} // namespace wengine::tui
