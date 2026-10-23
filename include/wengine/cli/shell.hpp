#pragma once

#include "wengine/cli/line_reader.hpp"
#include "wengine/cli/subshell.hpp"

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli {

class Shell {
public:
  // Execute one line. `line` is copied into an internal buffer so the
  // returned views stay valid for the call. Returns core::kExitOk (0),
  // core::kExitRuntime (1), core::kExitUsage (2), kEnterSubShell (100, a
  // module asked to push its session — see run_pending_session), or -1
  // for exit/quit.
  int execute(std::string_view line, std::ostream &out);

  // Execute pre-split tokens (e.g. process argv in headless mode).
  // Unlike execute(), no re-tokenizing happens: each element is one
  // argument verbatim. Same return codes.
  int execute_argv(std::span<const std::string_view> tokens, std::ostream &out);

  // REPL. On a TTY stdin this uses raw-mode line editing with persistent
  // history (up/down recall); otherwise plain getline (pipes, tests).
  // A module entry code (100) automatically pushes that module's session
  // on the same streams; leaving it returns to the top-level prompt.
  void run(std::istream &in, std::ostream &out);

  // True after execute()/execute_argv() returned kEnterSubShell for a
  // known module entry (cleared when consumed or on the next execute).
  [[nodiscard]] bool has_pending_session() const noexcept { return pending_.has_value(); }

  // Push the pending module session and run it on `in`/`out` until the
  // user leaves (back/exit/quit) or EOF. No-op without a pending
  // session. Returns 0, or -1 when a session verb requested app quit.
  int run_pending_session(std::istream &in, std::ostream &out);

  // History file for the interactive REPL. Empty disables persistence.
  // Defaults to LineReader::default_history_path().
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
