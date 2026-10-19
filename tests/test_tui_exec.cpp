// TUI executor parity: the TUI must print exactly what the shared CLI
// path prints for the same line (byte-identical text, same exit code),
// because it calls `cli::Shell::execute` verbatim rather than
// reimplementing any command. The palette model derives from the same
// registry, so new commands appear with no TUI-side change.
//
// TUI-7 hardening: the parity matrix covers every registry command
// (goldens + usage-error shapes + stray positionals), completion
// candidates equal `cli::complete_line` outputs, and history-file sharing
// with the REPL is proven at the `cli::History` file-format level (the
// TUI session loads the same default path the REPL appends to).

#include "wengine/tui/exec.hpp"
#include "wengine/tui/palette.hpp"
#include "wengine/tui/popup.hpp"
#include "wengine/cli/line_reader.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <sstream>

using namespace wengine;

namespace {

struct ParityCase {
  const char *line;
  int want_rc;
  const char *want_sub; // nullptr: no substring check; "" : expect empty text
};

void test_parity_full_matrix() {
  // Every registry command appears at least once as a golden plus at
  // least one usage-error / stray shape. Fast only: no network
  // endpoints, no paced wall-clock runs, no --forever (async-owned),
  // no --emit-* files (owned by the batches/async suites).
  const ParityCase cases[] = {
      // Empty line: REPL no-op.
      {"", 0, ""},
      // ohm (closed form, exactly-two rule).
      {"ohm 12V 3A", 0, "R = 4"},
      {"ohm --u 12V --r 4Ohm", 0, "I = 3"},
      {"ohm --i 2A --r 5Ohm", 0, "U = 10"},
      {"ohm 12V", 2, "Error"},
      {"ohm 12V 3A extra", 2, "Error"},
      {"ohm --u 12V --bogus 3", 2, "Error"},
      // s0 (six modes).
      {"s0 convert --const 1000imp/kWh", 0, "imp"},
      {"s0 rate --power 2kW --const 1000imp/kWh", 0, "Hz"},
      {"s0 full --u 230V --i 10A --const 1000imp/kWh --time 1h", 0, nullptr},
      {"s0 bogus", 2, "Error"},
      {"s0 rate --power 2kW", 2, "Error"}, // missing --const
      // i2c (pullup/series/full + overrides).
      {"i2c pullup --vdd 3.3V --cb 200pF", 0, "Rp"},
      {"i2c series --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm", 0, "Rs"},
      {"i2c full --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm", 0, nullptr},
      {"i2c pullup --vdd 3.3V", 2, "Error"}, // missing --cb
      {"i2c", 2, "Error"},                   // missing mode
      // ldo (PASS rc 0 + FAIL rc 1 + usage rc 2).
      {"ldo --vin 5V --vout 3.3V --iout 500mA", 0, "summary: PASS"},
      {"ldo --vin 3.4V --vout 3.3V --iout 500mA --vdo 0.3V", 1, "summary: FAIL"},
      {"ldo --vin 5V", 2, "Error"},
      {"ldo --vin 5V --vout 3.3V --iout 500mA extra", 2, "Error"},
      // led (PASS rc 0 + infeasible rc 1 + usage rc 2).
      {"led --vs 5V --vf 2V --if 20mA", 0, "Rs exact"},
      {"led --vs 2V --vf 3V --if 20mA", 1, "INFEASIBLE"},
      {"led --vs 5V", 2, "Error"},
      {"led --vs 5V --vf 2V --if 20mA extra", 2, "Error"},
      // sim (projectile + signal solver + rejections + stray).
      {"sim run --steps 2 --particles 1", 0, "solver=projectile"},
      {"sim run --solver signal --steps 2", 0, "solver=signal"},
      {"sim run --steps 0", 2, "Error"},
      {"sim run --solver nope --steps 2", 2, "Available"},
      {"sim run --steps 2 extra", 2, "Error"},
      {"sim run --steps 2 --emit-csv-stride 2", 2, "Error"}, // stride w/o output
      // sig batch (dc + sine + rejections + stray; control shapes stay
      // validation-only here — live exchanges live in test_control_server).
      {"sig run --sig-kind dc --steps 2", 0, "kind=dc"},
      {"sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 2 --dt 0.1ms", 0,
       "kind=sine"},
      {"sig run --sig-kind sum --steps 2", 2, "Error"}, // batch rejects sum
      {"sig run --sig-kind dc --steps 2 extra", 2, "Error"},
      {"sig list", 2, "Error"}, // control endpoint required
      {"sig list --control-tcp 127.0.0.1:1 extra", 2, "Error"},
      {"sig set 0 --control-tcp 127.0.0.1:1", 2, "Error"}, // no params
      // ic dispatch (chips + ops + rejections + stray).
      {"ic --help", 0, "Available ICs"},
      {"ic ap63205 spec", 0, nullptr},
      {"ic ap63205 inductor --vin 12V", 0, nullptr},
      {"ic ina219 cal --imax 15A --rshunt 2mOhm", 0, "calibration: PASS"},
      {"ic nopechip spec", 2, "Available ICs"},
      {"ic ap63205 bogusop --vin 12V", 2, "Error"},
      {"ic ap63205 spec extra", 2, "Error"},
      // Builtins + help + unknown.
      {"help", 0, "Available commands"},
      {"help ohm", 0, "Usage"},
      {"help nope", 1, "Unknown command"},
      {"nopecmd", 1, "Unknown command"},
  };
  cli::Shell shell;
  shell.set_history_file(""); // no file I/O in tests
  tui::TuiExecutor exec;
  exec.set_history_file("");

  for (const auto &c : cases) {
    std::ostringstream expected;
    const int rc_shell = shell.execute(c.line, expected);
    const tui::ExecResult got = exec.execute_line(c.line);
    if (got.rc != rc_shell || got.text != expected.str()) {
      std::cerr << "parity mismatch for: " << c.line << "\n--- shell rc=" << rc_shell
                << "\n"
                << expected.str() << "\n--- tui rc=" << got.rc << "\n"
                << got.text << "\n";
    }
    assert(got.rc == rc_shell);
    assert(got.text == expected.str());
    assert(got.rc == c.want_rc);
    if (c.want_sub != nullptr) {
      if (c.want_sub[0] == '\0') {
        assert(got.text.empty());
      } else {
        assert(got.text.find(c.want_sub) != std::string::npos);
      }
    }
  }
  // `exit`/`quit` are the same internal exit signal the REPL loop acts on.
  for (const char *q : {"exit", "quit"}) {
    std::ostringstream expected;
    assert(shell.execute(q, expected) == -1);
    const tui::ExecResult got = exec.execute_line(q);
    assert(got.rc == -1);
    assert(got.text == expected.str());
  }

  // Registry coverage: every registered command appears in the matrix
  // above (as `cmd ...` or `help cmd`), so additions fail loudly here
  // until the matrix grows with them.
  for (const auto &cmd : cli::CliRegistry::all()) {
    bool found = false;
    for (const auto &c : cases) {
      const std::string line(c.line);
      if (line == cmd.name || line.rfind(std::string(cmd.name) + " ", 0) == 0 ||
          line == "help " + std::string(cmd.name)) {
        found = true;
      }
    }
    if (!found) {
      std::cerr << "parity matrix misses registry command: " << cmd.name << "\n";
    }
    assert(found);
  }
}

void test_complete_parity_full() {
  // Completion candidates equal the REPL's `complete_line` outputs over
  // the same registry: every command prefix, every per-command option
  // prefix, plus modes/values/builtins/unknowns.
  tui::TuiExecutor exec;
  exec.set_history_file("");
  const auto all = cli::CliRegistry::all();
  struct Probe {
    const char *buf;
    std::size_t cursor;
  };
  const Probe probes[] = {
      {"", 0},          {"o", 1},        {"h", 1},        {"s", 1},
      {"si", 2},        {"i", 1},        {"l", 1},        {"ld", 2},
      {"le", 2},        {"zzz", 3},      {"help ", 5},    {"help o", 6},
      {"nope ", 5},     {"ohm --", 6},   {"ohm --u", 7},  {"ohm --from ", 11},
      {"ohm --from=u", 12}, {"s0 ", 3},  {"s0 r", 4},    {"s0 --", 5},
      {"i2c ", 4},      {"i2c p", 5},    {"i2c --", 6},  {"i2c pullup --speed ", 20},
      {"ldo --", 6},    {"led --", 6},   {"sim ", 4},    {"sim --", 6},
      {"sim run --solver ", 17}, {"sim run --solver=p", 18}, {"sig ", 4},
      {"sig l", 5},     {"sig s", 5},    {"sig --", 6},  {"sig run --sig-kind ", 19},
      {"sig run --sig-kind=s", 20}, {"sig run --sig-quantity ", 23}, {"ic ", 3},
      {"ic --", 5},     {"ic ap", 5},
  };
  for (const auto &p : probes) {
    const std::string buf(p.buf);
    const auto want = cli::complete_line(buf, p.cursor, all);
    const auto got = exec.complete(buf, p.cursor);
    if (got != want) {
      std::cerr << "completion mismatch for '" << buf << "' cursor " << p.cursor << "\n";
    }
    assert(got == want);
  }
  // Spot-checks on top (the loop above pins equality; these pin meaning).
  {
    const auto c = exec.complete("o", 1);
    assert(c.size() == 1 && c[0] == "ohm");
  }
  assert(exec.complete("zzz", 3).empty());
}

void test_history_shares_repl_file() {
  // The TUI session loads the same default history path the REPL
  // appends to (`LineReader::default_history_path`, shared format and
  // cap). Prove the file-format sharing: entries written REPL-style
  // (add + append) load back TUI-style (fresh load) with identical
  // dedup/cap semantics.
  const std::string dir = "/tmp/opencode/tui7_history_probe";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = dir + "/history";
  std::remove(path.c_str());

  // REPL side: add + append (mirrors `LineReader::read_tty`).
  cli::History repl;
  assert(repl.add("ohm 12V 3A"));
  assert(repl.append_to_file(path, "ohm 12V 3A").ok());
  assert(repl.add("sim run --steps 2 --particles 1"));
  assert(repl.append_to_file(path, "sim run --steps 2 --particles 1").ok());
  assert(!repl.add("sim run --steps 2 --particles 1")); // consecutive dup ignored
  assert(!repl.add(""));                                 // blanks ignored

  // TUI side: fresh load of the same file (mirrors `init_state`).
  cli::History tui;
  assert(tui.load(path) == 2);
  assert(tui.at(0) == "ohm 12V 3A");
  assert(tui.at(1) == "sim run --steps 2 --particles 1");

  // Append from the TUI side, reload REPL-style: still one shared log.
  assert(tui.add("help ohm"));
  assert(tui.append_to_file(path, "help ohm").ok());
  cli::History reread;
  assert(reread.load(path) == 3);
  assert(reread.at(2) == "help ohm");

  // Both sides resolve the same default location (env-override aware).
  // `init_state` uses exactly this function for the TUI session path.
  const std::string def = cli::LineReader::default_history_path();
  assert(!def.empty());
  assert(def.size() >= std::string("/wengine/history").size());
  assert(def.compare(def.size() - std::string("/wengine/history").size(),
                     std::string("/wengine/history").size(), "/wengine/history") == 0);

  std::remove(path.c_str());
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
  test_parity_full_matrix();
  test_complete_parity_full();
  test_history_shares_repl_file();
  test_palette_registry_driven();
  test_palette_filter();
  test_first_token();
  test_help_pane_parity();
  test_popup_wrap();
  std::cout << "test_tui_exec: OK\n";
  return 0;
}
