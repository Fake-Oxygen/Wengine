#pragma once

// Ncurses TUI entry point. Declared without ncurses types so `apps/cli`
// stays portable; the implementation lives in `src/tui/run.cpp` under
// the `wengine_tui` target, built only when Curses is found
// (`-DBUILD_TUI=ON`, default ON).
//
// Returns the CLI exit code: 0 ok, 1 runtime/environment failure,
// 2 usage error. `--tui` takes no further arguments.

namespace wengine::tui {

int run_tui();

} // namespace wengine::tui
