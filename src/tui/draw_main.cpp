// Frame chrome: title bar, active-pane dispatch, completion popup,
// status bar, and the input line with cursor placement.

#include "state.hpp"

#include "wengine/tui/popup.hpp"

#include <curses.h>

namespace wengine::tui {

void draw_scrollback_pane(State &s) {
  const int out_rows = s.out_rows;
  const int cols = s.cols;
  const std::size_t page = static_cast<std::size_t>(out_rows);
  const std::size_t total = s.scrollback.size();
  std::size_t max_scroll = 0;
  if (total > page) {
    max_scroll = total - page;
  }
  if (s.scroll > static_cast<int>(max_scroll)) {
    s.scroll = static_cast<int>(max_scroll);
  }
  if (s.scroll < 0) {
    s.scroll = 0;
  }
  const std::size_t first = max_scroll - static_cast<std::size_t>(s.scroll) > max_scroll ? 0 : max_scroll - static_cast<std::size_t>(s.scroll);
  for (int i = 0; i < out_rows; ++i) {
    const std::size_t idx = first + static_cast<std::size_t>(i);
    if (idx >= total) {
      break;
    }
    (void)mvaddnstr(1 + i, 0, s.scrollback[idx].c_str(), cols - 1);
  }
}

void draw_frame(State &s) {
  const int rows = s.rows;
  const int cols = s.cols;
  const int out_rows = s.out_rows;

  // --- title ---
  (void)werase(stdscr);
  if (s.use_color) {
    (void)attron(COLOR_PAIR(1));
  }
  (void)mvaddnstr(0, 0, kTitle.data(), cols - 1);
  (void)wclrtoeol(stdscr);
  if (s.use_color) {
    (void)attroff(COLOR_PAIR(1));
  }

  // --- active pane ---
  if (s.overlay == Overlay::kHelp) {
    draw_help_pane(s);
  } else if (s.overlay == Overlay::kPalette) {
    draw_palette_pane(s);
  } else if (s.overlay == Overlay::kForms) {
    draw_forms_pane(s);
  } else if (s.overlay == Overlay::kBatches) {
    draw_batches_pane(s);
  } else if (s.overlay == Overlay::kForm) {
    draw_form_pane(s);
  } else if (s.overlay == Overlay::kBatch) {
    draw_batch_pane(s);
  } else {
    draw_scrollback_pane(s);
  }

  // Transient completion popup just above the input line (never in the
  // palette, which has its own filter UI). Any non-Tab key already
  // dismissed it above; Tab refreshes it.
  if (s.overlay != Overlay::kPalette && !s.complete_names.empty()) {
    const std::size_t width = static_cast<std::size_t>(cols - 1);
    std::size_t max_rows = 6;
    if (max_rows > static_cast<std::size_t>(out_rows)) {
      max_rows = static_cast<std::size_t>(out_rows);
    }
    const std::vector<std::string> popup = wrap_popup_lines(s.complete_names, width, max_rows);
    const int nrows = static_cast<int>(popup.size());
    for (int i = 0; i < nrows; ++i) {
      (void)attron(A_REVERSE);
      (void)mvaddnstr(out_rows - nrows + 1 + i, 0, popup[static_cast<std::size_t>(i)].c_str(), cols - 1);
      (void)wclrtoeol(stdscr);
      (void)attroff(A_REVERSE);
    }
  }

  // --- status bar ---
  if (s.use_color) {
    (void)attron(COLOR_PAIR(s.last_rc == 0 ? 2 : 3));
  }
  const std::string status = "rc=" + std::to_string(s.last_rc) + "  " + s.status_msg + (s.overlay == Overlay::kNone && s.scroll > 0
                                  ? "  [scrolled -PgDn- to follow]" : "");
  (void)mvaddnstr(rows - 2, 0, status.c_str(), cols - 1);
  (void)wclrtoeol(stdscr);
  if (s.use_color) {
    (void)attroff(COLOR_PAIR(s.last_rc == 0 ? 2 : 3));
  }

  // --- input line ---
  const std::size_t avail = static_cast<std::size_t>(cols) - kPrompt.size() - 1;
  std::size_t offset = 0;
  if (s.input.size() > avail) {
    offset = s.input.size() - avail;
  }
  if (s.cursor < offset) {
    offset = s.cursor;
  }
  const std::string visible = s.input.substr(offset, avail);
  (void)mvaddstr(rows - 1, 0, kPrompt.data());
  (void)addnstr(visible.c_str(), static_cast<int>(avail));
  (void)wclrtoeol(stdscr);
  if (s.overlay == Overlay::kForm && s.open_form >= 0 && static_cast<std::size_t>(s.open_form) < s.form_defs.size()) {
    // Focus stays in the active form field (bottom line keeps its draft).
    const std::size_t sfi = static_cast<std::size_t>(s.open_form);
    const forms::FormDef &cdef = s.form_defs[sfi];
    if (s.form_field < cdef.fields.size()) {
      const auto &ff = cdef.fields[s.form_field];
      const std::string key_disp = ff.key.empty() ? "(pos)" : std::string(ff.key);
      const std::string prefix = "  " + std::string(ff.label) + " " + key_disp + ": ";
      const std::string &fval = s.form_vals[sfi][s.form_field];
      if (s.form_col > fval.size()) {
        s.form_col = fval.size();
      }
      const std::size_t favail = static_cast<std::size_t>(cols) > prefix.size() + 1 ? static_cast<std::size_t>(cols) - prefix.size() - 1 : std::size_t{1};
      std::size_t foff = 0;
      if (fval.size() > favail) {
        foff = fval.size() - favail;
      }
      if (s.form_col < foff) {
        foff = s.form_col;
      }
      const int frow = 3 + static_cast<int>(s.form_field);
      const int fcol = static_cast<int>(prefix.size() + (s.form_col - foff));
      if (frow >= 1 && frow < rows - 2) {
        (void)wmove(stdscr, frow, fcol < cols - 1 ? fcol : cols - 1);
      } else {
        (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
      }
    } else {
      (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
    }
  } else if (s.overlay == Overlay::kBatch && s.open_batch >= 0 && static_cast<std::size_t>(s.open_batch) < s.batch_defs.size()) {
    // Focus stays in the active builder field; the viewport scrolls via
    // batch_top (see draw), so the cursor row is relative to it.
    const std::size_t sbi = static_cast<std::size_t>(s.open_batch);
    const batches::BatchDef &cdef = s.batch_defs[sbi];
    if (s.batch_field < cdef.fields.size()) {
      const auto &ff = cdef.fields[s.batch_field];
      const std::string prefix = "  " + std::string(ff.label) + " " + std::string(ff.key) + ": " + (ff.flag ? "[flag] " : "");
      const std::string &fval = s.batch_vals[sbi][s.batch_field];
      if (s.batch_col > fval.size()) {
        s.batch_col = fval.size();
      }
      const std::size_t favail = static_cast<std::size_t>(cols) > prefix.size() + 1 ? static_cast<std::size_t>(cols) - prefix.size() - 1 : std::size_t{1};
      std::size_t foff = 0;
      if (fval.size() > favail) {
        foff = fval.size() - favail;
      }
      if (s.batch_col < foff) {
        foff = s.batch_col;
      }
      const std::size_t top = s.batch_top[sbi] <= s.batch_field ? s.batch_top[sbi] : s.batch_field;
      const int frow = 3 + static_cast<int>(s.batch_field - top);
      const int fcol = static_cast<int>(prefix.size() + (s.batch_col - foff));
      if (frow >= 1 && frow < rows - 2) {
        (void)wmove(stdscr, frow, fcol < cols - 1 ? fcol : cols - 1);
      } else {
        (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
      }
    } else {
      (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
    }
  } else {
    (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
  }
  (void)wrefresh(stdscr);
}

} // namespace wengine::tui
