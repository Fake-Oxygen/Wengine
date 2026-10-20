#pragma once

// Private loop state for the ncurses front end (`src/tui/`).
//
// `run.cpp` stays thin assembly (terminal setup, frame loop, shutdown);
// every work domain owns its TU and operates on this struct:
// - `state.cpp`: one-time setup (defs, values, prefills, history, welcome)
// - `text.cpp`: scrollback/help text utilities (no terminal handling)
// - `submit.cpp`: input-line / form / builder submission through the executor
// - `edit.cpp`: shared input-line editing + Tab completion
// - `draw_main.cpp`: frame chrome (title, scrollback, popup, status, input)
// - `panes_help.cpp`: palette browser + help pane (draw + keys)
// - `panes_forms.cpp`: closed-form browser + editor (draw + keys)
// - `panes_batches.cpp`: run-builder browser + editor (draw + keys)
// - `keys.cpp`: top-level key dispatch + main-line keys
//
// Nothing here includes `<curses.h>`: the struct carries no terminal types
// ( drawing TUs include it themselves). The reuse contract is unchanged:
// every submission goes through `TuiExecutor` (i.e.
// `cli::Shell::execute`), so panes never parse commands or call solvers.

#include "wengine/cli/line_reader.hpp"
#include "wengine/tui/async.hpp"
#include "wengine/tui/batches.hpp"
#include "wengine/tui/exec.hpp"
#include "wengine/tui/forms.hpp"
#include "wengine/tui/palette.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui {

constexpr int kCtrlQ = 'Q' - '@';
constexpr int kCtrlC = 'C' - '@';
constexpr int kCtrlU = 'U' - '@';
constexpr int kCtrlB = 'B' - '@';
constexpr int kEsc = 27;
constexpr int kBackspaceAscii = 127;
constexpr std::size_t kMaxScrollback = 2000;
constexpr std::size_t kMaxOutputLines = 500;

constexpr std::string_view kTitle =
    "WEngine TUI  |  Enter: run  Tab: complete  F1: help  F2: commands  "
    "F3: forms  F4: runs  /:search n:next F5:save  PgUp/PgDn: scroll  F10/Ctrl-Q: quit";
constexpr std::string_view kPrompt = "> ";

enum class Overlay { kNone, kPalette, kHelp, kForms, kForm, kBatches, kBatch };

enum class AsyncOrigin { kNone, kLine, kForm, kBatch };

// All mutable state of one TUI session. The frame loop in `run.cpp` owns a
// single instance; domain TUs only borrow it.
struct State {
  // Session.
  TuiExecutor exec;
  std::vector<PaletteEntry> pal_entries;
  cli::History history;
  std::string hist_path;
  std::vector<std::string> scrollback;

  // Input line (main view + pinned help pane share it).
  std::string input;
  std::size_t cursor = 0;
  std::string draft;
  int hist_pos = -1; // -1: editing fresh line, else index into history
  int scroll = 0;
  int last_rc = 0;
  std::string status_msg;
  Overlay overlay = Overlay::kNone;

  // Palette + help.
  std::string pal_filter;
  std::size_t pal_sel = 0;
  std::string help_title;
  std::vector<std::string> help_lines;
  std::size_t help_first = 0; // first visible help line (docs read top-down)
  std::vector<std::string> complete_names; // transient Tab popup (non-Tab clears)
  bool quit = false;

  // Guided forms (TUI-3): one argv-composing editor per closed-form family.
  std::vector<forms::FormDef> form_defs;
  std::vector<std::vector<std::string>> form_vals;
  std::size_t forms_sel = 0;
  int open_form = -1; // index into form_defs while Overlay::kForm is active
  std::size_t form_field = 0; // 0..fields.size() (last row = Submit)
  std::size_t form_col = 0; // cursor within the active field value
  std::vector<std::uint64_t> form_arg_selected;
  std::vector<int> form_active_profile;
  std::size_t form_arg_sel = 0;
  int form_active_pane = 0; // 0: command builder, 1: argument choices
  bool form_pane_prefix = false; // tmux-style Ctrl-B prefix
  int form_cursor_row = 3;
  std::vector<ExecResult> form_res;
  std::vector<bool> form_has;
  std::vector<std::string> form_argv;
  std::vector<std::size_t> form_res_first;

