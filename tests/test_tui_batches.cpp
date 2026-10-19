// TUI-4 run builders: composition-only parity with the headless CLI.
//
// Builders must never reimplement parsing: every submission composes one
// argv string run through `TuiExecutor` (i.e. `cli::Shell::execute`), so
// valid output and usage-error text agree byte-for-byte with the CLI by
// construction. This pins that contract over a matrix covering projectile
// batches, every batch signal kind (sine/square/noise/sum/expression/
// table/lua), paced runs, emit strides/selects, plus the --forever async
// routing (composed but never executed synchronously).

#include "wengine/tui/batches.hpp"
#include "wengine/tui/exec.hpp"
#include "wengine/cli/complete.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;

namespace {

const tui::batches::BatchDef *find_batch(const std::vector<tui::batches::BatchDef> &defs,
                                         std::string_view cmd) {
  for (const auto &d : defs) {
    if (d.command == cmd) {
      return &d;
    }
  }
  return nullptr;
}

int field_index(const tui::batches::BatchDef &def, std::string_view key) {
  for (std::size_t i = 0; i < def.fields.size(); ++i) {
    if (def.fields[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::vector<std::string> blank(const tui::batches::BatchDef &def) {
  return std::vector<std::string>(def.fields.size());
}

void set(std::vector<std::string> &vals, const tui::batches::BatchDef &def,
         std::string_view key, std::string_view value) {
  const int i = field_index(def, key);
  assert(i >= 0);
  vals[static_cast<std::size_t>(i)] = std::string(value);
}

void check_parity(tui::TuiExecutor &exec, cli::Shell &shell,
                  const tui::batches::BatchDef &def, const std::vector<std::string> &vals) {
  const std::string line = tui::batches::compose_line(def, vals);
  assert(!tui::batches::wants_async(def, vals)); // async lines never run here
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

std::string tmp(const char *name) { return std::string("/tmp/opencode/") + name; }

// Paced runs embed wall-clock lateness (`late=`, `max_late_us=`) that
// varies run-to-run even between two headless CLI invocations, so exact
// text parity is the wrong assertion there. Pin rc parity plus the
// deterministic lines instead (solver/kind + step budget + paced rate).
void check_paced_parity(tui::TuiExecutor &exec, cli::Shell &shell,
                        const tui::batches::BatchDef &def,
                        const std::vector<std::string> &vals, std::string_view want_rate,
                        std::string_view want_steps) {
  const std::string line = tui::batches::compose_line(def, vals);
  assert(!tui::batches::wants_async(def, vals));
  std::ostringstream expected;
  const int rc_shell = shell.execute(line, expected);
  const tui::ExecResult got = exec.execute_line(line);
  if (got.rc != rc_shell) {
    std::cerr << "paced rc mismatch for line: " << line << " shell=" << rc_shell
              << " tui=" << got.rc << "\n";
  }
  assert(got.rc == rc_shell && got.rc == 0);
  const std::string exp_text = expected.str();
  for (const std::string *text : {&exp_text, &got.text}) {
    if (text->find(want_rate) == std::string::npos ||
        text->find(want_steps) == std::string::npos ||
        text->find("paced: ") == std::string::npos) {
      std::cerr << "paced output missing markers for line: " << line << "\ntext:\n"
                << *text << "\n";
    }
    assert(text->find(want_rate) != std::string::npos);
    assert(text->find(want_steps) != std::string::npos);
    assert(text->find("paced: ") != std::string::npos);
  }
}

void test_defs_track_specs() {
  const auto defs = tui::batches::batch_defs();
  assert(defs.size() == 2);
  assert(static_cast<int>(tui::batches::kBatchCount) == 2);
  assert(defs[0].command == "sim");
  assert(defs[0].mode == "run");
  assert(defs[1].command == "sig");
  assert(defs[1].mode == "run");
  assert(defs[0].usage == cli::commands::SimSpec::kUsage);
  assert(defs[1].usage == cli::commands::SigSpec::kUsage);
  assert(defs[0].description == cli::commands::SimSpec::kDescription);
  assert(defs[1].description == cli::commands::SigSpec::kDescription);
  assert(!defs[0].fields.empty() && !defs[1].fields.empty());
}

void test_keys_match_complete_vocab() {
  const auto defs = tui::batches::batch_defs();
  const auto all = cli::CliRegistry::all();
  auto has = [](const std::vector<std::string> &v, std::string_view s) {
    for (const auto &e : v) {
      if (e == s) {
        return true;
      }
    }
    return false;
  };
  // Every builder flag must be a completion candidate for its command's
  // run line (same vocabulary in the input line and the builder).
  for (const auto &d : defs) {
    const std::string prefix = std::string(d.command) + " run --";
    const auto cand = cli::complete_line(prefix, prefix.size(), all);
    for (const auto &f : d.fields) {
      if (f.key.empty() || f.key.front() != '-') {
        continue;
      }
      assert(has(cand, std::string(f.key)));
    }
  }
  // Canonical per-builder coverage (every functional batch option).
  {
    const auto *sim = find_batch(defs, "sim");
    assert(sim != nullptr);
    for (std::string_view k : {"--solver", "--steps", "--dt", "--gravity", "--particles",
                               "--x0", "--y0", "--vx0", "--vy0", "--emit-every",
                               "--emit-csv", "--emit-csv-stride", "--emit-csv-select",
                               "--emit-shm", "--emit-shm-stride", "--emit-shm-select",
                               "--emit-tcp", "--emit-tcp-stride", "--emit-tcp-select",
                               "--emit-udp", "--emit-udp-stride", "--emit-udp-select",
                               "--emit-udp-budget", "--emit-uart", "--emit-uart-mode",
                               "--emit-uart-stride", "--emit-uart-select", "--paced",
                               "--rate", "--emit-health"}) {
      assert(field_index(*sim, k) >= 0);
    }
  }
  {
    const auto *sig = find_batch(defs, "sig");
    assert(sig != nullptr);
    for (std::string_view k : {"--sig-kind", "--sig-freq", "--sig-amp", "--sig-offset",
                               "--sig-phase", "--sig-duty", "--sig-seed", "--sig-quantity",
                               "--sig-count", "--sig-expr", "--sig-table", "--sig-lua",
                               "--steps", "--dt", "--emit-every", "--emit-csv",
                               "--emit-csv-stride", "--emit-csv-select", "--emit-shm",
                               "--emit-shm-stride", "--emit-shm-select", "--emit-tcp",
                               "--emit-tcp-stride", "--emit-tcp-select", "--emit-udp",
                               "--emit-udp-stride", "--emit-udp-select", "--emit-udp-budget",
                               "--emit-uart", "--emit-uart-mode", "--emit-uart-stride",
                               "--emit-uart-select", "--control-tcp", "--control-uart",
                               "--paced", "--rate", "--forever", "--emit-health"}) {
      assert(field_index(*sig, k) >= 0);
    }
  }
}

void test_registry_driven() {
  const auto solvers = tui::batches::solver_names();
  assert(solvers.size() == solvers::SolverRegistry::all().size());
  assert(!solvers.empty());
  assert(solvers[0] == solvers::SolverRegistry::all()[0].name);
  bool has_projectile = false;
  bool has_signal = false;
  for (const auto &s : solvers) {
    has_projectile = has_projectile || (s == "projectile");
    has_signal = has_signal || (s == "signal");
  }
  assert(has_projectile && has_signal);
  const auto kinds = tui::batches::signal_kind_names();
  assert(kinds.size() == signal::kKindNames.size());
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    assert(kinds[i] == signal::kKindNames[i]);
  }
  for (std::string_view want :
       {"sine", "square", "noise", "sum", "expression", "table", "lua"}) {
    bool found = false;
    for (const auto &k : kinds) {
      found = found || (k == want);
    }
    assert(found);
  }
  const auto qs = tui::batches::quantity_names();
  assert(qs.size() == 2 && qs[0] == "U" && qs[1] == "I");
  const auto modes = tui::batches::uart_modes();
  assert(modes.size() == 2 && modes[0] == "ascii" && modes[1] == "cobs");
}

void test_compose_shapes() {
  const auto defs = tui::batches::batch_defs();
  const auto *sim = find_batch(defs, "sim");
  const auto *sig = find_batch(defs, "sig");
  assert(sim && sig);
  {
    auto v = blank(*sim);
    assert(tui::batches::compose_line(*sim, v) == "sim run");
    assert(!tui::batches::wants_async(*sim, v));
  }
  {
    auto v = blank(*sig);
    assert(tui::batches::compose_line(*sig, v) == "sig run");
    assert(!tui::batches::wants_async(*sig, v));
  }
  {
    // Empty/whitespace fields omitted; values trim.
    auto v = blank(*sim);
    set(v, *sim, "--steps", "  100 ");
    set(v, *sim, "--dt", "   ");
    assert(tui::batches::compose_line(*sim, v) == "sim run --steps 100");
  }
  {
    // Bare flags emit the key only when enabled.
    auto v = blank(*sim);
    set(v, *sim, "--paced", "1");
    set(v, *sim, "--rate", "1kHz");
    assert(tui::batches::compose_line(*sim, v) == "sim run --paced --rate 1kHz");
    auto w = blank(*sim);
    set(w, *sim, "--paced", "0");
    set(w, *sim, "--rate", "1kHz");
    // Disabled flag omitted but its value stays (CLI reports --rate needs --paced).
    assert(tui::batches::compose_line(*sim, w) == "sim run --rate 1kHz");
    auto e = blank(*sim);
    assert(tui::batches::compose_line(*sim, e) == "sim run");
  }
  {
    // Paths with spaces quote to one argv token.
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "table");
    set(v, *sig, "--sig-table", "/tmp/opencode/my wave.csv");
    const std::string line = tui::batches::compose_line(*sig, v);
    assert(line == "sig run --sig-kind table --sig-table '/tmp/opencode/my wave.csv'");
  }
  {
    // --forever composes but routes to async, never the sync executor.
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "sine");
    set(v, *sig, "--forever", "1");
    assert(tui::batches::wants_async(*sig, v));
    assert(tui::batches::compose_line(*sig, v) ==
           "sig run --sig-kind sine --forever");
    auto w = blank(*sig);
    set(w, *sig, "--sig-kind", "sine");
    set(w, *sig, "--forever", "0");
    assert(!tui::batches::wants_async(*sig, w));
    // The sim builder has no forever field: never async.
    auto s = blank(*sim);
    set(s, *sim, "--steps", "100");
    assert(!tui::batches::wants_async(*sim, s));
  }
}

void test_parity_matrix() {
  const auto defs = tui::batches::batch_defs();
  tui::TuiExecutor exec;
  exec.set_history_file("");
  cli::Shell shell;
  shell.set_history_file("");

  const auto *sim = find_batch(defs, "sim");
  const auto *sig = find_batch(defs, "sig");
  assert(sim && sig);

  // sim: projectile batch (2 steps, explicit params).
  {
    auto v = blank(*sim);
    set(v, *sim, "--solver", "projectile");
    set(v, *sim, "--steps", "2");
    set(v, *sim, "--particles", "1");
    check_parity(exec, shell, *sim, v);
    const auto r = exec.execute_line(tui::batches::compose_line(*sim, v));
    assert(r.rc == 0 && r.text.find("sim run: solver=projectile") != std::string::npos);
  }
  // sim: defaults (empty builder == `sim run`).
  {
    auto v = blank(*sim);
    check_parity(exec, shell, *sim, v);
  }
  // sim: signal solver batch (default 1 Hz sine, one source).
  {
    auto v = blank(*sim);
    set(v, *sim, "--solver", "signal");
    set(v, *sim, "--steps", "2");
    check_parity(exec, shell, *sim, v);
  }
  // sim: paced batch (fast wall rate so the test never sleeps long;
  // wall lateness varies run-to-run, so parity is rc + deterministic lines).
  {
    auto v = blank(*sim);
    set(v, *sim, "--steps", "2");
    set(v, *sim, "--paced", "1");
    set(v, *sim, "--rate", "1kHz");
    check_paced_parity(exec, shell, *sim, v, "rate=1000Hz", "steps: 2 x 0.01s");
  }
  // sim: emit-csv path selectable — file written beside the parity text.
  {
    const std::string path = tmp("wengine_test_batch_sim.csv");
    std::remove(path.c_str());
    auto v = blank(*sim);
    set(v, *sim, "--steps", "2");
    set(v, *sim, "--emit-csv", path);
    check_parity(exec, shell, *sim, v);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    assert(header.find("step,time") != std::string::npos);
    std::remove(path.c_str());
  }
  // sim signal: csv stride + select over the single serial-0 source.
  {
    const std::string path = tmp("wengine_test_batch_sim_stride.csv");
    std::remove(path.c_str());
    auto v = blank(*sim);
    set(v, *sim, "--solver", "signal");
    set(v, *sim, "--steps", "4");
    set(v, *sim, "--emit-csv", path);
    set(v, *sim, "--emit-csv-stride", "2");
    set(v, *sim, "--emit-csv-select", "0");
    check_parity(exec, shell, *sim, v);
    std::remove(path.c_str());
  }

  // sig: every batch kind builds without typing flags.
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "sine");
    set(v, *sig, "--sig-freq", "1kHz");
    set(v, *sig, "--sig-amp", "3.3V");
    set(v, *sig, "--steps", "2");
    set(v, *sig, "--dt", "0.1ms");
    check_parity(exec, shell, *sig, v);
  }
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "square");
    set(v, *sig, "--sig-freq", "10Hz");
    set(v, *sig, "--steps", "2");
    check_parity(exec, shell, *sig, v);
  }
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "noise");
    set(v, *sig, "--sig-seed", "7");
    set(v, *sig, "--steps", "2");
    check_parity(exec, shell, *sig, v);
  }
  {
    // sum is registry-listed but batch-rejected (per-source operands only):
    // the builder still composes it and the CLI owns the refusal.
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "sum");
    set(v, *sig, "--steps", "2");
    check_parity(exec, shell, *sig, v);
    const auto r = exec.execute_line(tui::batches::compose_line(*sig, v));
    assert(r.rc == 2);
  }
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "expression");
    set(v, *sig, "--sig-expr", "sin(2*pi*440*t)*0.5");
    set(v, *sig, "--steps", "2");
    check_parity(exec, shell, *sig, v);
  }
  {
    const std::string wave = tmp("wengine_test_batch_wave.csv");
    {
      std::ofstream out(wave);
      out << "time,value\n0,1.0\n1,3.0\n";
    }
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "table");
    set(v, *sig, "--sig-table", wave);
    set(v, *sig, "--steps", "5");
    set(v, *sig, "--dt", "100ms");
    check_parity(exec, shell, *sig, v);
    std::remove(wave.c_str());
  }
  {
    const std::string lua = tmp("wengine_test_batch_src.lua");
    {
      std::ofstream out(lua);
      out << "return 2*math.sin(2*math.pi*t)\n";
    }
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "lua");
    set(v, *sig, "--sig-lua", lua);
    set(v, *sig, "--steps", "2");
    check_parity(exec, shell, *sig, v);
    std::remove(lua.c_str());
  }
  // sig: paced batch (fast wall rate; wall lateness varies run-to-run).
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "dc");
    set(v, *sig, "--sig-offset", "1V");
    set(v, *sig, "--steps", "2");
    set(v, *sig, "--paced", "1");
    set(v, *sig, "--rate", "1kHz");
    check_paced_parity(exec, shell, *sig, v, "rate=1000Hz", "steps: 2 x 0.01s");
  }
  // sig: emit-csv path + stride + select parity, file lands on disk.
  {
    const std::string path = tmp("wengine_test_batch_sig.csv");
    std::remove(path.c_str());
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "dc");
    set(v, *sig, "--sig-offset", "1V");
    set(v, *sig, "--steps", "4");
    set(v, *sig, "--emit-csv", path);
    set(v, *sig, "--emit-csv-stride", "2");
    set(v, *sig, "--emit-csv-select", "0");
    check_parity(exec, shell, *sig, v);
    const auto r = exec.execute_line(tui::batches::compose_line(*sig, v));
    assert(r.rc == 0 && r.text.find("select=0") != std::string::npos);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    assert(header.find("sig_id") != std::string::npos);
    std::remove(path.c_str());
  }
  // sig: --forever composes the flag and routes async — never executed here
  // (executing it would block until Ctrl-C by design).
  {
    auto v = blank(*sig);
    set(v, *sig, "--sig-kind", "sine");
    set(v, *sig, "--forever", "1");
    assert(tui::batches::wants_async(*sig, v));
    assert(tui::batches::compose_line(*sig, v).find("--forever") != std::string::npos);
  }
}

