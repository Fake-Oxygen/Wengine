// GUI signal panel: engine-only model validation/config building plus
// SimController bit-parity with the headless path for signal runs.
// Rendering (panels/signal_panel.hpp) is ImGui-only and verified by
// windowed runs; everything testable without a display lives here.

#include "controllers/sim_controller.hpp"
#include "models/signal_model.hpp"
#include "wengine/solvers/run.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <span>

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
  test_controller_parity();
  test_controller_rejects_bad_signal();
  test_controller_expression_parity();
  std::printf("test_signal_panel: OK\n");
  return 0;
}
