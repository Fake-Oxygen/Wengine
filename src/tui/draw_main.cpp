// Frame chrome: title bar, active-pane dispatch, completion popup,
// status bar, and the input line with cursor placement.

#include "state.hpp"

#include "wengine/cli/cancel.hpp"
#include "wengine/tui/popup.hpp"
#include "wengine/tui/views.hpp"

#include <curses.h>

#include <cstdint>

namespace wengine::tui {

void draw_scrollback_pane(State &s) {
  const int out_rows = s.out_rows;
  const int cols = s.cols;
  const std::size_t width = cols > 1 ? static_cast<std::size_t>(cols - 1) : std::size_t{1};
  const std::size_t page = out_rows > 0 ? static_cast<std::size_t>(out_rows) : std::size_t{1};
  const views::Wrapped wrapped = views::wrap_lines(s.scrollback, width);
  const std::size_t total = wrapped.rows.size();
  s.scroll = views::clamp_scroll(s.scroll, total, page);
  const std::size_t first = views::display_first(total, page, s.scroll);
  for (int i = 0; i < out_rows; ++i) {
    const std::size_t idx = first + static_cast<std::size_t>(i);
    if (idx >= total) {
      break;
    }
    const std::size_t logical = wrapped.logical_for_row[idx];
    const views::LineKind kind = logical < s.scrollback.size() ? views::classify_line(s.scrollback[logical]) : views::LineKind::kNormal;
    const int pair = views::color_pair_for(kind);
    if (s.use_color && pair != 0) {
      (void)attron(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      // No-color fallback: failures stay visible via bold, never vanish.
      (void)attron(A_BOLD);
    }
    (void)mvaddnstr(1 + i, 0, wrapped.rows[idx].c_str(), cols - 1);
    if (s.use_color && pair != 0) {
      (void)attroff(COLOR_PAIR(pair));
    } else if (!s.use_color && (kind == views::LineKind::kFail || kind == views::LineKind::kError)) {
      (void)attroff(A_BOLD);
    }
    // Search-hit highlight: reverse-video each query occurrence in the
    // visible chunk (chunk-local, so a wrap-split hit still counts as a
    // logical-line match for navigation, it just highlights the pieces).
    if (!s.search_query.empty()) {
      const auto spans = views::match_spans(wrapped.rows[idx], s.search_query);
      for (const auto &sp : spans) {
        (void)mvchgat(1 + i, static_cast<int>(sp.first), static_cast<int>(sp.second), A_REVERSE, 0, nullptr);
      }
    }
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
  std::string status;
  if (s.searching) {
    status = "/" + s.search_query + "  (Enter: find, Esc: cancel)";
  } else if (s.saving) {
    status = "save pane to: " + s.save_path + "  (Enter: save, Esc: cancel)";
  } else if (s.async.busy()) {
    // Progress line while the worker runs: step counts come from the CLI
    // chunk hooks (TUI-5), elapsed from the task clock. Loud cancel hint.
    std::string prog;
    if (cli::progress_active()) {
      const std::uint64_t done = cli::progress_done();
      const std::uint64_t total = cli::progress_total();
      if (total == UINT64_MAX) {
        prog = "steps " + std::to_string(done) + " (forever)";
      } else {
        prog = "steps " + std::to_string(done) + "/" + std::to_string(total);
      }
    } else {
      prog = "starting...";
    }
    status = "running `" + s.async.line() + "` " + prog + " (" + std::to_string(static_cast<int>(s.async.elapsed_s())) + "s) -- Ctrl-C cancels";
    if (s.overlay == Overlay::kNone) {
      const std::size_t width = s.cols > 1 ? static_cast<std::size_t>(s.cols - 1) : std::size_t{1};
      const std::size_t page = s.out_rows > 0 ? static_cast<std::size_t>(s.out_rows) : std::size_t{1};
      const views::Wrapped w = views::wrap_lines(s.scrollback, width);
      const std::size_t total = w.rows.size();
      const int sc = views::clamp_scroll(s.scroll, total, page);
      const std::size_t first = views::display_first(total, page, sc);
      const std::size_t vis = total > first ? (total - first < page ? total - first : page) : 0;
      status += "  [" + views::scroll_indicator(first, vis, total) + "]";
      if (s.scroll > 0) {
        status += " scrolled -PgDn- to follow";
      }
    }
    if (!s.search_query.empty() && !s.search_matches.empty()) {
      status += "  match " + std::to_string(s.search_sel + 1) + "/" + std::to_string(s.search_matches.size()) + " for `" + s.search_query + "` (n/N)";
    }
  } else {
    status = "rc=" + std::to_string(s.last_rc) + "  " + s.status_msg;
    if (s.overlay == Overlay::kNone) {
      const std::size_t width = s.cols > 1 ? static_cast<std::size_t>(s.cols - 1) : std::size_t{1};
      const std::size_t page = s.out_rows > 0 ? static_cast<std::size_t>(s.out_rows) : std::size_t{1};
      const views::Wrapped w = views::wrap_lines(s.scrollback, width);
      const std::size_t total = w.rows.size();
      const int sc = views::clamp_scroll(s.scroll, total, page);
      const std::size_t first = views::display_first(total, page, sc);
      const std::size_t vis = total > first ? (total - first < page ? total - first : page) : 0;
      status += "  [" + views::scroll_indicator(first, vis, total) + "]";
      if (s.scroll > 0) {
        status += " scrolled -PgDn- to follow";
      }
    }
    if (!s.search_query.empty()) {
      if (!s.search_matches.empty()) {
        status += "  match " + std::to_string(s.search_sel + 1) + "/" + std::to_string(s.search_matches.size()) +
                  " for `" + s.search_query + "` (n/N, Esc clears)";
      } else {
        status += "  no match for `" + s.search_query + "` (Esc clears)";
      }
    }
  }
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
      const bool dynamic = !cdef.arguments.empty();
      const int frow = dynamic ? s.form_cursor_row : 3 + static_cast<int>(s.form_field);
      const int fcol = static_cast<int>(prefix.size() + (s.form_col - foff));
      if (dynamic && s.form_active_pane == 1) {
        (void)wmove(stdscr, rows - 1, static_cast<int>(kPrompt.size() + (s.cursor - offset)));
      } else if (frow >= 1 && frow < rows - 2) {
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
  // Search/save prompts live on the status line: park the cursor there
  // while typing so resize never strands it off-screen.
  if (s.searching) {
    const int ccol = 1 + static_cast<int>(s.search_query.size());
    (void)wmove(stdscr, rows - 2, ccol < cols - 1 ? ccol : cols - 1);
  } else if (s.saving) {
    const std::string pre = "save pane to: ";
    const int ccol = static_cast<int>(pre.size() + s.save_path.size());
    (void)wmove(stdscr, rows - 2, ccol < cols - 1 ? ccol : cols - 1);
  }
  (void)wrefresh(stdscr);
}

} // namespace wengine::tui
