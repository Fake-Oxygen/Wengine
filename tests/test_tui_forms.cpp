// TUI-3 guided forms: composition-only parity with the headless CLI.
//
// The forms must never reimplement parsing: every submission composes one
// argv string run through `TuiExecutor` (i.e. `cli::Shell::execute`), so
// valid output and usage-error text agree byte-for-byte with the CLI by
// construction. This pins that contract over a matrix covering every
// option of ohm/s0/i2c/ldo/led plus the ic per-chip dispatch, and that
// field keys/usages track the specs' kUsage/complete vocabulary.

#include "wengine/tui/exec.hpp"
#include "wengine/tui/forms.hpp"
#include "wengine/cli/complete.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;

namespace {

const tui::forms::FormDef *find_form(const std::vector<tui::forms::FormDef> &defs,
                                     std::string_view cmd) {
  for (const auto &d : defs) {
    if (d.command == cmd) {
      return &d;
    }
  }
  return nullptr;
}

int field_index(const tui::forms::FormDef &def, std::string_view key_or_label) {
  for (std::size_t i = 0; i < def.fields.size(); ++i) {
    if (def.fields[i].key == key_or_label || def.fields[i].label == key_or_label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::vector<std::string> blank(const tui::forms::FormDef &def) {
  return std::vector<std::string>(def.fields.size());
}

void set(std::vector<std::string> &vals, const tui::forms::FormDef &def,
         std::string_view key_or_label, std::string_view value) {
  const int i = field_index(def, key_or_label);
  assert(i >= 0);
  vals[static_cast<std::size_t>(i)] = std::string(value);
}

void check_parity(tui::TuiExecutor &exec, cli::Shell &shell,
                  const tui::forms::FormDef &def, const std::vector<std::string> &vals) {
  const std::string line = tui::forms::compose_line(def, vals);
  std::ostringstream expected;
  const int rc_shell = shell.execute(line, expected);
  const tui::ExecResult got = exec.execute_line(line);
  if (got.rc != rc_shell || got.text != expected.str()) {
    std::cerr << "parity mismatch for line: " << line << "\n--- shell rc=" << rc_shell
              << " text:\n"
              << expected.str() << "\n--- tui rc=" << got.rc << " text:\n"
              << got.text << "\n";
  }
  assert(got.rc == rc_shell);
  assert(got.text == expected.str());
}

void test_defs_track_specs() {
  const auto defs = tui::forms::form_defs();
  assert(defs.size() == 6);
  // Order is the acceptance order (stable for the TUI list).
  assert(defs[0].command == "ohm");
  assert(defs[1].command == "s0");
  assert(defs[2].command == "i2c");
  assert(defs[3].command == "ldo");
  assert(defs[4].command == "led");
  assert(defs[5].command == "ic");
  // Usage aliases the live spec text verbatim (hints come from kUsage).
  assert(defs[0].usage == cli::commands::OhmSpec::kUsage);
  assert(defs[1].usage == cli::commands::S0Spec::kUsage);
  assert(defs[2].usage == cli::commands::I2cSpec::kUsage);
  assert(defs[3].usage == cli::commands::LdoSpec::kUsage);
  assert(defs[4].usage == cli::commands::LedSpec::kUsage);
  assert(defs[5].usage == cli::commands::IcSpec::kUsage);
  assert(defs[0].description == cli::commands::OhmSpec::kDescription);
  assert(!defs[0].fields.empty() && !defs[5].fields.empty());
}

void test_keys_match_complete_vocab() {
  const auto defs = tui::forms::form_defs();
  const auto all = cli::CliRegistry::all();
  auto has = [](const std::vector<std::string> &v, std::string_view s) {
    for (const auto &e : v) {
      if (e == s) {
        return true;
      }
    }
    return false;
  };
  // Every form field flag must be a completion candidate for its command
  // (same vocabulary in the input line and the form). Aliases
  // (--voltage/-U, ...) stay available on the line; the form emits the
  // canonical spelling, so only the forward direction is pinned here.
  // The `ic` top level completes chips (options complete per chip), so it
  // is checked against the chip vocabulary below instead.
  for (const auto &d : defs) {
    if (d.command == "ic") {
      continue;
    }
    const std::string prefix = std::string(d.command) + " --";
    const auto cand = cli::complete_line(prefix, prefix.size(), all);
    for (const auto &f : d.fields) {
      if (f.key.empty() || f.key == "--") {
        continue;
      }
      if (f.key.front() != '-') {
        continue;
      }
      assert(has(cand, std::string(f.key)));
    }
  }
  // Canonical per-command coverage (every functional option reachable).
  {
    const auto *ohm = find_form(defs, "ohm");
    assert(ohm != nullptr);
    for (std::string_view k : {"--u", "--i", "--r", "--from"}) {
      assert(field_index(*ohm, k) >= 0);
    }
  }
  {
    const auto *s0 = find_form(defs, "s0");
    assert(s0 != nullptr);
    for (std::string_view k :
         {"--power", "--freq", "--pulses", "--const", "--time", "--energy", "--u",
          "--i", "--pf"}) {
      assert(field_index(*s0, k) >= 0);
    }
    assert(field_index(*s0, "mode") >= 0);
  }
  {
    const auto *i2c = find_form(defs, "i2c");
    assert(i2c != nullptr);
    for (std::string_view k :
         {"--vdd", "--cb", "--speed", "--vol", "--iol", "--tr", "--rp", "--rs"}) {
      assert(field_index(*i2c, k) >= 0);
    }
    assert(field_index(*i2c, "mode") >= 0);
  }
  {
    const auto *ldo = find_form(defs, "ldo");
    assert(ldo != nullptr);
    for (std::string_view k :
         {"--vin", "--vout", "--iout", "--iq", "--vdo", "--ta", "--rth", "--tjmax"}) {
      assert(field_index(*ldo, k) >= 0);
    }
  }
  {
    const auto *led = find_form(defs, "led");
    assert(led != nullptr);
    for (std::string_view k : {"--vs", "--vf", "--if", "--rs", "--prating"}) {
      assert(field_index(*led, k) >= 0);
    }
  }
}

void test_ic_registry_driven() {
  // Chips come from IcRegistry, never hardcoded.
  const auto chips = tui::forms::ic_chip_names();
  assert(!chips.empty());
  assert(chips.size() == cli::IcRegistry::all().size());
  assert(chips[0] == cli::IcRegistry::all()[0].name);
  bool has_ap = false;
  for (const auto &c : chips) {
    has_ap = has_ap || (c == "ap63205");
  }
  assert(has_ap);
  // Ops come from the chip's own complete (per-chip subcommands).
  const auto ops = tui::forms::ic_op_names("ap63205");
  for (std::string_view want :
       {"inductor", "cout", "uvlo", "delay", "thermal", "spec"}) {
    bool found = false;
    for (const auto &o : ops) {
      found = found || (o == want);
    }
    assert(found);
  }
  assert(tui::forms::ic_op_names("nope-chip").empty());
  // The ic form covers the chip's option vocabulary.
  const auto opts = tui::forms::ic_option_names("ap63205");
  const auto defs = tui::forms::form_defs();
  const auto *ic = find_form(defs, "ic");
  assert(ic != nullptr);
  assert(field_index(*ic, "chip") >= 0 && field_index(*ic, "op") >= 0);
  for (const auto &o : opts) {
    if (o == "--help" || o == "-h") {
      continue; // help lives in the help pane, not the form
    }
    assert(field_index(*ic, o) >= 0);
  }
}

void test_compose_shapes() {
  const auto defs = tui::forms::form_defs();
  const auto *ohm = find_form(defs, "ohm");
  assert(ohm != nullptr);
  {
    auto v = blank(*ohm);
    assert(tui::forms::compose_line(*ohm, v) == "ohm");
  }
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "12V");
    set(v, *ohm, "--i", "3A");
    assert(tui::forms::compose_line(*ohm, v) == "ohm --u 12V --i 3A");
  }
  {
    // Empty/whitespace fields are omitted; values trim.
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "  12V ");
    set(v, *ohm, "--i", "   ");
    assert(tui::forms::compose_line(*ohm, v) == "ohm --u 12V");
  }
  {
    // Positional slots (mode/chip/op) emit bare tokens in field order.
    const auto *ic = find_form(defs, "ic");
    assert(ic != nullptr);
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "spec");
    assert(tui::forms::compose_line(*ic, v) == "ic ap63205 spec");
  }
}

