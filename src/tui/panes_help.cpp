// Command palette + help browser panes (draw + keys).
//
// Entries derive from `CliRegistry` (see `tui/palette.hpp`) so new commands
// appear for free; help panes run `help <cmd>` through the executor,
// keeping them byte-identical to the CLI by construction.

#include "state.hpp"

#include <curses.h>

#include <algorithm>

namespace wengine::tui {

void draw_palette_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  const std::vector<std::size_t> view = filter_entries(s.pal_entries, s.pal_filter);
  const std::size_t sel = view.empty() ? 0 : std::min(s.pal_sel, view.size() - 1);
  (void)mvaddnstr(1, 0, "Commands -- type to filter | Enter: insert | F1: help | Esc: close", cols - 1);
  (void)wclrtoeol(stdscr);
  const std::string filt = "filter: " + s.pal_filter;
  (void)mvaddnstr(2, 0, filt.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);
  const std::size_t page = out_rows > 2 ? static_cast<std::size_t>(out_rows - 2) : std::size_t{1};
  std::size_t first = 0;
  if (!view.empty() && sel >= page) {
    first = sel - page + 1; // keep the highlight on screen
  }
  if (view.empty()) {
    (void)mvaddnstr(3, 0, "(no match)", cols - 1);
  }
  for (std::size_t i = 0; i < page; ++i) {
    const std::size_t vi = first + i;
    if (vi >= view.size()) {
      break;
    }
    const PaletteEntry &e = s.pal_entries[view[vi]];
    const std::string row = "  " + e.name + " - " + e.description;
    if (vi == sel) {
      (void)attron(A_REVERSE);
    }
    (void)mvaddnstr(static_cast<int>(3 + i), 0, row.c_str(), cols - 1);
    if (vi == sel) {
      (void)attroff(A_REVERSE);
    }
  }
}

void draw_help_pane(State &s) {
  const int cols = s.cols;
  const int out_rows = s.out_rows;
  const std::string head = s.help_title + "  (type to compose, Enter runs + closes, PgUp/PgDn scroll, Esc closes)";
  (void)mvaddnstr(1, 0, head.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);
  const std::size_t page = out_rows > 1 ? static_cast<std::size_t>(out_rows - 1) : std::size_t{1};
  std::size_t max_first = 0;
  if (s.help_lines.size() > page) {
    max_first = s.help_lines.size() - page;
  }
  if (s.help_first > max_first) {
    s.help_first = max_first;
  }
  for (std::size_t i = 0; i < page; ++i) {
    const std::size_t idx = s.help_first + i;
    if (idx >= s.help_lines.size()) {
      break;
    }
    (void)mvaddnstr(static_cast<int>(2 + i), 0, s.help_lines[idx].c_str(), cols - 1);
  }
}

void palette_key(State &s, int ch) {
  const std::vector<std::size_t> view = filter_entries(s.pal_entries, s.pal_filter);
  const std::size_t sel = view.empty() ? 0 : std::min(s.pal_sel, view.size() - 1);
  if (ch == kEsc || ch == kCtrlC) {
    s.overlay = Overlay::kNone;
    s.status_msg = "palette closed";
  } else if (ch == KEY_UP) {
    if (sel > 0) {
      s.pal_sel = sel - 1;
    }
  } else if (ch == KEY_DOWN) {
    if (!view.empty() && sel + 1 < view.size()) {
      s.pal_sel = sel + 1;
    }
  } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
    if (!s.pal_filter.empty()) {
      s.pal_filter.pop_back();
      s.pal_sel = 0;
    }
  } else if (ch == KEY_F(1)) {
    if (!view.empty()) {
      const PaletteEntry &e = s.pal_entries[view[sel]];
      int help_rc = 0;
      s.help_lines = fetch_help(s.exec, e.name, help_rc);
      s.help_title = "help " + e.name;
      s.help_first = 0;
      s.overlay = Overlay::kHelp;
      s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
    }
  } else if (ch == KEY_F(3)) {
    s.forms_sel = 0;
    s.overlay = Overlay::kForms;
    s.status_msg = "forms -- " + std::to_string(s.form_defs.size()) + " calculators";
  } else if (ch == KEY_F(4)) {
    s.batches_sel = 0;
    s.overlay = Overlay::kBatches;
    s.status_msg = "builders -- " + std::to_string(s.batch_defs.size()) + " batch runs";
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    if (!view.empty()) {
      const PaletteEntry &e = s.pal_entries[view[sel]];
      s.input = e.name + " ";
      s.cursor = s.input.size();
      s.hist_pos = -1;
      s.overlay = Overlay::kNone;
      s.status_msg = "inserted `" + e.name + "` -- edit args, Enter to run";
    } else {
      s.status_msg = "no match -- keep typing or Esc";
    }
  } else if (ch >= 32 && ch < 127) {
    s.pal_filter.push_back(static_cast<char>(ch));
    s.pal_sel = 0;
  }
}

void help_key(State &s, int ch) {
  // Pinned help: the input line stays live so syntax stays visible
  // while composing. Up/Down recall history (PgUp/PgDn scroll the pane),
  // Enter runs and keeps the pane open, Esc/F1 closes it.
  const std::size_t help_page = s.out_rows > 1 ? static_cast<std::size_t>(s.out_rows - 1) : std::size_t{1};
  if (ch == kEsc || ch == KEY_F(1)) {
    s.overlay = Overlay::kNone;
    s.status_msg = "ready";
  } else if (ch == KEY_F(2)) {
    s.pal_filter.clear();
    s.pal_sel = 0;
    s.overlay = Overlay::kPalette;
    s.status_msg = "palette -- " + std::to_string(s.pal_entries.size()) + " commands";
  } else if (ch == KEY_F(3)) {
    s.forms_sel = 0;
    s.overlay = Overlay::kForms;
    s.status_msg = "forms -- " + std::to_string(s.form_defs.size()) + " calculators";
  } else if (ch == KEY_F(4)) {
    s.batches_sel = 0;
    s.overlay = Overlay::kBatches;
    s.status_msg = "builders -- " + std::to_string(s.batch_defs.size()) + " batch runs";
  } else if (ch == KEY_PPAGE) {
    s.help_first = s.help_first >= help_page ? s.help_first - help_page : 0;
  } else if (ch == KEY_NPAGE) {
    s.help_first += help_page; // clamped at draw time
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    // Running from help closes the pane onto the result.
    submit_line(s);
    s.overlay = Overlay::kNone;
  } else {
    edit_key(s, ch);
  }
}

} // namespace wengine::tui
