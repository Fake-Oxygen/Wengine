#include "wengine/cli/args.hpp"
#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/complete.hpp"
#include "wengine/cli/logic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/cli/subshell.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine::cli;

namespace {

void test_tokenizer() {
  // Basic split, zero-copy views into input.
  std::string line = "ohm 12.4V  0.54A";
  auto toks = tokenize_view(line);
  assert(toks.size() == 3);
  assert(toks[0] == "ohm" && toks[1] == "12.4V" && toks[2] == "0.54A");

  // Quotes group whitespace.
  std::string q = R"(cmd "hello world" 'a b' bare)";
  auto qt = tokenize_view(q);
  assert(qt.size() == 4);
  assert(qt[1] == "hello world");
  assert(qt[2] == "a b");

  // Empty / whitespace only.
  assert(tokenize_view("").empty());
  assert(tokenize_view("   \t ").empty());

  // tokenize_into counts overflow without writing OOB.
  std::array<std::string_view, 2> small{};
  std::size_t n = tokenize_into("a b c d", small);
  assert(n == 4);
  assert(small[0] == "a" && small[1] == "b");
}

void test_scalars() {
  assert(parse_double("12.4").ok() && std::fabs(parse_double("12.4").value - 12.4) < 1e-12);
  assert(!parse_double("").ok());
  assert(!parse_double("12V").ok()); // suffix belongs to unit parsers
  assert(!parse_double("abc").ok());

  assert(parse_i64("-42").ok() && parse_i64("-42").value == -42);
  assert(!parse_i64("3.5").ok());

  assert(parse_bool("true").value && parse_bool("1").value);
  assert(!parse_bool("0").value && !parse_bool("off").value);
  assert(!parse_bool("maybe").ok());
}

void test_units() {
  auto v = parse_voltage("12.4V");
  assert(v.ok() && std::fabs(v.value - 12.4) < 1e-12);
  assert(std::fabs(parse_voltage("1kV").value - 1000.0) < 1e-9);
  assert(std::fabs(parse_voltage("500mV").value - 0.5) < 1e-12);
  assert(std::fabs(parse_voltage("3").value - 3.0) < 1e-12); // bare SI
  assert(!parse_voltage("5A").ok());
  assert(!parse_voltage("abc").ok());

  assert(std::fabs(parse_current("0.54A").value - 0.54) < 1e-12);
  assert(std::fabs(parse_current("2mA").value - 0.002) < 1e-12);
  assert(!parse_current("5V").ok());

  assert(std::fabs(parse_resistance("4Ohm").value - 4.0) < 1e-12);
  assert(std::fabs(parse_resistance("2kOhm").value - 2000.0) < 1e-9);
  assert(!parse_resistance("7V").ok());
}

void test_cursor_options() {
  std::array<std::string_view, 4> t{"--u", "12V", "--i=0.5A", "-R"};
  ArgCursor c{t};
  auto u = c.take_option({"--u", "-U"});
  assert(u && *u == "12V");
  auto i = c.take_option({"--i", "--current"});
  assert(i && *i == "0.5A"); // --key=value form
  assert(c.has_flag({"-R"}));
  assert(c.take_flag({"-R"}));
  assert(!c.has_remaining());
}

void test_cursor_positional() {
  std::array<std::string_view, 4> t{"12V", "--u", "9V", "0.5A"};
  ArgCursor c{t};
  // take_option consumes "--u 9V"
  auto u = c.take_option({"--u"});
  assert(u && *u == "9V");
  auto p0 = c.next_positional();
  assert(p0 && *p0 == "12V");
  auto p1 = c.next_positional();
  assert(p1 && *p1 == "0.5A");
  assert(!c.has_remaining());
}

void test_registry_compile_time() {
  static_assert(CliRegistry::find("ohm") != nullptr);
  static_assert(CliRegistry::find("s0") != nullptr);
  static_assert(CliRegistry::find("i2c") != nullptr);
  static_assert(CliRegistry::find("ldo") != nullptr);
  static_assert(CliRegistry::find("led") != nullptr);
  static_assert(CliRegistry::find("sim") != nullptr);
  static_assert(CliRegistry::find("sig") != nullptr);
  static_assert(CliRegistry::find("ic") != nullptr);
  static_assert(CliRegistry::find("logic") != nullptr);
  static_assert(CliRegistry::find("nope") == nullptr);
  static_assert(CliRegistry::all().size() == 9);
  static_assert(!CliRegistry::all()[0].usage.empty());
  static_assert(CliRegistry::find("ohm")->complete != nullptr);
  static_assert(CliRegistry::find("s0")->complete != nullptr);
  static_assert(CliRegistry::find("i2c")->complete != nullptr);
  static_assert(CliRegistry::find("ldo")->complete != nullptr);
  static_assert(CliRegistry::find("led")->complete != nullptr);
  static_assert(CliRegistry::find("sim")->complete != nullptr);
  static_assert(CliRegistry::find("sig")->complete != nullptr);
  static_assert(CliRegistry::find("ic")->complete != nullptr);
  static_assert(CliRegistry::find("logic")->complete != nullptr);
  assert(CliRegistry::find("ohm")->run != nullptr);
  assert(CliRegistry::find("s0")->run != nullptr);
  assert(CliRegistry::find("i2c")->run != nullptr);
  assert(CliRegistry::find("ldo")->run != nullptr);
  assert(CliRegistry::find("led")->run != nullptr);
  assert(CliRegistry::find("sim")->run != nullptr);
  assert(CliRegistry::find("sig")->run != nullptr);
  assert(CliRegistry::find("ic")->run != nullptr);
  assert(CliRegistry::find("logic")->run != nullptr);
}

void test_ohm_handler() {
  auto run = [](std::string line) {
    std::array<std::string_view, kMaxArgs> buf{};
    // Keep owned storage alive: tokenize views into `line`, then copy
    // views into a stable vector<string_view> is unnecessary here since
    // ArgCursor only lives for the call.
    std::size_t n = tokenize_into(line, buf);
    std::span<const std::string_view> args{buf.data(), n};
    ArgCursor cur{args};
    std::ostringstream out;
    int rc = commands::OhmSpec::run(cur, out);
    return std::pair<int, std::string>{rc, out.str()};
  };

  {
    auto [rc, s] = run("12V 3A");
    assert(rc == 0);
    assert(s.find("R = 4") != std::string::npos);
  }
  {
    auto [rc, s] = run("--u 12V --i 3A");
    assert(rc == 0 && s.find("P = 36") != std::string::npos);
  }
  {
    auto [rc, s] = run("--i 2A --r 5Ohm");
    assert(rc == 0 && s.find("U = 10") != std::string::npos);
  }
  {
    auto [rc, s] = run("--u 12V --r 4Ohm");
    assert(rc == 0 && s.find("I = 3") != std::string::npos);
  }
  {
    auto [rc, s] = run("--u=12V --i=3A");
    assert(rc == 0);
  }
  {
    auto [rc, s] = run("12V");
    assert(rc == 2); // need exactly 2
  }
  {
    auto [rc, s] = run("--u 12V --bogus 3");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run("--help");
    assert(rc == 0 && s.find("Usage") != std::string::npos);
  }
}

void test_sig_control_usage() {
  // SIG-6 per-source family: argument validation only (no server here —
  // live exchanges live in test_control_server.cpp).
  auto run = [](std::string line) {
    std::array<std::string_view, kMaxArgs> buf{};
    std::size_t n = tokenize_into(line, buf);
    std::span<const std::string_view> args{buf.data(), n};
    ArgCursor cur{args};
    std::ostringstream out;
    int rc = commands::SigSpec::run(cur, out);
    return std::pair<int, std::string>{rc, out.str()};
  };
  {
    auto [rc, s] = run("");
    assert(rc == 2); // missing mode
  }
  {
    auto [rc, s] = run("bogus");
    assert(rc == 2); // unknown mode
  }
  {
    auto [rc, s] = run("list");
    assert(rc == 2); // control endpoint required
  }
  {
    auto [rc, s] = run("list --control-tcp nope");
    assert(rc == 2); // bad endpoint shape
  }
  {
    auto [rc, s] = run("list --control-tcp 127.0.0.1:1 extra");
    assert(rc == 2); // no extra positionals
  }
  {
    auto [rc, s] = run("list --control-tcp 127.0.0.1:1 --steps 3");
    assert(rc == 2); // batch options do not leak into control modes
  }
  {
    auto [rc, s] = run("set --control-tcp 127.0.0.1:1 --sig-freq 1Hz");
    assert(rc == 2); // serial required
  }
  {
    auto [rc, s] = run("set x --control-tcp 127.0.0.1:1 --sig-freq 1Hz");
    assert(rc == 2); // bad serial
  }
  {
    auto [rc, s] = run("set 0 --control-tcp 127.0.0.1:1");
    assert(rc == 2); // at least one parameter required
  }
  {
    auto [rc, s] = run("set 0 --control-tcp 127.0.0.1:1 --sig-kind sine");
    assert(rc == 2); // kind is add-only
  }
  {
    auto [rc, s] = run("start 0 --control-tcp 127.0.0.1:1 --sig-freq 1Hz");
    assert(rc == 2); // start/stop/drop take no options
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-kind sum");
    assert(rc == 2); // sum needs operands
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-operands 0");
    assert(rc == 2); // operands need sum
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-kind table");
    assert(rc == 2); // bulk payloads stay batch-only
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-kind expression");
    assert(rc == 2); // expression text required
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-expr 't*2'");
    assert(rc == 2); // expression text needs its kind
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-kind square --sig-duty 7");
    assert(rc == 2); // engine re-validates; CLI reports usage errors
  }
  {
    auto [rc, s] = run("add --control-tcp 127.0.0.1:1 --sig-kind sine --sig-amp 2mA");
    assert(rc == 2); // dimensional firewall: mA as volts fails
  }
  {
    auto [rc, s] = run("list --control-tcp 127.0.0.1:1 --plot");
    assert(rc == 2); // run-only flag never leaks into control modes
  }
  {
    auto [rc, s] = run("--help");
    assert(rc == 0 && s.find("sig add") != std::string::npos);
  }
}

void test_sig_run_finite() {
  // Batch boundary: non-finite numerics fail loudly instead of sampling
  // inf/nan; unknown kinds fail at parse. Sinkless one-step runs.
  auto run = [](std::string line) {
    std::array<std::string_view, kMaxArgs> buf{};
    std::size_t n = tokenize_into(line, buf);
    std::span<const std::string_view> args{buf.data(), n};
    ArgCursor cur{args};
    std::ostringstream out;
    int rc = commands::SigSpec::run(cur, out);
    return std::pair<int, std::string>{rc, out.str()};
  };
  {
    auto [rc, s] = run("run --steps 1 --sig-kind sine --sig-freq 1Hz");
    assert(rc == 0);
  }
  {
    auto [rc, s] = run("run --steps 1 --sig-phase inf");
    assert(rc == 2); // shared validate_params rejects non-finite phase
    assert(s.find("invalid signal params") != std::string::npos);
  }
  {
    auto [rc, s] = run("run --steps 1 --sig-kind square --sig-duty inf");
    assert(rc == 2); // duty range rejects inf/nan where active
    assert(s.find("invalid signal params") != std::string::npos);
  }
  {
    // Inert where unread: sine ignores duty (existing pinned behavior).
    auto [rc, s] = run("run --steps 1 --sig-kind sine --sig-duty inf");
    assert(rc == 0);
  }
  {
    auto [rc, s] = run("run --steps 1 --sig-kind bogus");
    assert(rc == 2); // unknown kind at parse
  }
  {
    // --plot is consumed as a run flag (validation precedes any window):
    // bad steps still fail without opening a display.
    auto [rc, s] = run("run --steps 0 --plot");
    assert(rc == 2);
    assert(s.find("steps must be >= 1") != std::string::npos);
  }
}

void test_shell_integration() {
  Shell sh;
  sh.set_history_file(""); // no file I/O in tests
  std::ostringstream out;
  assert(sh.execute("", out) == 0);
  assert(sh.execute("ohm 12V 3A", out) == 0);
  assert(out.str().find("R = 4") != std::string::npos);
  assert(sh.execute("help ohm", out) == 0);
  assert(sh.execute("nopecmd", out) == 1);
  assert(sh.execute("ohm --help", out) == 0);
  assert(sh.execute("s0 convert --const 1000imp/kWh", out) == 0);
  assert(sh.execute("s0 --help", out) == 0);
  assert(sh.execute("ldo --vin 5V --vout 3.3V --iout 500mA", out) == 0);
  assert(out.str().find("summary: PASS") != std::string::npos);
  assert(sh.execute("led --vs 5V --vf 2V --if 20mA", out) == 0);
  assert(out.str().find("Rs exact = 150") != std::string::npos);
  assert(sh.execute("ldo --help", out) == 0);
  assert(sh.execute("sim run --steps 2 --particles 1", out) == 0);
  assert(out.str().find("solver=projectile") != std::string::npos);
  assert(sh.execute("sig run --sig-kind dc --steps 2", out) == 0);
  assert(out.str().find("sig run: kind=dc") != std::string::npos);
  assert(sh.execute("ic --help", out) == 0);
  assert(out.str().find("Available ICs") != std::string::npos);
  assert(sh.execute("exit", out) == -1);
}

void test_completion() {
  const auto all = CliRegistry::all();
  auto has = [](const std::vector<std::string> &v, std::string_view s) {
    for (const auto &e : v) {
      if (e == s) {
        return true;
      }
    }
    return false;
  };

  // Command names (registry + builtins).
  {
    const auto c = complete_line("", 0, all);
    assert(has(c, "ohm") && has(c, "s0") && has(c, "i2c") && has(c, "ldo") &&
           has(c, "led") && has(c, "sim") && has(c, "sig") && has(c, "ic") &&
           has(c, "logic") && has(c, "help") && has(c, "exit") && has(c, "quit"));
  }
  assert(complete_line("o", 1, all) == std::vector<std::string>{"ohm"});
  {
    // Registry order defines candidate order: ldo, led, then logic.
    const std::vector<std::string> want{"ldo", "led", "logic"};
    assert(complete_line("l", 1, all) == want);
  }
  assert(complete_line("log", 3, all) == std::vector<std::string>{"logic"});
  assert(complete_line("ld", 2, all) == std::vector<std::string>{"ldo"});
  assert(complete_line("le", 2, all) == std::vector<std::string>{"led"});
  {
    // "s" now prefixes three commands: definition order (s0, sim, sig).
    const auto c = complete_line("s", 1, all);
    assert(c.size() == 3 && c[0] == "s0" && c[1] == "sim" && c[2] == "sig");
  }
  {
    const auto c = complete_line("si", 2, all);
    assert(c.size() == 2 && c[0] == "sim" && c[1] == "sig");
  }
  {
    // Registry order defines candidate order: i2c before ic.
    const std::vector<std::string> want{"i2c", "ic"};
    assert(complete_line("i", 1, all) == want);
  }
  assert(complete_line("h", 1, all) == std::vector<std::string>{"help"});
  assert(complete_line("zzz", 3, all).empty());
  // Only text before the cursor counts.
  assert(complete_line("ohm --u 12V", 4, all).empty());

  // Ohm options and --from values.
  {
    const auto c = complete_line("ohm --", 6, all);
    assert(has(c, "--u") && has(c, "--from") && !has(c, "-U"));
  }
  assert(complete_line("ohm --u", 7, all) == std::vector<std::string>{"--u"});
  {
    const auto c = complete_line("ohm --from ", 11, all);
    assert(c.size() == 6 && has(c, "ui") && has(c, "ir"));
  }
  assert(complete_line("ohm --from=u", 12, all) ==
         (std::vector<std::string>{"--from=ui", "--from=ur"}));
  assert(complete_line("ohm 12V", 7, all).empty()); // numeric positional

  // S0 modes and options.
  {
    const auto c = complete_line("s0 ", 3, all);
    assert(c.size() == 6 && has(c, "rate") && has(c, "full"));
  }
  assert(complete_line("s0 r", 4, all) == std::vector<std::string>{"rate"});
  {
    const auto c = complete_line("s0 --", 5, all);
    assert(has(c, "--power") && has(c, "--const") && has(c, "--mode"));
  }
  assert(complete_line("s0 --mode ", 10, all).size() == 6);

  // I2C modes, speeds and options.
  {
    const auto c = complete_line("i2c ", 4, all);
    assert(c.size() == 3 && has(c, "pullup") && has(c, "series") && has(c, "full"));
  }
  assert(complete_line("i2c p", 5, all) == std::vector<std::string>{"pullup"});

  // LDO options (named-options only: values are numeric, nothing to complete).
  {
    const auto c = complete_line("ldo --", 6, all);
    assert(has(c, "--vin") && has(c, "--vout") && has(c, "--iout") &&
           has(c, "--iq") && has(c, "--vdo") && has(c, "--ta") && has(c, "--rth") &&
           has(c, "--tjmax"));
  }
  {
    const auto c = complete_line("ldo --v", 7, all);
    assert(c.size() == 3 && has(c, "--vin") && has(c, "--vout") && has(c, "--vdo"));
  }
  assert(complete_line("ldo --vin 5V", 12, all).empty()); // numeric value

  // LED options (named-options only: values are numeric, nothing to complete).
  {
    const auto c = complete_line("led --", 6, all);
    assert(has(c, "--vs") && has(c, "--vf") && has(c, "--if") && has(c, "--rs") &&
           has(c, "--prating"));
  }
  {
    const auto c = complete_line("led --v", 7, all);
    assert(c.size() == 2 && has(c, "--vs") && has(c, "--vf"));
  }
  assert(complete_line("led --vs 5V", 11, all).empty()); // numeric value
  {
    const auto c = complete_line("i2c --", 6, all);
    assert(has(c, "--vdd") && has(c, "--cb") && has(c, "--speed") && has(c, "--rp"));
  }
  {
    const auto c = complete_line("i2c pullup --speed ", 20, all);
    assert(c.size() == 3 && has(c, "std") && has(c, "fast") && has(c, "fmp"));
  }

  // Sim modes and options.
  assert(complete_line("sim ", 4, all) == std::vector<std::string>{"run"});
  {
    const auto c = complete_line("sim --", 6, all);
    assert(has(c, "--steps") && has(c, "--dt") && has(c, "--emit-csv") &&
           has(c, "--emit-shm") && has(c, "--emit-tcp") &&
           has(c, "--emit-udp") && has(c, "--emit-every") &&
           has(c, "--emit-shm-stride") && has(c, "--emit-shm-select") &&
           has(c, "--emit-udp-budget") && has(c, "--emit-health") && has(c, "--solver") &&
           has(c, "--emit-ros") && has(c, "--emit-ros-stride") &&
           has(c, "--emit-ros-select") && has(c, "--ros-node") &&
           has(c, "--ros-ns") && has(c, "--ros-signal-stem") &&
           has(c, "--ros-particle-stem") && has(c, "--ros-qos") &&
           has(c, "--ros-grouped") && has(c, "--ros-per-serial") &&
           has(c, "--paced") && has(c, "--rate"));
  }
  assert(complete_line("sim run --steps 10", 18, all).empty()); // numeric value
  {
    const std::vector<std::string> want{"projectile", "signal"};
    assert(complete_line("sim run --solver ", 17, all) == want);
  }
  assert(complete_line("sim run --solver=p", 18, all) ==
         std::vector<std::string>{"--solver=projectile"});
  assert(complete_line("sim run --solver=s", 18, all) ==
         std::vector<std::string>{"--solver=signal"});
  // Sig modes, options, and kind/quantity vocabularies.
  {
    const auto c = complete_line("sig ", 4, all);
    assert(c.size() == 7 && has(c, "run") && has(c, "list") && has(c, "add") &&
           has(c, "set") && has(c, "start") && has(c, "stop") && has(c, "drop"));
  }
  assert(complete_line("sig l", 5, all) == std::vector<std::string>{"list"});
  {
    const auto c = complete_line("sig s", 5, all);
    assert(c.size() == 3 && has(c, "set") && has(c, "start") && has(c, "stop"));
  }
  {
    const auto c = complete_line("sig --", 6, all);
    assert(has(c, "--steps") && has(c, "--dt") && has(c, "--emit-csv") &&
           has(c, "--emit-shm") && has(c, "--emit-tcp") && has(c, "--emit-udp") &&
           has(c, "--emit-every") && has(c, "--emit-csv-stride") &&
           has(c, "--emit-shm-select") && has(c, "--emit-udp-budget") &&
           has(c, "--emit-health") && has(c, "--forever") &&
           has(c, "--control-tcp") && has(c, "--control-uart") &&
           has(c, "--control-ros") && has(c, "--emit-uart") && has(c, "--emit-uart-mode") &&
           has(c, "--emit-uart-stride") && has(c, "--emit-uart-select") &&
           has(c, "--emit-ros") && has(c, "--emit-ros-stride") &&
           has(c, "--emit-ros-select") && has(c, "--ros-node") &&
           has(c, "--ros-ns") && has(c, "--ros-signal-stem") &&
           has(c, "--ros-particle-stem") && has(c, "--ros-qos") &&
           has(c, "--ros-grouped") && has(c, "--ros-per-serial") &&
           has(c, "--paced") && has(c, "--rate") && has(c, "--plot") && has(c, "--sig-kind") &&
           has(c, "--sig-freq") && has(c, "--sig-quantity") && has(c, "--sig-count") &&
           has(c, "--sig-expr") && has(c, "--sig-table") && has(c, "--sig-lua") &&
           has(c, "--sig-operands"));
  }
  {
    const std::vector<std::string> want{"sine",     "square",   "triangle", "saw",
                                        "dc",       "pulse",    "noise",    "sum",
                                        "expression", "table",  "lua"};
    assert(complete_line("sig run --sig-kind ", 19, all) == want);
  }
  {
    const std::vector<std::string> want{"--sig-kind=sine", "--sig-kind=square",
                                        "--sig-kind=saw", "--sig-kind=sum"};
    assert(complete_line("sig run --sig-kind=s", 20, all) == want);
  }
  {
    const std::vector<std::string> want{"U", "I"};
    assert(complete_line("sig run --sig-quantity ", 23, all) == want);
  }
  {
    const std::vector<std::string> want{"ascii", "cobs"};
    assert(complete_line("sig run --emit-uart-mode ", 25, all) == want);
    assert(complete_line("sim run --emit-uart-mode ", 25, all) == want);
  }
  {
    const std::vector<std::string> want{"latest", "reliable"};
    assert(complete_line("sim run --ros-qos ", 18, all) == want);
    assert(complete_line("sig run --ros-qos ", 18, all) == want);
  }
  assert(complete_line("sig run --ros-qos=l", 19, all) ==
         std::vector<std::string>{"--ros-qos=latest"});
  assert(complete_line("sim run --ros-qos=r", 19, all) ==
         std::vector<std::string>{"--ros-qos=reliable"});
  {
    const auto c = complete_line("sim --", 6, all);
    assert(has(c, "--emit-uart") && has(c, "--emit-uart-mode") &&
           has(c, "--emit-uart-stride") && has(c, "--emit-uart-select"));
  }
  assert(complete_line("help ", 5, all).size() == complete_line("", 0, all).size());
  assert(complete_line("help o", 6, all) == std::vector<std::string>{"ohm"});
  assert(complete_line("nope ", 5, all).empty());

  // Ic dispatch: registered chips complete by name, options still complete.
  {
    const std::vector<std::string> want{"ap63205", "ina219", "tmc22xx"};
    assert(complete_line("ic ", 3, all) == want);
  }
  assert(complete_line("ic --", 5, all) == std::vector<std::string>{"--help"});

  // Common prefix.
  {
    const std::vector<std::string> empty;
    assert(common_prefix(empty) == "");
  }
  assert(common_prefix({{"--power", "--power-factor"}}) == "--power");
  assert(common_prefix({{"rate", "power"}}) == "");
}

void test_shell_run_fallback() {
  // Non-TTY streams (pipes/tests) use the plain getline path.
  Shell sh;
  sh.set_history_file("");
  std::istringstream in("ohm 12V 3A\nnopecmd\nexit\n");
  std::ostringstream out;
  sh.run(in, out);
  const std::string s = out.str();
  assert(s.find("R = 4") != std::string::npos);
  assert(s.find("Unknown command: nopecmd") != std::string::npos);
}

void test_shell_headless() {
  Shell sh;
  sh.set_history_file("");
  std::ostringstream out;
  // argv-style: pre-split tokens run verbatim.
  {
    const std::array<std::string_view, 3> argv{"ohm", "12V", "3A"};
    assert(sh.execute_argv(argv, out) == 0);
    assert(out.str().find("R = 4") != std::string::npos);
  }
  // No re-splitting: a spaced token stays ONE argument (invalid voltage).
  {
    const std::array<std::string_view, 2> argv{"ohm", "12V 3A"};
    assert(sh.execute_argv(argv, out) == 2);
  }
  // REPL parity: same command via line and via argv agrees byte-for-byte.
  {
    std::ostringstream a, b;
    const std::array<std::string_view, 4> argv{"s0", "convert", "--const",
                                               "1000imp/kWh"};
    assert(sh.execute("s0 convert --const 1000imp/kWh", a) == 0);
    assert(sh.execute_argv(argv, b) == 0);
    assert(a.str() == b.str());
  }
  // --help shares the help path (also in headless: `./app --help`).
  {
    const std::array<std::string_view, 1> argv{"--help"};
    assert(sh.execute_argv(argv, out) == 0);
    assert(out.str().find("Available commands") != std::string::npos);
  }
  {
    const std::array<std::string_view, 2> argv{"help", "s0"};
    assert(sh.execute_argv(argv, out) == 0);
  }
  {
    const std::array<std::string_view, 1> argv{"nope"};
    assert(sh.execute_argv(argv, out) == 1);
  }
  {
    const std::array<std::string_view, 1> argv{"exit"};
    assert(sh.execute_argv(argv, out) == -1); // main maps this to exit 0
  }
  {
    const std::vector<std::string_view> empty;
    assert(sh.execute_argv(empty, out) == 0);
  }
  {
    // Over the token cap, like the REPL path.
    std::vector<std::string_view> many(70, "x");
    many[0] = "ohm";
    assert(sh.execute_argv(many, out) == 1);
  }
}

void test_history() {
  History h(3);
  assert(h.empty());
  assert(h.add("ohm 12V 3A"));
  assert(h.add("s0 rate --power 2kW --const 1000imp/kWh"));
  assert(!h.add(""));                            // empty ignored
  assert(!h.add("s0 rate --power 2kW --const 1000imp/kWh")); // dup ignored
  assert(h.size() == 2);
  assert(h.add("a") && h.add("b")); // over capacity: drops oldest
  assert(h.size() == 3);
  assert(h.at(0) == "s0 rate --power 2kW --const 1000imp/kWh");

  // Persistence round-trip.
  const std::string path = "/tmp/wengine_test_history.txt";
  std::remove(path.c_str());
  assert(h.save(path).ok());
  History loaded;
  assert(loaded.load(path) == 3);
  assert(loaded.at(2) == "b");
  assert(loaded.add("c"));
  assert(loaded.append_to_file(path, "c").ok());
  History reloaded;
  assert(reloaded.load(path) == 4);
  std::remove(path.c_str());
  assert(reloaded.load("/tmp/wengine_no_such_file_xyz.txt") == 0); // missing OK

  // LineReader honors disabled persistence and plain-stream reads.
  LineReader reader;
  reader.set_history_file("");
  assert(reader.history().empty());
  std::istringstream in("hello\n");
  std::ostringstream out;
  auto line = reader.read_line("> ", in, out);
  assert(line && *line == "hello");
  assert(out.str() == "> ");
  std::istringstream eof_in("");
  assert(!reader.read_line("> ", eof_in, out));
}

void test_logic_session() {
  // Module registry shape (LOGIC-1: help + back; LOGIC-8 appends verbs).
  static_assert(logic::LogicRegistry::all().size() == 2);
  static_assert(logic::LogicRegistry::find("help") != nullptr);
  static_assert(logic::LogicRegistry::find("back") != nullptr);
  static_assert(logic::LogicRegistry::find("wire") == nullptr);

  Shell sh;
  sh.set_history_file("");
  std::ostringstream out;

  // Bare `logic` requests the session without printing (the session loop
  // owns the entry banner).
  {
    std::ostringstream silent;
    assert(sh.execute("logic", silent) == kEnterSubShell);
    assert(silent.str().empty());
    assert(sh.has_pending_session());
  }
  // One-shot `logic help` == session `help`: same verb, same text.
  {
    std::ostringstream one_shot, direct;
    assert(sh.execute("logic help", one_shot) == 0);
    const std::span<const std::string_view> no_args{};
    ArgCursor cur{no_args};
    assert(logic::LogicHelpSpec::run(cur, direct) == 0);
    assert(one_shot.str() == direct.str());
    assert(one_shot.str().find("back - Leave the logic session") != std::string::npos);
  }
  // Session-control one-shot is a loud usage error, never silent.
  assert(sh.execute("logic back", out) == 2);
  assert(out.str().find("no session is active") != std::string::npos);
  assert(sh.execute("logic nope", out) == 2);
  assert(out.str().find("unknown logic command") != std::string::npos);
  assert(sh.execute("logic back extra", out) == 2);
  assert(sh.execute("logic --help", out) == 0);
  assert(out.str().find("Session verbs") != std::string::npos);
  assert(sh.execute("help logic", out) == 0);
  assert(out.str().find("[logic]>") != std::string::npos);

  // Full REPL drive over plain streams: prompt swap, session help, back
  // returns to the top level, then quit.
  {
    Shell repl;
    repl.set_history_file("");
    std::istringstream in("logic\nhelp\nback\nexit\n");
    std::ostringstream session_out;
    repl.run(in, session_out);
    const std::string s = session_out.str();
    assert(s.find("Entering logic session") != std::string::npos);
    assert(s.find("[logic]> ") != std::string::npos);
    assert(s.find("Logic session commands") != std::string::npos);
    assert(s.find("Leaving logic session.") != std::string::npos);
    assert(!repl.has_pending_session());
  }
  // `exit` inside the session leaves it (only the outer loop quits).
  {
    Shell repl;
    repl.set_history_file("");
    std::istringstream in("logic\nexit\nexit\n");
    std::ostringstream session_out;
    repl.run(in, session_out);
    const std::string s = session_out.str();
    assert(s.find("Entering logic session") != std::string::npos);
    assert(s.find("Leaving logic session.") != std::string::npos);
  }

  // Scoped completion: module registry sees only module verbs (+ session
  // builtins via complete_line), never top-level commands.
  {
    const auto mod = logic::LogicRegistry::all();
    auto has = [](const std::vector<std::string> &v, std::string_view word) {
      for (const auto &e : v) {
        if (e == word) {
          return true;
        }
      }
      return false;
    };
    const auto c = complete_line("", 0, mod);
    assert(has(c, "help") && has(c, "back") && has(c, "exit") && has(c, "quit"));
    assert(!has(c, "ohm") && !has(c, "sim") && !has(c, "logic"));
    assert(complete_line("b", 1, mod) == std::vector<std::string>{"back"});
    // Registry names shadow builtins: the session `help` verb lists once
    // (complete_names dedupes; the registry entry wins).
    const std::vector<std::string> want_h{"help"};
    assert(complete_line("h", 1, mod) == want_h);
    assert(complete_line("zzz", 3, mod).empty());
    // Entry completion: verbs plus --help, then per-verb delegation.
    const auto all = CliRegistry::all();
    const auto verbs = complete_line("logic ", 6, all);
    assert(has(verbs, "help") && has(verbs, "back"));
    assert(!has(verbs, "ohm") && !has(verbs, "--help")); // flags need a '-' prefix
    const std::vector<std::string> want_dash_help{"--help"};
    assert(complete_line("logic --h", 9, all) == want_dash_help);
    const std::vector<std::string> want_help{"help"};
    assert(complete_line("logic h", 7, all) == want_help);
    assert(complete_line("logic help ", 11, all).size() == 2); // help|back
  }
}

} // namespace

int main() {
  test_tokenizer();
  test_scalars();
  test_units();
  test_cursor_options();
  test_cursor_positional();
  test_registry_compile_time();
  test_ohm_handler();
  test_sig_control_usage();
  test_sig_run_finite();
  test_completion();
  test_shell_integration();
  test_shell_run_fallback();
  test_shell_headless();
  test_history();
  test_logic_session();
  std::cout << "test_cli_args: OK\n";
  return 0;
}
