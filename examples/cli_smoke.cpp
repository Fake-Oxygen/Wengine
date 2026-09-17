#include "wengine/cli/shell.hpp"
#include <sstream>

// Minimal smoke: feed "ohm" + "exit" through the shell (no manual I/O).
int main() {
  wengine::cli::Shell shell;
  std::ostringstream out;
  if (shell.execute("ohm", out) != 0) {
    return 1;
  }
  return out.str().empty() ? 1 : 0;
}
