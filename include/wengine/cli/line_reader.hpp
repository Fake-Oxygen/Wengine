#pragma once

/**
 * @file
 * Interactive line editor with persistent history (up/down recall).
 * Shell::run uses LineReader when stdin is a TTY, otherwise plain
 * getline (pipes, tests, non-POSIX platforms). All terminal handling
 * lives in src/cli/line_reader.cpp; this header is portable C++20 with
 * no platform includes. Raw-mode discipline: the TTY path applies raw
 * mode with TCSANOW (never TCSAFLUSH, which would discard queued
 * typeahead between reads) and restores the saved mode with TCSADRAIN on
 * every return. Completion integrates via CommandDef spans (top-level
 * registry, or the module registry inside a session). Consumers: Shell
 * and SubShell sessions.
 */

// Interactive line editor with persistent history (up/down recall).
//
// Design: `Shell::run` uses `LineReader` when stdin is a TTY, otherwise it
// falls back to plain `std::getline` (pipes, tests, non-POSIX platforms).
// All terminal handling lives in `src/cli/line_reader.cpp`; this header is
// portable C++20 with no platform includes.
//
// Error convention (unified): fallible persistence returns `core::Status`
// (`kInternal` on I/O failure); `add()` stays `bool` (stored vs ignored —
// a predicate, not an error); `load()` returns the entry count (missing
// file is 0, not an error); `read_line()` returns `nullopt` only for EOF
// (absence, not failure).

#include "wengine/core/status.hpp"
#include "wengine/cli/command.hpp"
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

// Bounded in-memory history with optional file persistence.
/**
 * @brief Bounded in-memory line history with optional file persistence.
 */
class History {
public:
  /**
   * @brief Default history capacity (entries).
   */
  static constexpr std::size_t kDefaultMaxEntries = 500;

  /**
   * @brief History holding up to max_entries lines.
   * @param[in] max_entries Capacity; oldest entries drop past it.
   */
  explicit History(std::size_t max_entries = kDefaultMaxEntries) noexcept;

  // Append unless empty or a duplicate of the previous entry.
  // Drops oldest entries past capacity. Returns true when stored.
  /**
   * @brief Append a line unless empty or a repeat of the last entry.
   * @param[in] line Line to store (copied; views never alias the reader).
   * @return True when stored; false for empty or duplicate lines (a
   *         predicate, not an error).
   */
  bool add(std::string_view line);

  /**
   * @brief Stored entry count.
   * @return Number of entries in memory.
   */
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  /**
   * @brief Whether no entries are stored.
   * @return True when size is zero.
   */
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  /**
   * @brief Entry by index (oldest first).
   * @param[in] i Index below size().
   * @return Reference to the stored line (valid until mutation).
   */
  [[nodiscard]] const std::string &at(std::size_t i) const { return entries_.at(i); }
  /**
   * @brief Most recent entry.
   * @return Reference to the last stored line (valid until mutation).
   */
  [[nodiscard]] const std::string &back() const { return entries_.back(); }

  /**
   * @brief Drop every stored entry (persistence file untouched).
   */
  void clear() noexcept { entries_.clear(); }

  // Load persisted lines (up to capacity, oldest-first). Missing file is OK.
  // Returns number of entries loaded.
  /**
   * @brief Load persisted lines, oldest first, up to capacity.
   * @param[in] path History file path.
   * @return Entries loaded; a missing file loads zero (not an error).
   */
  std::size_t load(std::string_view path);
  // Rewrite the whole file from memory. `kInternal` on I/O error.
  /**
   * @brief Rewrite the whole file from memory.
   * @param[in] path History file path.
   * @return kOk on success; kInternal on an I/O error.
   */
  core::Status save(std::string_view path) const noexcept;
  // Append one line to the file. `kInternal` on I/O error.
  /**
   * @brief Append one line to the file.
   * @param[in] path History file path.
   * @param[in] line Line to append.
   * @return kOk on success; kInternal on an I/O error.
   */
  core::Status append_to_file(std::string_view path, std::string_view line) const noexcept;

private:
  std::vector<std::string> entries_;
  std::size_t max_entries_;
};