void test_invalid_surfaces_cli_error() {
  const auto defs = tui::batches::batch_defs();
  tui::TuiExecutor exec;
  exec.set_history_file("");
  cli::Shell shell;
  shell.set_history_file("");

  auto expect_usage = [&](const tui::batches::BatchDef &def, std::vector<std::string> vals) {
    assert(!tui::batches::wants_async(def, vals));
    const std::string line = tui::batches::compose_line(def, vals);
    std::ostringstream expected;
    const int rc_shell = shell.execute(line, expected);
    const tui::ExecResult got = exec.execute_line(line);
    assert(got.rc == rc_shell);
    assert(got.text == expected.str());
    assert(got.rc == 2);
    assert(got.text.find("Error") != std::string::npos);
    // The CLI owns the refusal shape: usage bodies carry "Usage", while
    // registry refusals (unknown solver) list what is available instead.
    const bool has_shape = got.text.find("Usage") != std::string::npos ||
                           got.text.find(std::string(def.command)) != std::string::npos ||
                           got.text.find("Available") != std::string::npos;
    assert(has_shape);
  };

  const auto *sim = find_batch(defs, "sim");
  const auto *sig = find_batch(defs, "sig");
  assert(sim && sig);

  {
    auto v = blank(*sim); // unknown solver lists the registry
    set(v, *sim, "--solver", "nope-solver");
    set(v, *sim, "--steps", "2");
    expect_usage(*sim, v);
  }
  {
    auto v = blank(*sim); // bad step count reaches the CLI
    set(v, *sim, "--steps", "0");
    expect_usage(*sim, v);
  }
  {
    auto v = blank(*sim); // stride without its output is a usage error
    set(v, *sim, "--steps", "2");
    set(v, *sim, "--emit-csv-stride", "2");
    expect_usage(*sim, v);
  }
  {
    auto v = blank(*sig); // sum batch refusal via the CLI
    set(v, *sig, "--sig-kind", "sum");
    set(v, *sig, "--steps", "2");
    expect_usage(*sig, v);
  }
  {
    auto v = blank(*sig); // expression without its program
    set(v, *sig, "--sig-kind", "expression");
    set(v, *sig, "--steps", "2");
    expect_usage(*sig, v);
  }
  {
    auto v = blank(*sig); // cross-kind leftover
    set(v, *sig, "--sig-kind", "sine");
    set(v, *sig, "--sig-expr", "t");
    set(v, *sig, "--steps", "2");
    expect_usage(*sig, v);
  }
  {
    auto v = blank(*sig); // bad duty reaches the CLI validator
    set(v, *sig, "--sig-kind", "square");
    set(v, *sig, "--sig-duty", "2");
    set(v, *sig, "--steps", "2");
    expect_usage(*sig, v);
  }
  {
    auto v = blank(*sig); // --rate without --paced
    set(v, *sig, "--sig-kind", "dc");
    set(v, *sig, "--steps", "2");
    set(v, *sig, "--rate", "1kHz");
    expect_usage(*sig, v);
  }
}

} // namespace

int main() {
  test_defs_track_specs();
  test_keys_match_complete_vocab();
  test_registry_driven();
  test_compose_shapes();
  test_parity_matrix();
  test_invalid_surfaces_cli_error();
  std::cout << "test_tui_batches: OK\n";
  return 0;
}
