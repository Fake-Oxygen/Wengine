// GUI signal panel: engine-only model validation/config building plus
// SimController bit-parity with the headless path for signal runs
// (batch builds and SIG-10 verb sessions through the controller bus).
// Rendering (panels/signal_panel.hpp) is ImGui-only and verified by
// windowed runs; everything testable without a display lives here.

#include "controllers/sim_controller.hpp"
#include "models/signal_model.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal_control.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_model_defaults() {
  // Empty fields mean defaults (CLI-missing-option semantics).
  gui::SignalModel m;
  m.kind.clear();
  m.freq.clear();
  m.amp.clear();
  m.duty.clear();
  m.seed.clear();
  m.quantity.clear();
  m.count.clear();
  m.dt.clear();
  m.steps.clear();
  m.emit_every.clear();
  const gui::SignalBuild b = m.build();
  assert(b.ok);
  assert(b.cfg.solver == "signal");
  assert(b.cfg.particles == 0 && b.cfg.signal.set);
  assert(b.cfg.signal.params.kind == signal::SignalKind::kSine);
  assert(b.cfg.signal.params.freq_hz == 1.0);
  assert(b.cfg.signal.count == 1);
}

void test_model_dc_golden() {
  gui::SignalModel m;
  m.kind = "dc";
  m.amp = "3.3V";
  m.quantity = "U";
  const gui::SignalBuild b = m.build();
  assert(b.ok);
  assert(b.cfg.signal.params.kind == signal::SignalKind::kDc);
  assert(b.cfg.signal.params.amp == 3.3);
  assert(b.cfg.signal.params.quantity == signal::QuantityId::kVoltage);
}

void test_model_rejections() {
  gui::SignalModel m;
  m.kind = "sawtooth";
  assert(!m.build().ok);
  m.kind = "sine";
  m.quantity = "W";
  assert(!m.build().ok);
  m.quantity = "U";
  m.freq = "0Hz";
  assert(!m.build().ok);
  m.freq = "1";
  m.duty = "2";
  m.kind = "square";
  assert(!m.build().ok);
  m.duty = "0.5";
  m.kind = "sine";
  m.steps = "0";
  assert(!m.build().ok);
  m.steps = "100";
  m.dt = "0s";
  assert(!m.build().ok);
  m.dt = "0.01";
  // Dimensional firewall, same as the CLI: current text as volts fails.
  m.amp = "2mA";
  assert(!m.build().ok);
  m.amp = "2mV";
  assert(m.build().ok);
  // Batch sums need per-source operands (SIG-6 family).
  m.kind = "sum";
  m.amp = "1";
  assert(!m.build().ok);
  // Tables need a CSV file the batch form has no picker for (SIG-9 panel).
  m.kind = "table";
  assert(!m.build().ok);
  // Lua needs the scripting runtime (wengine_core only here — SIG-9 panel).
  m.kind = "lua";
  assert(!m.build().ok);
  // Expressions work in batch (parsed data, no I/O); stray text is rejected.
  m.kind = "expression";
  m.expr.clear();
  assert(!m.build().ok);
  m.expr = "2*t+";
  assert(!m.build().ok);
  m.expr = "2*t+1";
  const gui::SignalBuild eb = m.build();
  assert(eb.ok);
  assert(eb.cfg.signal.params.kind == signal::SignalKind::kExpression);
  m.kind = "sine";
  assert(!m.build().ok); // expression text needs kind=expression
  m.expr.clear();
  assert(m.build().ok);
}

void test_controller_parity() {
  // Model-built config drives the controller bit-identically to headless.
  gui::SignalModel m;
  m.kind = "sine";
  m.freq = "50Hz";
  m.amp = "5V";
  m.dt = "0.001";
  m.steps = "200";
  const gui::SignalBuild b = m.build();
  assert(b.ok);

  gui::SimController sim;
  assert(sim.set_config(b.cfg).ok());
  assert(sim.run_exact(b.cfg.steps).ok());
  const world::Snapshot snap = sim.snapshot();
  assert(snap.step_index == 200 && snap.signal_count() == 1);
  // 50 Hz at t = 0.2 s: exactly 10 periods -> ~0.
  assert(near(snap.sig_value[0], 0.0, 1e-9));

  world::State ref;
  assert(solvers::run_headless(b.cfg, std::span<emit::ISink *>{}, ref).ok());
  assert(sim.state() == ref);
  assert(sim.snapshot() == world::take_snapshot(ref));
}