// TTY line reader: prompt, Emacs-style editing, history recall.
// Non-copyable (owns the history file session).
/**
 * @brief TTY line reader: prompt, Emacs-style editing, history recall.
 * @note Non-copyable (owns the history file session). Uses raw-mode
 *       editing on TTYs (TCSANOW entry, TCSADRAIN restore, so queued
 *       typeahead survives), plain getline otherwise.
 */
class LineReader {
public:
  /**
   * @brief Construct a TTY reader (no history file until set).
   */
  LineReader();
  /**
   * @brief Destroy the reader (restores the saved terminal mode).
   */
  ~LineReader();

  /**
   * @brief Copy construction is deleted (owns the history file session).
   */
  LineReader(const LineReader &) = delete;
  /**
   * @brief Copy-assign is deleted (owns the history file session).
   */
  LineReader &operator=(const LineReader &) = delete;

  // History file used for load/append. Empty disables persistence.
  /**
   * @brief History file for load/append (empty disables persistence).
   * @param[in] path File path (copied; empty disables persistence).
   * @param[in] max_entries Trim target applied when appending.
   */
  void set_history_file(std::string path, std::size_t max_entries = 500);
  // Default location: $XDG_CACHE_HOME/wengine/history or
  // $HOME/.cache/wengine/history. Empty when HOME is unavailable.
  /**
   * @brief Default history file location.
   * @return `$XDG_CACHE_HOME`/wengine/history, or
   *         `$HOME`/.cache/wengine/history; empty when HOME is unavailable.
   */
  [[nodiscard]] static std::string default_history_path();

  /**
   * @brief In-memory history (recall, tests, session sharing).
   * @return Reference to the owned history.
   */
  [[nodiscard]] History &history() noexcept { return history_; }

  // Command set used for Tab completion (defaults to the top-level
  // registry; module sessions swap in their own registry while active
  // and restore this on exit). The span must outlive the reader —
  // registries are static, so this holds by construction.
  /**
   * @brief Command set used for Tab completion.
   * @param[in] cmds Registry span (top-level by default; module sessions
   *            swap in their own while active and restore on exit). The
   *            span must outlive the reader - registries are static, so
   *            this holds by construction.
   */
  void set_complete_commands(std::span<const CommandDef> cmds) noexcept { complete_commands_ = cmds; }

  // Read one line. Returns nullopt on EOF (Ctrl-D on empty line / closed
  // stdin). Uses raw-mode editing on TTYs, plain getline otherwise.
  // `in` other than stdin always uses the plain path (keeps tests/pipes).
  /**
   * @brief Read one line with prompt, editing, and history.
   * @param[in] prompt Prompt text written before reading.
   * @param[in,out] in Input stream (`in` other than stdin always takes
   *                the plain path, keeping tests and pipes TTY-free).
   * @param[out] out Stream receiving the prompt and echo.
   * @return The line (history-updated); nullopt on EOF only (Ctrl-D on
   *         an empty line, or closed stdin) - absence, not failure.
   * @note Raw-mode editing on TTYs (TCSANOW entry, never TCSAFLUSH),
   *       plain getline otherwise. Verify TTY behavior with a real PTY,
   *       never piped script alone.
   */
  std::optional<std::string> read_line(std::string_view prompt, std::istream &in, std::ostream &out);

private:
  History history_;
  std::string history_path_;
  bool history_loaded_{false};
  std::span<const CommandDef> complete_commands_{};

  void ensure_history_loaded();
  std::optional<std::string> read_plain(std::string_view prompt, std::istream &in,
                                        std::ostream &out);
#if defined(__unix__) || defined(__APPLE__)
  std::optional<std::string> read_tty(std::string_view prompt, std::ostream &out);
#endif
};

} // namespace wengine::cli
