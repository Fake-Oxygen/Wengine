// Submission paths: every line runs on the async worker (TUI-5) through
// `cli::Shell::execute` with captured output. Panes never reimplement
// parsing, validation, or solver calls, so TUI output stays byte-identical
// to the CLI by construction. The frame loop polls completion via
// `poll_async` so input stays live; Ctrl-C requests a step-boundary stop.

#include "state.hpp"

namespace wengine::tui {

namespace {

std::string_view first_word(std::string_view s) {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  std::size_t e = b;
  while (e < s.size() && s[e] != ' ' && s[e] != '\t') {
    ++e;
  }
  return s.substr(b, e - b);
}

bool start_task(State &s, const std::string &line, AsyncOrigin origin, std::size_t index) {
  if (s.async.busy()) {
    s.status_msg = "busy: running `" + s.async.line() + "` -- Ctrl-C cancels";
    return false;
  }
  s.async_origin = origin;
  s.async_index = index;
  s.scroll = 0;
  if (!s.async.start(line)) {
    s.async_origin = AsyncOrigin::kNone;
    s.status_msg = "busy: running `" + s.async.line() + "` -- Ctrl-C cancels";
    return false;
  }
  s.status_msg = "running `" + line + "` -- Ctrl-C cancels";
  return true;
}

} // namespace

// Shared submit: echo the line, record history, run async.
void submit_line(State &s) {
  if (s.async.busy()) {
    s.status_msg = "busy: running `" + s.async.line() + "` -- Ctrl-C cancels";
    return;
  }
  const std::string line = s.input;
  const std::string_view cmd = first_word(line);
  // `exit`/`quit` never needs the worker: quit immediately like the REPL.
  if (cmd == "exit" || cmd == "quit") {
    push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
    if (!line.empty()) {
      if (s.history.add(line) && !s.hist_path.empty()) {
        (void)s.history.append_to_file(s.hist_path, line);
      }
    }
    s.hist_pos = -1;
    s.draft.clear();
    s.input.clear();
    s.cursor = 0;
    s.scroll = 0;
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
    return;
  }
  if (line.empty()) {
    push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
    s.hist_pos = -1;
    s.draft.clear();
    s.scroll = 0;
    s.last_rc = 0;
    s.status_msg = "ok (no output)";
    s.input.clear();
    s.cursor = 0;
    return;
  }
  push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
  if (s.history.add(line) && !s.hist_path.empty()) {
    (void)s.history.append_to_file(s.hist_path, line);
  }
  s.hist_pos = -1;
  s.draft.clear();
  s.input.clear();
  s.cursor = 0;
  (void)start_task(s, line, AsyncOrigin::kLine, 0);
}

// Form submit: compose the open form's argv string and run it async (never
// a form-side reimplementation). The composed line also lands in the
// scrollback + shared history like a typed line; completion routes the
// CLI-identical result back beside the fields.
void submit_form(State &s) {
  if (s.open_form < 0 || static_cast<std::size_t>(s.open_form) >= s.form_defs.size()) {
    return;
  }
  if (s.async.busy()) {
    s.status_msg = "busy: running `" + s.async.line() + "` -- Ctrl-C cancels";
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
  s.form_res_first[fi] = 0;
  if (line.empty()) {
    s.last_rc = 0;
    s.status_msg = "ok (no output)";
    return;
  }
  const std::string_view cmd = first_word(line);
  if (cmd == "exit" || cmd == "quit") {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
    return;
  }
  (void)start_task(s, line, AsyncOrigin::kForm, fi);
}

// Builder submit: compose the open builder's argv string and run it on the
// async worker (TUI-5) — finite batches and `--forever` alike, so the
// builder never blocks. Completion routes the CLI-identical result beside
// the fields plus the scrollback/history mirror.
void submit_batch(State &s) {
  if (s.open_batch < 0 ||
      static_cast<std::size_t>(s.open_batch) >= s.batch_defs.size()) {
    return;
  }
  if (s.async.busy()) {
    s.status_msg = "busy: running `" + s.async.line() + "` -- Ctrl-C cancels";
    return;
  }
  const std::size_t bi = static_cast<std::size_t>(s.open_batch);
  const std::string line = batches::compose_line(s.batch_defs[bi], s.batch_vals[bi]);
  s.batch_argv[bi] = line;
  push_text(s.scrollback, std::string(kPrompt) + line, kMaxScrollback);
  if (!line.empty()) {
    if (s.history.add(line) && !s.hist_path.empty()) {
      (void)s.history.append_to_file(s.hist_path, line);
    }
  }
  s.hist_pos = -1;
  s.draft.clear();
  s.batch_res_first[bi] = 0;
  if (line.empty()) {
    s.last_rc = 0;
    s.status_msg = "ok (no output)";
    return;
  }
  const std::string_view cmd = first_word(line);
  if (cmd == "exit" || cmd == "quit") {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
    return;
  }
  (void)start_task(s, line, AsyncOrigin::kBatch, bi);
}

// Completion routing for the frame loop: join a finished worker (non-
// blocking) and mirror its captured text into the scrollback with the
// 500-line cap policy plus the origin pane. Returns true when a task
// finished this tick.
bool poll_async(State &s) {
  AsyncResult r;
  if (!s.async.try_take(r)) {
    return false;
  }
  finish_async(s, r);
  return true;
}

void finish_async(State &s, const AsyncResult &r) {
  s.scroll = 0;
  const AsyncOrigin origin = s.async_origin;
  const std::size_t idx = s.async_index;
  s.async_origin = AsyncOrigin::kNone;
  s.async_index = 0;
  if (r.rc == -1) {
    s.quit = true;
    s.last_rc = 0;
    s.status_msg = "bye";
    return;
  }
  s.last_rc = r.rc;
  // Tails into the scrollback with the 500-line cap policy (loud
  // truncation note); `--emit-*` file outputs already landed on disk in
  // the worker via the normal CLI path, unaffected.
  push_text(s.scrollback, r.text, kMaxOutputLines);
  const std::string base = (r.cancelled ? "cancelled at step boundary -- " : "") + std::string("exit ") + std::to_string(r.rc);
  if (origin == AsyncOrigin::kForm && idx < s.form_defs.size() &&
      idx < s.form_res.size() && idx < s.form_has.size() &&
      idx < s.form_res_first.size() && idx < s.form_argv.size()) {
    s.form_res[idx] = ExecResult{r.rc, r.text};
    s.form_has[idx] = true;
    s.form_res_first[idx] = 0;
    s.status_msg = "form `" + r.line + "` -> " + base;
  } else if (origin == AsyncOrigin::kBatch && idx < s.batch_defs.size() && idx < s.batch_res.size() && idx < s.batch_has.size() &&
             idx < s.batch_res_first.size() && idx < s.batch_argv.size()) {
    s.batch_res[idx] = ExecResult{r.rc, r.text};
    s.batch_has[idx] = true;
    s.batch_res_first[idx] = 0;
    s.status_msg = "builder `" + r.line + "` -> " + base;
  } else {
    s.status_msg = r.text.empty() ? "ok (no output)" : base;
    if (r.cancelled && r.text.empty()) {
      s.status_msg = "cancelled at step boundary -- exit " + std::to_string(r.rc);
    }
  }
}

} // namespace wengine::tui
