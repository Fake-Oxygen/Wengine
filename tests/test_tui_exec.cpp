// TUI executor parity: the TUI must print exactly what the shared CLI
// path prints for the same line (byte-identical text, same exit code),
// because it calls `cli::Shell::execute` verbatim rather than
// reimplementing any command. The palette model derives from the same
// registry, so new commands appear with no TUI-side change.

#include "wengine/tui/exec.hpp"
#include "wengine/tui/palette.hpp"
#include "wengine/tui/popup.hpp"
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

void test_palette_registry_driven() {
  // Builtin coupling is pinned: palette builtins must equal the REPL's.
  static_assert(cli::kBuiltinCommands.size() == 3);
  const auto entries = tui::palette_entries();
  // Registry size itself is guarded by test_cli_args' static_assert; here
  // the palette must track it exactly (no hardcoded command count).
  assert(entries.size() == cli::CliRegistry::all().size() + cli::kBuiltinCommands.size());

  // Every registry command appears once with its registry description.
  for (const auto &c : cli::CliRegistry::all()) {
    bool found = false;
    for (const auto &e : entries) {
      if (!e.builtin && e.name == c.name) {
        assert(e.description == c.description);
        assert(!e.description.empty());
        found = true;
      }
    }
    assert(found);
  }
  // Builtins match the REPL vocabulary and Shell's help wording.
  std::size_t builtins = 0;
  for (const auto &e : entries) {
    if (e.builtin) {
      ++builtins;
      bool known = false;
      for (const auto &b : cli::kBuiltinCommands) {
        known = known || (e.name == b);
      }
      assert(known);
      assert(!e.description.empty());
    }
  }
  assert(builtins == cli::kBuiltinCommands.size());
}

void test_palette_filter() {
  const auto entries = tui::palette_entries();
  {
    const auto all = tui::filter_entries(entries, "");
    assert(all.size() == entries.size());
  }
  {
    // Prefix filter: "s" matches the s-family in registry order.
    const auto s = tui::filter_entries(entries, "s");
    assert(s.size() == 3);
    assert(entries[s[0]].name == "s0");
    assert(entries[s[1]].name == "sim");
    assert(entries[s[2]].name == "sig");
  }
  {
    const auto o = tui::filter_entries(entries, "o");
    assert(o.size() == 1 && entries[o[0]].name == "ohm");
  }
  assert(tui::filter_entries(entries, "zzz").empty());
  // Full names still match (Enter on a fully-typed filter inserts it).
  {
    const auto ic = tui::filter_entries(entries, "ic");
    assert(ic.size() == 1 && entries[ic[0]].name == "ic");
  }
}

void test_first_token() {
  assert(tui::first_token("sim run --steps 2") == "sim");
  assert(tui::first_token("  ohm 12V") == "ohm");
  assert(tui::first_token("") == "");
  assert(tui::first_token("   ") == "");
  assert(tui::first_token("help") == "help");
}

void test_help_pane_parity() {
  // The help pane runs `help <cmd>` through the executor, so its content
  // is `help <cmd>` byte-for-byte, including error shapes. Pin that the
  // pane's data source agrees with the Shell on every entry + builtins.
  cli::Shell shell;
  shell.set_history_file("");
  tui::TuiExecutor exec;
  exec.set_history_file("");
  const auto entries = tui::palette_entries();
  for (const auto &e : entries) {
    const std::string line = "help " + e.name;
    std::ostringstream expected;
    const int rc_shell = shell.execute(line, expected);
    const tui::ExecResult got = exec.execute_line(line);
    assert(got.rc == rc_shell);
    assert(got.text == expected.str());
    // Registry commands have real usage bodies; builtins report loudly.
    if (!e.builtin) {
      assert(got.rc == 0 && got.text.find("Usage") != std::string::npos);
    }
  }
  {
    std::ostringstream expected;
    const int rc_shell = shell.execute("help", expected);
    const tui::ExecResult got = exec.execute_line("help");
    assert(got.rc == rc_shell && got.text == expected.str());
  }
}

void test_popup_wrap() {
  const std::vector<std::string> names{"ohm", "s0", "sim"};
  // One row when everything fits.
  {
    const auto rows = tui::wrap_popup_lines(names, 40, 6);
    assert(rows.size() == 1 && rows[0] == "ohm s0 sim");
  }
  // Greedy wrap at the width.
  {
    const auto rows = tui::wrap_popup_lines(names, 7, 6);
    assert(rows.size() == 2 && rows[0] == "ohm s0" && rows[1] == "sim");
  }
  // Row cap ends loudly with the remainder count, never silently.
  {
    const std::vector<std::string> many{"aa", "bb", "cc", "dd", "ee"};
    const auto rows = tui::wrap_popup_lines(many, 5, 2);
    assert(rows.size() == 2);
    assert(rows[0] == "aa bb");
    assert(rows[1].find("cc dd") != std::string::npos);
    assert(rows[1].find("+1 more") != std::string::npos);
  }
  // Degenerate inputs stay empty, never crash.
  assert(tui::wrap_popup_lines(names, 40, 0).empty());
  assert(tui::wrap_popup_lines(names, 0, 6).empty());
  assert(tui::wrap_popup_lines({}, 40, 6).empty());
  // One over-wide name still shows (the draw layer truncates to width).
  {
    const std::vector<std::string> wide{"averylongname"};
    const auto rows = tui::wrap_popup_lines(wide, 5, 6);
    assert(rows.size() == 1 && rows[0] == "averylongname");
  }
}

} // namespace

int main() {
  test_parity();
  test_complete_delegates();
  test_palette_registry_driven();
  test_palette_filter();
  test_first_token();
  test_help_pane_parity();
  test_popup_wrap();
  std::cout << "test_tui_exec: OK\n";
  return 0;
}