void test_parity_matrix() {
  const auto defs = tui::forms::form_defs();
  tui::TuiExecutor exec;
  exec.set_history_file("");
  cli::Shell shell;
  shell.set_history_file("");

  const auto *ohm = find_form(defs, "ohm");
  const auto *s0 = find_form(defs, "s0");
  const auto *i2c = find_form(defs, "i2c");
  const auto *ldo = find_form(defs, "ldo");
  const auto *led = find_form(defs, "led");
  const auto *ic = find_form(defs, "ic");
  assert(ohm && s0 && i2c && ldo && led && ic);

  // ohm: all three solve pairs.
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "12V");
    set(v, *ohm, "--i", "3A");
    check_parity(exec, shell, *ohm, v);
    const auto r = exec.execute_line(tui::forms::compose_line(*ohm, v));
    assert(r.rc == 0 && r.text.find("R = 4") != std::string::npos);
  }
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "12V");
    set(v, *ohm, "--r", "4Ohm");
    check_parity(exec, shell, *ohm, v);
  }
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--i", "2A");
    set(v, *ohm, "--r", "5Ohm");
    check_parity(exec, shell, *ohm, v);
  }
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "1kV");
    set(v, *ohm, "--r", "4Ohm");
    check_parity(exec, shell, *ohm, v);
  }
  {
    auto v = blank(*ohm);
    set(v, *ohm, "--u", "12.4V");
    set(v, *ohm, "--i", "0.54A");
    check_parity(exec, shell, *ohm, v);
  }

  // s0: every mode.
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "rate");
    set(v, *s0, "--power", "2kW");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "power");
    set(v, *s0, "--freq", "0.555Hz");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "energy");
    set(v, *s0, "--pulses", "500");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "pulses");
    set(v, *s0, "--power", "2kW");
    set(v, *s0, "--time", "1h");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "pulses");
    set(v, *s0, "--energy", "2kWh");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "convert");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "full");
    set(v, *s0, "--u", "230V");
    set(v, *s0, "--i", "10A");
    set(v, *s0, "--const", "1000imp/kWh");
    set(v, *s0, "--time", "1h");
    check_parity(exec, shell, *s0, v);
  }
  {
    auto v = blank(*s0);
    set(v, *s0, "mode", "full");
    set(v, *s0, "--u", "230V");
    set(v, *s0, "--i", "10A");
    set(v, *s0, "--pf", "0.9");
    set(v, *s0, "--const", "1000imp/kWh");
    check_parity(exec, shell, *s0, v);
  }

  // i2c: every mode + each override option.
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "pullup");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    check_parity(exec, shell, *i2c, v);
  }
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "pullup");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    set(v, *i2c, "--rp", "1.5kOhm");
    check_parity(exec, shell, *i2c, v);
  }
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "pullup");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    set(v, *i2c, "--speed", "std");
    check_parity(exec, shell, *i2c, v);
  }
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "pullup");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    set(v, *i2c, "--speed", "fmp");
    set(v, *i2c, "--vol", "0.4V");
    set(v, *i2c, "--iol", "3mA");
    set(v, *i2c, "--tr", "300ns");
    check_parity(exec, shell, *i2c, v);
  }
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "series");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    set(v, *i2c, "--rp", "1.5kOhm");
    set(v, *i2c, "--rs", "100Ohm");
    check_parity(exec, shell, *i2c, v);
  }
  {
    auto v = blank(*i2c);
    set(v, *i2c, "mode", "full");
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    set(v, *i2c, "--rp", "1.5kOhm");
    set(v, *i2c, "--rs", "100Ohm");
    check_parity(exec, shell, *i2c, v);
  }

  // ldo: every option.
  {
    auto v = blank(*ldo);
    set(v, *ldo, "--vin", "5V");
    set(v, *ldo, "--vout", "3.3V");
    set(v, *ldo, "--iout", "500mA");
    check_parity(exec, shell, *ldo, v);
  }
  {
    auto v = blank(*ldo);
    set(v, *ldo, "--vin", "5V");
    set(v, *ldo, "--vout", "3.3V");
    set(v, *ldo, "--iout", "500mA");
    set(v, *ldo, "--iq", "50uA");
    set(v, *ldo, "--vdo", "0.3V");
    set(v, *ldo, "--ta", "85");
    set(v, *ldo, "--rth", "100");
    set(v, *ldo, "--tjmax", "125");
    check_parity(exec, shell, *ldo, v);
  }

  // led: every option.
  {
    auto v = blank(*led);
    set(v, *led, "--vs", "5V");
    set(v, *led, "--vf", "2V");
    set(v, *led, "--if", "20mA");
    check_parity(exec, shell, *led, v);
  }
  {
    auto v = blank(*led);
    set(v, *led, "--vs", "5V");
    set(v, *led, "--vf", "3.2V");
    set(v, *led, "--if", "20mA");
    set(v, *led, "--rs", "100Ohm");
    check_parity(exec, shell, *led, v);
  }
  {
    auto v = blank(*led);
    set(v, *led, "--vs", "5V");
    set(v, *led, "--vf", "2V");
    set(v, *led, "--if", "20mA");
    set(v, *led, "--prating", "0.25W");
    check_parity(exec, shell, *led, v);
  }

  // ic: every ap63205 op + every option spelling.
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "inductor");
    set(v, *ic, "--vin", "12V");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "inductor");
    set(v, *ic, "--vin", "12V");
    set(v, *ic, "--iout", "2A");
    set(v, *ic, "--ripple", "0.4");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "inductor");
    set(v, *ic, "--vin", "12V");
    set(v, *ic, "--delta-il", "0.8A");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "cout");
    set(v, *ic, "--vin", "12V");
    set(v, *ic, "--esr", "5mOhm");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "cout");
    set(v, *ic, "--vin", "12V");
    set(v, *ic, "--iout", "1A");
    set(v, *ic, "--delta-il", "0.8A");
    set(v, *ic, "--esr", "5mOhm");
    set(v, *ic, "--overshoot", "0.25V");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "uvlo");
    set(v, *ic, "--von", "8V");
    set(v, *ic, "--voff", "7V");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "delay");
    set(v, *ic, "--ts", "10ms");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "delay");
    set(v, *ic, "--cd", "12.7nF");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "thermal");
    set(v, *ic, "--pd", "0.5W");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "thermal");
    set(v, *ic, "--vin", "12V");
    set(v, *ic, "--iout", "2A");
    set(v, *ic, "--ta", "85");
    check_parity(exec, shell, *ic, v);
  }
  {
    auto v = blank(*ic);
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "spec");
    check_parity(exec, shell, *ic, v);
  }
}

