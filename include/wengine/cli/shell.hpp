#pragma once

/**
 * @file
 * The REPL driver: tokenizing and dispatching command lines against the
 * compile-time CLI registry, the interactive read-eval-print loop, and
 * the module sub-shell push/pop mechanics (logic workspace sessions).
 * Owns the process exit-code vocabulary end to end - commands return
 * 0/1/2 via cli/status.hpp helpers, `exit`/`quit` return the internal
 * -1 signal, and kEnterSubShell (100) is the module-session handshake
 * consumed here, never visible to the process boundary. Consumers:
 * apps/cli/main.cpp (REPL + headless argv dispatch), the TUI worker
 * (calls execute on worker threads), and tests/test_shell*.cpp. No
 * iostream ownership beyond the streams handed in; LineReader owns the
 * terminal specifics (see cli/line_reader.hpp).
 */

#include "wengine/cli/line_reader.hpp"
#include "wengine/cli/subshell.hpp"

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

/**
 * @brief Line dispatcher and REPL: one place that tokenizes, finds the
 *        command, runs it, and owns sub-shell session push/pop.
 * @note Single instance per process is the intended use; the TUI calls
 *       execute() from a worker thread while the ncurses loop polls
 *       progress atoms (cli/cancel.hpp).
 */
class Shell {
public:
  // Execute one line. `line` is copied into an internal buffer so the
  // returned views stay valid for the call. Returns core::kExitOk (0),
  // core::kExitRuntime (1), core::kExitUsage (2), kEnterSubShell (100, a
  // module asked to push its session — see run_pending_session), or -1
  // for exit/quit.
  /**
   * @brief Tokenize and execute one command line.
   * @param[in] line Raw input line (copied; tokens view into the copy).
   * @param[out] out Output stream for command results and errors.
   * @return core::kExitOk (0), core::kExitRuntime (1), core::kExitUsage
   *         (2), kEnterSubShell (100, a module asked to push its
   *         session - see run_pending_session), or -1 (internal exit
   *         signal for exit/quit).
   */
  int execute(std::string_view line, std::ostream &out);

  // Execute pre-split tokens (e.g. process argv in headless mode).
  // Unlike execute(), no re-tokenizing happens: each element is one
  // argument verbatim. Same return codes.
  /**
   * @brief Execute pre-split argument tokens (headless argv path).
   * @param[in] tokens Arguments verbatim, one per element (no
   *                   re-tokenizing, unlike execute()).
   * @param[out] out Output stream for command results and errors.
   * @return Same vocabulary as execute().
   */
  int execute_argv(std::span<const std::string_view> tokens, std::ostream &out);

  // REPL. On a TTY stdin this uses raw-mode line editing with persistent
  // history (up/down recall); otherwise plain getline (pipes, tests).
  // A module entry code (100) automatically pushes that module's session
  // on the same streams; leaving it returns to the top-level prompt.
  /**
   * @brief Run the read-eval-print loop on the given streams.
   * @param[in] in Input stream (TTY: raw-mode editing; otherwise plain
   *               getline).
   * @param[out] out Output stream for prompts and command output.
   * @note A module entry code (kEnterSubShell) automatically pushes that
   *       module's session on the same streams; leaving it returns to
   *       the top-level prompt. EOF or `exit`/`quit` ends the loop.
   */
  void run(std::istream &in, std::ostream &out);

  // True after execute()/execute_argv() returned kEnterSubShell for a
  // known module entry (cleared when consumed or on the next execute).
  /**
   * @brief Whether a module session is waiting to be pushed.
   * @return True after execute()/execute_argv() returned kEnterSubShell
   *         for a known module entry; cleared when consumed or on the
   *         next execute.
   */
  [[nodiscard]] bool has_pending_session() const noexcept { return pending_.has_value(); }

  // Push the pending module session and run it on `in`/`out` until the
  // user leaves (back/exit/quit) or EOF. No-op without a pending
  // session. Returns 0, or -1 when a session verb requested app quit.
  /**
   * @brief Push the pending module session and run it to completion.
   * @param[in] in Input stream for the session loop.
   * @param[out] out Output stream for the session loop (prompt swapped).
   * @return 0 normally; -1 when a session verb requested app quit.
   * @note No-op without a pending session.
   */
  int run_pending_session(std::istream &in, std::ostream &out);

  // History file for the interactive REPL. Empty disables persistence.
  // Defaults to LineReader::default_history_path().
  /**
   * @brief Set the history file for the interactive REPL.
   * @param[in] path History path; empty disables persistence. Defaults
   *                 to LineReader::default_history_path().
   */
  void set_history_file(std::string path);

private:
  void print_help(std::ostream &out) const;

  // Run one module session (shared by run() and run_pending_session()).
  // Returns 0 normally, -1 when the session requests app quit.
  int run_session(const SubShellConfig &cfg, std::istream &in, std::ostream &out);

  LineReader reader_;
  std::optional<SubShellConfig> pending_;
  std::vector<SubShellConfig> sessions_; // depth <= kMaxSubShellDepth
};

} // namespace wengine::cli
