// cli_smoke: minimal in-process smoke for the CLI shell driver (no I/O).
//
// Feeds one "ohm 12V 3A" line through cli::Shell::execute into a string
// stream and exits nonzero on a dispatch failure or empty output. No
// publisher, segment, or socket endpoint required; guards the shell +
// registry link at example build time.
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
