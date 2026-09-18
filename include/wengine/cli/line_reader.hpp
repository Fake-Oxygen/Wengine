#pragma once

// Interactive line editor with persistent history (up/down recall).
//
// Design: `Shell::run` uses `LineReader` when stdin is a TTY, otherwise it
// falls back to plain `std::getline` (pipes, tests, non-POSIX platforms).
// All terminal handling lives in `src/cli/line_reader.cpp`; this header is
// portable C++20 with no platform includes.

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

// Bounded in-memory history with optional file persistence.
class History {
public:
  static constexpr std::size_t kDefaultMaxEntries = 500;

  explicit History(std::size_t max_entries = kDefaultMaxEntries) noexcept;

  // Append unless empty or a duplicate of the previous entry.
  // Drops oldest entries past capacity. Returns true when stored.
  bool add(std::string_view line);

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const std::string &at(std::size_t i) const { return entries_.at(i); }
  [[nodiscard]] const std::string &back() const { return entries_.back(); }

  void clear() noexcept { entries_.clear(); }

  // Load persisted lines (up to capacity, oldest-first). Missing file is OK.
  // Returns number of entries loaded.
  std::size_t load(std::string_view path);
  // Rewrite the whole file from memory. Returns false on I/O error.
  bool save(std::string_view path) const noexcept;
  // Append one line to the file. Returns false on I/O error.
  bool append_to_file(std::string_view path, std::string_view line) const noexcept;

private:
  std::vector<std::string> entries_;
  std::size_t max_entries_;
};

// TTY line reader: prompt, Emacs-style editing, history recall.
// Non-copyable (owns the history file session).
class LineReader {
public:
  LineReader();
  ~LineReader();

  LineReader(const LineReader &) = delete;
  LineReader &operator=(const LineReader &) = delete;

  // History file used for load/append. Empty disables persistence.
  void set_history_file(std::string path, std::size_t max_entries = 500);
  // Default location: $XDG_CACHE_HOME/wengine/history or
  // $HOME/.cache/wengine/history. Empty when HOME is unavailable.
  [[nodiscard]] static std::string default_history_path();

  [[nodiscard]] History &history() noexcept { return history_; }

  // Read one line. Returns nullopt on EOF (Ctrl-D on empty line / closed
  // stdin). Uses raw-mode editing on TTYs, plain getline otherwise.
  // `in` other than stdin always uses the plain path (keeps tests/pipes).
  std::optional<std::string> read_line(std::string_view prompt, std::istream &in,
                                       std::ostream &out);

private:
  History history_;
  std::string history_path_;
  bool history_loaded_{false};

  void ensure_history_loaded();
  std::optional<std::string> read_plain(std::string_view prompt, std::istream &in,
                                        std::ostream &out);
#if defined(__unix__) || defined(__APPLE__)
  std::optional<std::string> read_tty(std::string_view prompt, std::ostream &out);
#endif
};

} // namespace wengine::cli
