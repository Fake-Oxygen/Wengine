// TUI executor parity: the TUI must print exactly what the shared CLI
// path prints for the same line (byte-identical text, same exit code),
// because it calls `cli::Shell::execute` verbatim rather than
// reimplementing any command.

#include "wengine/tui/exec.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <iostream>
#include <sstream>

using namespace wengine;

namespace {

void test_parity() {
  cli::Shell shell;
  shell.set_history_file(""); // no file I/O in tests
  tui::TuiExecutor exec;
  exec.set_history_file("");

  const char *cases[] = {"",
                         "ohm 12V 3A",
                         "ohm --u 12V --r 4Ohm",
                         "help ohm",
                         "s0 convert --const 1000imp/kWh",
                         "ldo --vin 5V --vout 3.3V --iout 500mA",
                         "led --vs 5V --vf 2V --if 20mA",
                         "sim run --steps 2 --particles 1",
                         "sig run --sig-kind dc --steps 2",
                         "ic --help",
                         "nopecmd",
                         "ohm 12V", // usage error, same code + text
                         "exit"};
  for (const char *c : cases) {
    std::ostringstream expected;
    const int rc_shell = shell.execute(c, expected);
    const tui::ExecResult got = exec.execute_line(c);
    assert(got.rc == rc_shell);
    assert(got.text == expected.str());
  }
  // `quit` is the same internal exit signal the REPL loop acts on.
  const tui::ExecResult q = exec.execute_line("quit");
  assert(q.rc == -1);
}

void test_complete_delegates() {
  tui::TuiExecutor exec;
  exec.set_history_file("");
  {
    const auto c = exec.complete("o", 1);
    assert(c.size() == 1 && c[0] == "ohm");
  }
  {
    const auto c = exec.complete("sim run --solver=s", 18);
    assert(!c.empty());
  }
  assert(exec.complete("zzz", 3).empty());
}

} // namespace

int main() {
  test_parity();
  test_complete_delegates();
  std::cout << "test_tui_exec: OK\n";
  return 0;
}