void test_invalid_surfaces_cli_error() {
  const auto defs = tui::forms::form_defs();
  tui::TuiExecutor exec;
  exec.set_history_file("");
  cli::Shell shell;
  shell.set_history_file("");

  auto expect_usage = [&](const tui::forms::FormDef &def, std::vector<std::string> vals) {
    const std::string line = tui::forms::compose_line(def, vals);
    std::ostringstream expected;
    const int rc_shell = shell.execute(line, expected);
    const tui::ExecResult got = exec.execute_line(line);
    assert(got.rc == rc_shell);
    assert(got.text == expected.str());
    // No form-side reimplementation: the CLI owns the verdict. Usage errors
    // carry the CLI error text plus a usage body (per-chip `ic` errors print
    // the chip usage, the unknown-chip refusal lists chips — accept either).
    assert(got.rc == 2);
    assert(got.text.find("Error") != std::string::npos);
    const bool has_usage_shape = got.text.find("Usage") != std::string::npos ||
                                 got.text.find(def.command) != std::string::npos ||
                                 got.text.find("Available ICs") != std::string::npos;
    assert(has_usage_shape);
  };

  const auto *ohm = find_form(defs, "ohm");
  const auto *s0 = find_form(defs, "s0");
  const auto *i2c = find_form(defs, "i2c");
  const auto *ldo = find_form(defs, "ldo");
  const auto *led = find_form(defs, "led");
  const auto *ic = find_form(defs, "ic");
  assert(ohm && s0 && i2c && ldo && led && ic);

  {
    auto v = blank(*ohm); // exactly-two rule
    set(v, *ohm, "--u", "12V");
    expect_usage(*ohm, v);
  }
  {
    auto v = blank(*ohm); // bad unit text goes to the CLI parser
    set(v, *ohm, "--u", "12x");
    set(v, *ohm, "--i", "3A");
    expect_usage(*ohm, v);
  }
  {
    auto v = blank(*s0); // rate without its meter constant
    set(v, *s0, "mode", "rate");
    set(v, *s0, "--power", "2kW");
    expect_usage(*s0, v);
  }
  {
    auto v = blank(*s0); // bad power text
    set(v, *s0, "mode", "rate");
    set(v, *s0, "--power", "2xW");
    set(v, *s0, "--const", "1000imp/kWh");
    expect_usage(*s0, v);
  }
  {
    auto v = blank(*i2c); // missing mode
    set(v, *i2c, "--vdd", "3.3V");
    set(v, *i2c, "--cb", "200pF");
    expect_usage(*i2c, v);
  }
  {
    auto v = blank(*i2c); // missing Cb
    set(v, *i2c, "mode", "pullup");
    set(v, *i2c, "--vdd", "3.3V");
    expect_usage(*i2c, v);
  }
  {
    auto v = blank(*ldo); // missing IOUT
    set(v, *ldo, "--vin", "5V");
    set(v, *ldo, "--vout", "3.3V");
    expect_usage(*ldo, v);
  }
  {
    auto v = blank(*led); // missing IF
    set(v, *led, "--vs", "5V");
    set(v, *led, "--vf", "2V");
    expect_usage(*led, v);
  }
  {
    auto v = blank(*ic); // unknown chip: registry refusal via the CLI
    set(v, *ic, "chip", "ne555");
    set(v, *ic, "op", "astable");
    expect_usage(*ic, v);
  }
  {
    auto v = blank(*ic); // unknown op on a known chip
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "bogus");
    expect_usage(*ic, v);
  }
  {
    auto v = blank(*ic); // missing --vin reaches the chip validator
    set(v, *ic, "chip", "ap63205");
    set(v, *ic, "op", "inductor");
    expect_usage(*ic, v);
  }
}

} // namespace

int main() {
  test_defs_track_specs();
  test_keys_match_complete_vocab();
  test_ic_registry_driven();
  test_compose_shapes();
  test_parity_matrix();
  test_invalid_surfaces_cli_error();
  std::cout << "test_tui_forms: OK\n";
  return 0;
}
