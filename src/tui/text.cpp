// Scrollback + help text utilities (no terminal handling).

#include "state.hpp"

namespace wengine::tui {

// Split `text` into lines, keeping at most `max_lines`; `dropped` counts
// the remainder so the caller can say so loudly instead of silently cutting.
void split_capped(std::vector<std::string> &out, std::string_view text, std::size_t max_lines, std::size_t &dropped) {
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      if (out.size() >= max_lines) {
        ++dropped;
      } else {
        out.push_back(cur);
      }
      cur.clear();
    } else if (c != '\r') {
      if (out.size() < max_lines) {
        cur.push_back(c);
      }
    }
  }
  if (!cur.empty()) {
    if (out.size() >= max_lines) {
      ++dropped;
    } else {
      out.push_back(cur);
    }
  }
}

void push_text(std::vector<std::string> &scrollback, std::string_view text, std::size_t max_lines) {
  std::size_t dropped = 0;
  split_capped(scrollback, text, max_lines, dropped);
  // `split_capped` caps only what this call appends; trim older history here.
  if (scrollback.size() > kMaxScrollback) {
    const auto excess = scrollback.size() - kMaxScrollback;
    scrollback.erase(scrollback.begin(), scrollback.begin() + static_cast<std::vector<std::string>::difference_type>(excess));
  }
  if (dropped > 0) {
    scrollback.push_back("[... " + std::to_string(dropped) + " lines truncated -- rerun with --emit-csv for the full log]");
  }
}

// Fetch `help <target>` (or bare `help`) through the executor so the pane is
// byte-identical to the CLI. The rc is reported, never stored: viewing help
// must not clobber the last command's status-bar exit code.
std::vector<std::string> fetch_help(TuiExecutor &exec, std::string_view target, int &rc_out) {
  const std::string line = target.empty() ? "help" : "help " + std::string(target);
  ExecResult res = exec.execute_line(line);
  rc_out = res.rc;
  std::vector<std::string> lines;
  std::size_t dropped = 0;
  split_capped(lines, res.text, kMaxScrollback, dropped);
  if (dropped > 0) {
    lines.push_back("[... truncated]");
  }
  return lines;
}

// Whitespace-separated token bounds around `cursor` (quotes stay literal:
// completion works on the raw token the CLI tokenizer will later split).
void token_bounds(std::string_view buf, std::size_t cursor, std::size_t &begin, std::size_t &end) {
  begin = cursor;
  while (begin > 0 && buf[begin - 1] != ' ' && buf[begin - 1] != '\t') {
    --begin;
  }
  end = cursor;
  while (end < buf.size() && buf[end] != ' ' && buf[end] != '\t') {
    ++end;
  }
}

} // namespace wengine::tui
