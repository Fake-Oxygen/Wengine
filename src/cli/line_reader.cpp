#include "wengine/cli/line_reader.hpp"
#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/complete.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <istream>
#include <ostream>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

namespace wengine::cli {

// ------------------------------------------------------------------ History

History::History(std::size_t max_entries) noexcept
    : max_entries_(max_entries == 0 ? 1 : max_entries) {}

bool History::add(std::string_view line) {
  if (line.empty()) {
    return false;
  }
  if (!entries_.empty() && entries_.back() == line) {
    return false; // consecutive duplicate
  }
  entries_.emplace_back(line);
  while (entries_.size() > max_entries_) {
    entries_.erase(entries_.begin());
  }
  return true;
}

std::size_t History::load(std::string_view path) {
  std::ifstream f{std::string{path}};
  if (!f) {
    return 0;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
      line.clear();
    }
  }
  // Keep the newest `max_entries_` lines.
  const std::size_t start = lines.size() > max_entries_ ? lines.size() - max_entries_ : 0;
  entries_.clear();
  for (std::size_t i = start; i < lines.size(); ++i) {
    entries_.push_back(std::move(lines[i]));
  }
  return entries_.size();
}

core::Status History::save(std::string_view path) const noexcept {
  std::ofstream f{std::string{path}, std::ios::trunc};
  if (!f) {
    return {core::StatusCode::kInternal};
  }
  for (const auto &e : entries_) {
    f << e << '\n';
  }
  if (!static_cast<bool>(f)) {
    return {core::StatusCode::kInternal};
  }
  return core::Status::success();
}

core::Status History::append_to_file(std::string_view path,
                                     std::string_view line) const noexcept {
  std::ofstream f{std::string{path}, std::ios::app};
  if (!f) {
    return {core::StatusCode::kInternal};
  }
  f << line << '\n';
  if (!static_cast<bool>(f)) {
    return {core::StatusCode::kInternal};
  }
  return core::Status::success();
}

// ---------------------------------------------------------------- LineReader

LineReader::LineReader() : history_path_(default_history_path()) {}

LineReader::~LineReader() = default;

void LineReader::set_history_file(std::string path, std::size_t max_entries) {
  history_ = History(max_entries);
  history_path_ = std::move(path);
  history_loaded_ = false;
}

std::string LineReader::default_history_path() {
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    return std::string{xdg} + "/wengine/history";
  }
  const char *home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  return std::string{home} + "/.cache/wengine/history";
}

void LineReader::ensure_history_loaded() {
  if (history_loaded_ || history_path_.empty()) {
    return;
  }
  history_loaded_ = true;
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path{history_path_}.parent_path(), ec);
  history_.load(history_path_);
}

