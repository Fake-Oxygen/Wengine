// Top-level key dispatch + main-line keys (the kNone overlay:
// F-key pane switches, context help, scrollback paging, submit, editing).

#include "state.hpp"

#include <curses.h>

namespace wengine::tui {

void main_key(State &s, int ch) {
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
