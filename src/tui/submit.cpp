// Submission paths: every line goes through `TuiExecutor` (i.e.
// `cli::Shell::execute`) with captured output. Panes never reimplement
// parsing, validation, or solver calls, so TUI output stays byte-identical
// to the CLI by construction.

#include "state.hpp"

namespace wengine::tui {

// Shared submit: runs the input line through the executor.
void submit_line(State &s) {
  push_text(s.scrollback, std::string(kPrompt) + s.input, kMaxScrollback);
  if (!s.input.empty()) {
    if (s.history.add(s.input) && !s.hist_path.empty()) {
      (void)s.history.append_to_file(s.hist_path, s.input);
    }
  }
  s.hist_pos = -1;
  s.draft.clear();
  ExecResult res = s.exec.execute_line(s.input);
  s.input.clear();
  s.cursor = 0;
  s.scroll = 0;
  if (res.rc == -1) {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
  } else {
    s.last_rc = res.rc;
    push_text(s.scrollback, res.text, kMaxOutputLines);
    s.status_msg = res.text.empty() ? "ok (no output)" : "exit " + std::to_string(res.rc);
  }
}

// Form submit: compose the open form's argv string and run it through the
// same executor (never a form-side reimplementation). The composed line
// also lands in the scrollback + shared history like a typed line, while
// the form pane keeps the identical result beside the fields.
void submit_form(State &s) {
  if (s.open_form < 0 ||
      static_cast<std::size_t>(s.open_form) >= s.form_defs.size()) {
    return;
  }
  const std::size_t fi = static_cast<std::size_t>(s.open_form);
  const std::string line = forms::compose_line(s.form_defs[fi], s.form_vals[fi]);
  s.form_argv[fi] = line;
  push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
  if (!line.empty()) {
    if (s.history.add(line) && !s.hist_path.empty()) {
      (void)s.history.append_to_file(s.hist_path, line);
    }
  }
  s.hist_pos = -1;
  s.draft.clear();
  ExecResult res = s.exec.execute_line(line);
  s.scroll = 0;
  s.form_res_first[fi] = 0;
  if (res.rc == -1) {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
  } else {
    s.last_rc = res.rc;
    s.form_res[fi] = res;
    s.form_has[fi] = true;
    push_text(s.scrollback, res.text, kMaxOutputLines);
    s.status_msg = "form `" + line + "` -> exit " + std::to_string(res.rc);
  }
}

// Builder submit: compose the open builder's argv string. Finite runs go
// through the same executor (CLI-identical result beside the fields +
// scrollback/history mirror). `--forever` never runs here: it would block
// the loop until Ctrl-C, so the line is held for the async worker
// (TUI-5) and the builder reports that loudly instead of hanging.
void submit_batch(State &s) {
  if (s.open_batch < 0 ||
      static_cast<std::size_t>(s.open_batch) >= s.batch_defs.size()) {
    return;
  }
  const std::size_t bi = static_cast<std::size_t>(s.open_batch);
  const std::string line = batches::compose_line(s.batch_defs[bi], s.batch_vals[bi]);
  s.batch_argv[bi] = line;
  if (batches::wants_async(s.batch_defs[bi], s.batch_vals[bi])) {
    s.batch_has[bi] = false;
    s.batch_res_first[bi] = 0;
    s.scroll = 0;
    s.status_msg = "async run held: `" + line +
                   "` needs the TUI-5 worker (never blocks the builder) -- "
                   "clear --forever to run it now";
    push_text(s.scrollback, std::string(kPrompt) + line +
                                "\n[held for the async worker (TUI-5): --forever runs "
                                "until Ctrl-C and never blocks the builder]", kMaxScrollback);
    if (!line.empty()) {
      if (s.history.add(line) && !s.hist_path.empty()) {
        (void)s.history.append_to_file(s.hist_path, line);
      }
    }
    s.hist_pos = -1;
    s.draft.clear();
    return;
  }
  push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
  if (!line.empty()) {
    if (s.history.add(line) && !s.hist_path.empty()) {
      (void)s.history.append_to_file(s.hist_path, line);
    }
  }
  s.hist_pos = -1;
  s.draft.clear();
  ExecResult res = s.exec.execute_line(line);
  s.scroll = 0;
  s.batch_res_first[bi] = 0;
  if (res.rc == -1) {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
  } else {
    s.last_rc = res.rc;
    s.batch_res[bi] = res;
    s.batch_has[bi] = true;
    push_text(s.scrollback, res.text, kMaxOutputLines);
    s.status_msg = "builder `" + line + "` -> exit " + std::to_string(res.rc);
  }
}

} // namespace wengine::tui