std::optional<std::string> LineReader::read_plain(std::string_view prompt, std::istream &in,
                                                  std::ostream &out) {
  out << prompt << std::flush;
  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::optional<std::string> LineReader::read_line(std::string_view prompt, std::istream &in,
                                                 std::ostream &out) {
#if defined(__unix__) || defined(__APPLE__)
  if (&in == &std::cin && ::isatty(STDIN_FILENO)) {
    return read_tty(prompt, out);
  }
#else
  (void)this;
#endif
  return read_plain(prompt, in, out);
}

#if defined(__unix__) || defined(__APPLE__)

namespace {

// RAII raw terminal mode (linenoise-style). Restores on destruction.
class RawMode {
public:
  explicit RawMode(int fd) : fd_(fd) {
    if (::tcgetattr(fd_, &orig_) != 0) {
      return;
    }
    raw_ = orig_;
    raw_.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw_.c_oflag &= ~(OPOST);
    raw_.c_cflag |= (CS8);
    raw_.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw_.c_cc[VMIN] = 1;
    raw_.c_cc[VTIME] = 0;
    // TCSANOW (not TCSAFLUSH): preserve already-queued typeahead so
    // successive read_line calls don't drop pasted/queued input.
    if (::tcsetattr(fd_, TCSANOW, &raw_) == 0) {
      active_ = true;
    }
  }

  ~RawMode() {
    if (active_) {
      ::tcsetattr(fd_, TCSADRAIN, &orig_);
    }
  }

  RawMode(const RawMode &) = delete;
  RawMode &operator=(const RawMode &) = delete;

  [[nodiscard]] bool active() const noexcept { return active_; }

private:
  int fd_;
  termios orig_{};
  termios raw_{};
  bool active_{false};
};

bool input_ready(int fd, int timeout_ms) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  const int r = ::select(fd + 1, &fds, nullptr, nullptr, &tv);
  return r > 0;
}

// Blocking single-byte read; nullopt on EOF/error.
std::optional<unsigned char> read_byte(int fd) {
  unsigned char c = 0;
  while (true) {
    const ssize_t r = ::read(fd, &c, 1);
    if (r == 1) {
      return c;
    }
    if (r == 0) {
      return std::nullopt;
    }
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
}

} // namespace

std::optional<std::string> LineReader::read_tty(std::string_view prompt, std::ostream &out) {
  ensure_history_loaded();

  RawMode raw{STDIN_FILENO};
  if (!raw.active()) {
    return read_plain(prompt, std::cin, out);
  }

  std::string buf;
  std::size_t cursor = 0;
  std::string draft; // stashed line while browsing history
  std::size_t hist_idx = history_.size();

  auto refresh = [&] {
    out << '\r' << prompt << buf << "\x1b[K" << std::flush;
    if (cursor < buf.size()) {
      out << "\x1b[" << (buf.size() - cursor) << 'D' << std::flush;
    }
  };

  auto history_up = [&] {
    if (history_.empty() || hist_idx == 0) {
      return;
    }
    if (hist_idx == history_.size()) {
      draft = buf;
    }
    --hist_idx;
    buf = history_.at(hist_idx);
    cursor = buf.size();
    refresh();
  };

  auto history_down = [&] {
    if (hist_idx >= history_.size()) {
      return;
    }
    ++hist_idx;
    buf = (hist_idx == history_.size()) ? draft : history_.at(hist_idx);
    cursor = buf.size();
    refresh();
  };

  // Detach from the browsed history entry after an edit, keeping the edited
  // text as the new draft (Down no longer discards in-progress edits).
  auto touch = [&] {
    if (hist_idx < history_.size()) {
      draft = buf;
      hist_idx = history_.size();
    }
  };

  out << prompt << std::flush;

  while (true) {
    auto bc = read_byte(STDIN_FILENO);
    if (!bc) {
      return std::nullopt; // closed stdin
    }
    const unsigned char c = *bc;

    if (c == '\r' || c == '\n') { // Enter
      out << "\r\n" << std::flush;
      if (!buf.empty()) {
        if (history_.add(buf) && !history_path_.empty()) {
          history_.append_to_file(history_path_, buf);
        }
      }
      return buf;
    }
    if (c == 3) { // Ctrl-C: cancel line, fresh prompt via empty result
      out << "^C\r\n" << std::flush;
      return std::string{};
    }
    if (c == 4) { // Ctrl-D: EOF on empty line, else delete under cursor
      if (buf.empty()) {
        return std::nullopt;
      }
      if (cursor < buf.size()) {
        buf.erase(cursor, 1);
        touch();
        refresh();
      }
      continue;
    }
    if (c == 127 || c == 8) { // Backspace
      if (cursor > 0) {
        buf.erase(cursor - 1, 1);
        --cursor;
        touch();
        refresh();
      }
      continue;
    }
    if (c == 1) { // Ctrl-A: home
      cursor = 0;
      refresh();
      continue;
    }
    if (c == 5) { // Ctrl-E: end
      cursor = buf.size();
      refresh();
      continue;
    }
    if (c == 21) { // Ctrl-U: kill to start
      buf.erase(0, cursor);
      cursor = 0;
      touch();
      refresh();
      continue;
    }
    if (c == 11) { // Ctrl-K: kill to end
      buf.erase(cursor);
      touch();
      refresh();
      continue;
    }
    if (c == 23) { // Ctrl-W: kill word left
      if (cursor > 0) {
        std::size_t end = cursor;
        while (end > 0 && buf[end - 1] == ' ') {
          --end;
        }
        while (end > 0 && buf[end - 1] != ' ') {
          --end;
        }
        buf.erase(end, cursor - end);
        cursor = end;
        touch();
        refresh();
      }
      continue;
    }
    if (c == 12) { // Ctrl-L: clear screen
      out << "\x1b[H\x1b[2J" << std::flush;
      refresh();
      continue;
    }
    if (c == 27) { // Escape: CSI / SS3 sequences (arrows, home/end/del)
      if (!input_ready(STDIN_FILENO, 50)) {
        continue; // lone ESC: ignore
      }
      auto b1 = read_byte(STDIN_FILENO);
      if (!b1) {
        return std::nullopt;
      }
      if (*b1 == '[') {
        // CSI: params bytes 0x30-0x3F, then final byte 0x40-0x7E.
        std::string params;
        unsigned char fin = 0;
        while (true) {
          if (!input_ready(STDIN_FILENO, 50)) {
            break;
          }
          auto bx = read_byte(STDIN_FILENO);
          if (!bx) {
            return std::nullopt;
          }
          if (*bx >= 0x40 && *bx <= 0x7E) {
            fin = *bx;
            break;
          }
          if (params.size() < 8) {
            params.push_back(static_cast<char>(*bx));
          }
        }
        if (fin == 'A') {
          history_up();
        } else if (fin == 'B') {
          history_down();
        } else if (fin == 'C') {
          if (cursor < buf.size()) {
            ++cursor;
            refresh();
          }
        } else if (fin == 'D') {
          if (cursor > 0) {
            --cursor;
            refresh();
          }
        } else if (fin == 'H' || (fin == '~' && (params == "1" || params == "7"))) {
          cursor = 0;
          refresh();
        } else if (fin == 'F' || (fin == '~' && (params == "4" || params == "8"))) {
          cursor = buf.size();
          refresh();
        } else if (fin == '~' && params == "3") {
          if (cursor < buf.size()) {
            buf.erase(cursor, 1);
            touch();
            refresh();
          }
        }
        // Other sequences ignored.
      } else if (*b1 == 'O') {
        if (!input_ready(STDIN_FILENO, 50)) {
          continue;
        }
        auto b2 = read_byte(STDIN_FILENO);
        if (!b2) {
          return std::nullopt;
        }
        if (*b2 == 'A') {
          history_up();
        } else if (*b2 == 'B') {
          history_down();
        } else if (*b2 == 'C') {
          if (cursor < buf.size()) {
            ++cursor;
            refresh();
          }
        } else if (*b2 == 'D') {
          if (cursor > 0) {
            --cursor;
            refresh();
          }
        } else if (*b2 == 'H') {
          cursor = 0;
          refresh();
        } else if (*b2 == 'F') {
          cursor = buf.size();
          refresh();
        }
      }
      // Bare ESC + other: ignore.
      continue;
    }
    if (c == 9) { // Tab: completion (common prefix or candidate list)
      std::size_t tok_start = cursor;
      while (tok_start > 0 && buf[tok_start - 1] != ' ' && buf[tok_start - 1] != '\t') {
        --tok_start;
      }
      auto cands = complete_line(buf, cursor, CliRegistry::all());
      if (cands.empty()) {
        continue;
      }
      if (cands.size() == 1) {
        buf.replace(tok_start, cursor - tok_start, cands[0]);
        cursor = tok_start + cands[0].size();
        if (cursor == buf.size()) {
          buf.push_back(' ');
          ++cursor;
        } else if (buf[cursor] != ' ' && buf[cursor] != '\t') {
          buf.insert(cursor, 1, ' ');
          ++cursor;
        }
        touch();
        refresh();
        continue;
      }
      const std::string prefix = common_prefix(cands);
      if (prefix.size() > cursor - tok_start) {
        buf.replace(tok_start, cursor - tok_start, prefix);
        cursor = tok_start + prefix.size();
        touch();
        refresh();
        continue;
      }
      std::sort(cands.begin(), cands.end());
      std::size_t width = 0;
      for (const auto &s : cands) {
        width = std::max(width, s.size());
      }
      width += 2;
      const std::size_t cols = std::max<std::size_t>(1, 80 / width);
      out << "\r\n";
      for (std::size_t k = 0; k < cands.size(); ++k) {
        out << cands[k];
        if ((k + 1) % cols == 0 || k + 1 == cands.size()) {
          out << "\r\n";
        } else {
          out << std::string(width - cands[k].size(), ' ');
        }
      }
      refresh();
      continue;
    }
    if (c < 32) {
      continue; // other control chars ignored
    }
    buf.insert(cursor, 1, static_cast<char>(c));
    ++cursor;
    touch();
    refresh();
  }
}

#endif // POSIX

} // namespace wengine::cli
