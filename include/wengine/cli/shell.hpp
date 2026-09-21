#pragma once

#include "wengine/cli/line_reader.hpp"

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace wengine::cli {

class Shell {
public:
  // Execute one line. `line` is copied into an internal buffer so the
  // returned views stay valid for the call. Returns 0 ok, 1 error, -1 exit.
  int execute(std::string_view line, std::ostream &out);

  // Execute pre-split tokens (e.g. process argv in headless mode).
  // Unlike execute(), no re-tokenizing happens: each element is one
  // argument verbatim. Same return codes.
  int execute_argv(std::span<const std::string_view> tokens, std::ostream &out);

  // REPL. On a TTY stdin this uses raw-mode line editing with persistent
  // history (up/down recall); otherwise plain getline (pipes, tests).
  void run(std::istream &in, std::ostream &out);

  // History file for the interactive REPL. Empty disables persistence.
  // Defaults to LineReader::default_history_path().
  void set_history_file(std::string path);

private:
  void print_help(std::ostream &out) const;

  LineReader reader_;
};

} // namespace wengine::cli