void test_controller_rejects_bad_signal() {
  gui::SimController sim;
  core::RunConfig bad = sim.config();
  bad.solver = "signal";
  bad.signal.set = true;
  bad.signal.params.freq_hz = 0.0;
  assert(!sim.set_config(bad).ok());
  assert(!sim.error().empty());
  // Rejected edits leave the live run untouched.
  assert(sim.state().step_index == 0);
  // Table points without a table kind (and vice versa) are rejected, never
  // silently dropped — same rule as the headless driver.
  bad = sim.config();
  bad.solver = "signal";
  bad.signal.set = true;
  bad.signal.params.kind = signal::SignalKind::kTable;
  assert(!sim.set_config(bad).ok());
  bad.signal.table = {{0.0, 1.0}, {1.0, 3.0}};
  bad.loop.dt = 0.1;
  assert(sim.set_config(bad).ok());
  assert(sim.run_exact(5).ok());
  assert(near(sim.snapshot().sig_value[0], 2.0, 1e-9));
  bad.signal.params.kind = signal::SignalKind::kSine;
  assert(!sim.set_config(bad).ok());
  // Lua configs need the scripting runtime the controller does not link.
  bad.signal.params.kind = signal::SignalKind::kLua;
  bad.signal.lua = "return 1";
  assert(!sim.set_config(bad).ok());
  assert(!sim.error().empty());
}

void test_model_table_lua() {
  // Table CSV text parses at the model edge (the panel loads the file).
  gui::SignalModel m;
  m.kind = "table";
  assert(!m.build().ok); // no CSV text yet
  m.table_csv = "time,value\n0,1\n1,3\n";
  const gui::SignalBuild tb = m.build();
  assert(tb.ok);
  assert(tb.cfg.signal.table.size() == 2);
  assert(tb.cfg.signal.table[0].t == 0.0 && tb.cfg.signal.table[1].v == 3.0);
  m.table_csv = "time,value\n0,1\n0,2\n"; // not increasing
  assert(!m.build().ok);
  m.table_csv = "step,time\n0,1\n";
  assert(!m.build().ok);
  m.table_csv = "time,value\n0,1\n1,3\n";
  m.kind = "sine";
  assert(!m.build().ok); // table CSV needs kind=table
  m.table_csv.clear();
  assert(m.build().ok);
  // Lua script text rides the batch section (syntax/runtime gates live
  // downstream: the shared builder rejects kLua without a runtime).
  m.kind = "lua";
  assert(!m.build().ok); // no script text yet
  m.lua_script = "return t * 2";
  const gui::SignalBuild lb = m.build();
  assert(lb.ok);
  assert(lb.cfg.signal.lua == "return t * 2");
  m.lua_script = std::string(signal::kMaxLuaScriptBytes + 1, 'x');
  assert(!m.build().ok);
  m.lua_script = "return 1";
  m.kind = "sine";
  assert(!m.build().ok); // lua script needs kind=lua
  m.lua_script.clear();
  assert(m.build().ok);
}

void test_controller_table_parity() {
  // Model-built table config drives the controller bit-identically.
  gui::SignalModel m;
  m.kind = "table";
  m.table_csv = "time,value\n0,0\n1,10\n";
  m.dt = "0.5";
  m.steps = "4";
  const gui::SignalBuild b = m.build();
  assert(b.ok);
  gui::SimController sim;
  assert(sim.set_config(b.cfg).ok());
  assert(sim.run_exact(b.cfg.steps).ok());
  world::State ref;
  assert(solvers::run_headless(b.cfg, std::span<emit::ISink *>{}, ref).ok());
  assert(sim.state() == ref);
  assert(near(sim.snapshot().sig_value[0], 10.0, 1e-9)); // clamped at t=2
}

