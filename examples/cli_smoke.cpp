#include "wengine/cli/shell.hpp"
#include <sstream>

// Minimal smoke: feed "ohm ..." through the shell (no manual I/O).
int main() {
  wengine::cli::Shell shell;
  std::ostringstream out;
  if (shell.execute("ohm 12V 3A", out) != 0) {
    return 1;
  }
  return out.str().empty() ? 1 : 0;
}
