// Top-level key dispatch + main-line keys (the kNone overlay:
// F-key pane switches, context help, scrollback paging, submit, editing).

#include "state.hpp"

#include "wengine/tui/views.hpp"

#include <curses.h>

namespace wengine::tui {

namespace {

// Bring logical match `logical` into view in display-row space (wrapped).
void jump_to_logical(State &s, std::size_t logical) {
  const std::size_t width = s.cols > 1 ? static_cast<std::size_t>(s.cols - 1) : std::size_t{1};
  const std::size_t page = s.out_rows > 0 ? static_cast<std::size_t>(s.out_rows) : std::size_t{1};
  const views::Wrapped w = views::wrap_lines(s.scrollback, width);
  if (w.row_start_for_logical.size() <= logical) {
    return;
  }
  const std::size_t target = w.row_start_for_logical[logical];
  s.scroll = views::clamp_scroll(views::scroll_to_show(target, w.rows.size(), page), w.rows.size(), page);
}

void jump_to_sel(State &s) {
  if (s.search_matches.empty()) {
    return;
  }
  if (s.search_sel >= s.search_matches.size()) {
    s.search_sel = s.search_matches.size() - 1;
  }
  jump_to_logical(s, s.search_matches[s.search_sel]);
}

void confirm_search(State &s) {
  s.searching = false;
  s.search_matches = views::find_matches(s.scrollback, s.search_query);
  s.search_sel = 0;
  if (s.search_matches.empty()) {
    s.status_msg = s.search_query.empty() ? "search cleared" : "no match for `" + s.search_query + "`";
  } else {
    jump_to_sel(s);
    s.status_msg = "match 1/" + std::to_string(s.search_matches.size()) + " for `" + s.search_query + "` (n/N)";
  }
}

}  // namespace

void main_key(State &s, int ch) {
  // Search prompt takes over the main view first (save prompts are global
  // in `handle_key` so F5 works from any pane).
  if (s.searching) {
    if (ch == kEsc) {
      s.searching = false;
      s.status_msg = "search kept `" + s.search_query + "` (Esc clears)";
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      confirm_search(s);
    } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
      if (!s.search_query.empty()) {
        s.search_query.pop_back();
      }
    } else if (ch == KEY_DC) {
      s.search_query.clear();
    } else if (ch == kCtrlU || ch == kCtrlC) {
      s.search_query.clear();
    } else if (ch >= 32 && ch < 127) {
      s.search_query.push_back(static_cast<char>(ch));
    }
    return;
  }
  if (ch == KEY_F(2)) {
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
  } else if (ch == KEY_F(1)) {
    const std::string_view tok = first_token(s.input);
    int help_rc = 0;
    s.help_lines = fetch_help(s.exec, tok, help_rc);
    s.help_title = tok.empty() ? "help" : "help " + std::string(tok);
    s.help_first = 0;
    s.overlay = Overlay::kHelp;
    s.status_msg = s.help_title + " (exit " + std::to_string(help_rc) + ", Esc to close)";
  } else if (ch == KEY_RESIZE) {
    // Layout is recomputed from getmaxyx() every frame; nothing to do.
  } else if (ch == KEY_F(5)) {
    // Save scrollback pane (from the main view; the prompt lives on the
    // status line so resize never strands it).
    if (s.save_path.empty()) {
      s.save_path = "tui_output.txt";
    }
    s.saving = true;
    s.status_msg = "save scrollback -- Enter writes, Esc cancels";
  } else if (ch == '/' && s.input.empty()) {
    // Bare `/` starts scrollback search (no valid command starts with `/`,
    // so leading-slash input is never a literal; later slashes in paths
    // type normally via the edit path below).
    s.searching = true;
    s.status_msg = "search -- type, Enter finds, Esc keeps";
  } else if ((ch == 'n' || ch == 'N') && s.input.empty() && !s.search_matches.empty() && !s.search_query.empty()) {
    if (ch == 'n') {
      s.search_sel = (s.search_sel + 1) % s.search_matches.size();
    } else {
      s.search_sel = (s.search_sel + s.search_matches.size() - 1) % s.search_matches.size();
    }
    jump_to_sel(s);
    s.status_msg = "match " + std::to_string(s.search_sel + 1) + "/" + std::to_string(s.search_matches.size()) +
                   " for `" + s.search_query + "` (n/N)";
  } else if (ch == kEsc && s.input.empty() && (!s.search_query.empty() || !s.search_matches.empty())) {
    s.searching = false;
    s.search_query.clear();
    s.search_matches.clear();
    s.search_sel = 0;
    s.status_msg = "search cleared";
  } else if (ch == KEY_PPAGE) {
    s.scroll += s.out_rows > 1 ? s.out_rows - 1 : 1;
  } else if (ch == KEY_NPAGE) {
    s.scroll -= s.out_rows > 1 ? s.out_rows - 1 : 1;
  } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
    submit_line(s);
  } else {
    edit_key(s, ch);
  }
}

void handle_key(State &s, int ch) {
  if (ch == KEY_F(10) || ch == kCtrlQ) {
    // Quit while running cancels first; the run loop joins the worker so
    // sinks flush/close normally before `endwin`.
    if (s.async.busy()) {
      s.async.request_cancel();
      s.status_msg = "cancelling `" + s.async.line() + "`...";
    }
    s.quit = true;
    return;
  }
  // Ctrl-C while busy is cancel (step boundary), never line/field clear.
  // Idle Ctrl-C keeps its existing clear/close meaning in each pane.
  if (s.async.busy() && ch == kCtrlC) {
    s.async.request_cancel();
    s.status_msg = "cancelling `" + s.async.line() + "` at step boundary...";
    return;
  }
  // Save prompt is global: F5 from any pane saves the scrollback (the one
  // captured-text pane shared by every view). Keys here mirror the main
  // save prompt so the status-line editor never depends on the overlay.
  if (s.saving) {
    if (ch == kEsc) {
      s.saving = false;
      s.status_msg = "save cancelled";
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      std::string err;
      const bool ok = views::save_lines(s.scrollback, s.save_path, err);
      s.saving = false;
      if (ok) {
        s.status_msg = "saved " + std::to_string(s.scrollback.size()) + " lines to '" + s.save_path + "'";
      } else {
        s.status_msg = "save failed: " + err;
      }
    } else if (ch == KEY_BACKSPACE || ch == kBackspaceAscii || ch == 8) {
      if (!s.save_path.empty()) {
        s.save_path.pop_back();
      }
    } else if (ch == kCtrlU || ch == kCtrlC) {
      s.save_path.clear();
    } else if (ch >= 32 && ch < 127) {
      s.save_path.push_back(static_cast<char>(ch));
    }
    return;
  }
  if (ch == KEY_F(5) && !s.searching) {
    if (s.save_path.empty()) {
      s.save_path = "tui_output.txt";
    }
    s.saving = true;
    s.status_msg = "save scrollback -- Enter writes, Esc cancels";
    return;
  }
  if (s.overlay == Overlay::kPalette) {
    palette_key(s, ch);
  } else if (s.overlay == Overlay::kHelp) {
    help_key(s, ch);
  } else if (s.overlay == Overlay::kForms) {
    forms_key(s, ch);
  } else if (s.overlay == Overlay::kBatches) {
    batches_key(s, ch);
  } else if (s.overlay == Overlay::kForm) {
    form_key(s, ch);
  } else if (s.overlay == Overlay::kBatch) {
    batch_key(s, ch);
  } else {
    main_key(s, ch);
  }
}

} // namespace wengine::tui