void test_controller_verb_parity() {
  // Same verb session through the controller bus and a headless
  // ConfiguredRun + ControlBus reference: identical states, snapshots,
  // and replay logs. Verbs apply at exact steps on both sides.
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.steps = 100;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.params.kind = signal::SignalKind::kSine;
  cfg.signal.params.freq_hz = 1.0;
  cfg.signal.set = true;

  gui::SimController sim;
  assert(sim.set_config(cfg).ok());

  solvers::ConfiguredRun ref_run;
  assert(solvers::ConfiguredRun::build(cfg, std::span<core::ISolver *>{},
                                       false, ref_run, nullptr)
             .ok());
  solvers::ControlBus ref_bus{7};
  const std::uint32_t ref_ch = ref_bus.add_channel();
  auto ref_drain = [&] {
    return ref_bus.drain(ref_run.state(), ref_run.state().step_index);
  };

  // Timed session: (step to drain before, verb line). Serials are
  // deterministic: batch sine is 0, ADDs mint 1, 2, 3 in order.
  const std::vector<std::pair<std::uint64_t, std::string>> session = {
      {2, "ADD sine U FREQ 2 AMP 3"},
      {4, "SET 1 AMP 5"},
      {5, "STOP 1"},
      {7, "START 1"},
      {8, "ADD expression U EXPR 2*t+1"},
      {9, "ADD sum U OPS 0,1"},
      {10, "SET 0 FREQ 4"},
      {12, "DROP 2"},
  };
  std::size_t cursor = 0;
  for (std::uint64_t step = 0; step < 15; ++step) {
    while (cursor < session.size() && session[cursor].first == step) {
      assert(sim.send_command(session[cursor].second));
      assert(ref_bus.enqueue(ref_ch, session[cursor].second));
      ++cursor;
    }
    assert(sim.run_exact(1).ok());
    (void)ref_drain();
    assert(ref_run.run(1).ok());
    assert(sim.state() == ref_run.state());
  }
  assert(sim.snapshot() == world::take_snapshot(ref_run.state()));
  assert(sim.command_log().size() == ref_bus.log().size());
  assert(sim.command_log().size() == session.size());
  for (std::size_t k = 0; k < session.size(); ++k) {
    assert(sim.command_log()[k].line == ref_bus.log()[k].line);
    assert(sim.command_log()[k].step == ref_bus.log()[k].step);
  }
  // Verdicts: every session line applied (OK verdicts, none rejected).
  const std::vector<std::string> resps = sim.take_responses();
  assert(resps.size() == session.size());
  for (const std::string &r : resps) {
    assert(r.size() >= 2 && r[0] == 'O' && r[1] == 'K');
  }
  // Post-session census: serials 0 (sine), 1 (sine), 3 (sum) live;
  // serial 2 was dropped.
  assert(!world::find_serial(sim.state().signals, 2).has_value());
  assert(world::find_serial(sim.state().signals, 3).has_value());
}

void test_controller_verb_rejection() {
  gui::SimController sim;
  core::RunConfig cfg = sim.config();
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.set = true;
  assert(sim.set_config(cfg).ok());
  const world::State before = sim.state();
  assert(sim.send_command("DROP 999"));
  const solvers::DrainStats st = sim.drain_commands();
  assert(st.rejected == 1 && st.applied == 0);
  const std::vector<std::string> resps = sim.take_responses();
  assert(resps.size() == 1 && resps[0].substr(0, 3) == "ERR");
  assert(sim.state() == before); // refusal touches nothing
}

void test_controller_expression_parity() {
  // Expression configs drive the controller bit-identically to headless.
  core::RunConfig cfg;
  cfg.loop.dt = 0.1;
  cfg.steps = 2;
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.params.kind = signal::SignalKind::kExpression;
  cfg.signal.params.expr = signal::parse_expression("2*t+1").value;
  cfg.signal.set = true;
  gui::SimController sim;
  assert(sim.set_config(cfg).ok());
  assert(sim.run_exact(cfg.steps).ok());
  world::State ref;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, ref).ok());
  assert(sim.state() == ref);
  assert(near(sim.snapshot().sig_value[0], 1.4, 1e-9));
}

} // namespace

int main() {
  test_model_defaults();
  test_model_dc_golden();
  test_model_rejections();
  test_model_table_lua();
  test_controller_parity();
  test_controller_rejects_bad_signal();
  test_controller_table_parity();
  test_controller_verb_parity();
  test_controller_verb_rejection();
  test_controller_expression_parity();
  std::printf("test_signal_panel: OK\n");
  return 0;
}