  // Run builders (TUI-4): one argv-composing editor per batch command.
  std::vector<batches::BatchDef> batch_defs;
  std::vector<std::vector<std::string>> batch_vals;
  std::size_t batches_sel = 0;
  int open_batch = -1; // index into batch_defs while Overlay::kBatch is active
  std::size_t batch_field = 0; // 0..fields.size() (last row = Submit)
  std::size_t batch_col = 0; // cursor within the active field value
  std::vector<ExecResult> batch_res;
  std::vector<bool> batch_has;
  std::vector<std::string> batch_argv;
  std::vector<std::size_t> batch_res_first;
  std::vector<std::size_t> batch_top; // field viewport scroll

  // Per-frame geometry (set by the loop before drawing) + terminal caps.
  int rows = 0;
  int cols = 0;
  int out_rows = 0;
  bool use_color = false;

  // Output views (TUI-6): scrollback search + save-pane-to-file. The search
  // runs on the captured logical lines (never on wrapped display rows), so
  // it finds across truncation notes and paged-out history. `searching` /
  // `saving` are transient prompt modes owned by the main-view key handler;
  // `search_matches`/`search_sel` survive for `n`/`N` navigation until the
  // next search or an Esc clear.
  bool searching = false;
  std::string search_query;
  std::vector<std::size_t> search_matches;
  std::size_t search_sel = 0;
  bool saving = false;
  std::string save_path;

  // Async worker (TUI-5): one CLI line off the UI thread. Input stays live
  // while it runs; Ctrl-C requests a step-boundary stop. Only the frame
  // loop polls/takes (`poll_async`); submissions start, completions route
  // by `async_origin`/`async_index`.
  AsyncTask async;
  AsyncOrigin async_origin = AsyncOrigin::kNone;
  std::size_t async_index = 0;
};

// One-time setup: defs, value stores, demo prefills, shared history load,
// welcome text. No terminal handling.
void init_state(State &s);

// Submission through the executor (CLI owns every verdict).
// Long sim/sig batches run on the async worker (TUI-5); the frame loop
// polls completion via `poll_async` so input stays live.
void submit_line(State &s);
void submit_form(State &s);
void submit_batch(State &s);
bool poll_async(State &s);
void finish_async(State &s, const AsyncResult &r);

// Shared input-line editing + Tab completion. Returns true when consumed.
bool edit_key(State &s, int edit_ch);

// Frame drawing (title, active pane, popup, status bar, input line).
void draw_frame(State &s);

// Top-level key dispatch (quit + per-overlay handlers + main-line keys).
void handle_key(State &s, int ch);

// Text utilities (text.cpp).
void split_capped(std::vector<std::string> &out, std::string_view text, std::size_t max_lines, std::size_t &dropped);
void push_text(std::vector<std::string> &scrollback, std::string_view text, std::size_t max_lines);
std::vector<std::string> fetch_help(TuiExecutor &exec, std::string_view target, int &rc_out);
void token_bounds(std::string_view buf, std::size_t cursor, std::size_t &begin, std::size_t &end);

// Per-pane drawing (panes_*.cpp, drawn by draw_frame).
void draw_palette_pane(State &s);
void draw_help_pane(State &s);
void draw_forms_pane(State &s);
void draw_batches_pane(State &s);
void draw_form_pane(State &s);
void draw_batch_pane(State &s);
void draw_scrollback_pane(State &s);

// Per-overlay key handlers (panes_*.cpp + keys.cpp, called by handle_key).
void palette_key(State &s, int ch);
void help_key(State &s, int ch);
void forms_key(State &s, int ch);
void batches_key(State &s, int ch);
void form_key(State &s, int ch);
void batch_key(State &s, int ch);
void main_key(State &s, int ch);

} // namespace wengine::tui
