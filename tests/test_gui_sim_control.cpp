// GUI stepped-sim control: SimController parity with the headless path.
// Same RunConfig must give bit-identical snapshots whether driven by
// solvers::run_headless() (CLI) or by the controller (GUI panel).

#include "sim_controller.hpp"
#include "wengine/solvers/run.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <span>

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// Independently derived goldens for the defaults (v0 = (10, 10), g = 9.81,
// dt = 0.01, n = 100, semi-implicit Euler; same derivation as test_sim):
//   x = 100*10*0.01 = 10
//   vy = 10 - 100*9.81*0.01 = 0.19
//   y = 100*10*0.01 - 9.81*0.01^2*100*101/2 = 5.04595
constexpr double kGoldenX = 10.0;
constexpr double kGoldenY = 5.04595;
constexpr double kGoldenVy = 0.19;

wengine::core::RunConfig default_cfg() {
  wengine::core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = 100;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.has_gravity = false;
  cfg.particles = 1;
  cfg.x0 = 0.0;
  cfg.y0 = 0.0;
  cfg.vx0 = 10.0;
  cfg.vy0 = 10.0;
  return cfg;
}

void test_headless_parity_and_goldens() {
  const wengine::core::RunConfig cfg = default_cfg();
  wengine::gui::SimController sim;
  assert(sim.set_config(cfg).ok());
  assert(sim.run_exact(cfg.steps).ok());

  // Bit-identical to the CLI headless path for the same config.
  wengine::world::State ref;
  assert(wengine::solvers::run_headless(cfg, std::span<wengine::emit::ISink *>{},
                                        ref)
             .ok());
  assert(sim.snapshot() == wengine::world::take_snapshot(ref));

  // And matching the independently derived closed form.
  const wengine::world::Snapshot snap = sim.snapshot();
  assert(snap.step_index == 100 && near(snap.sim_time, 1.0));
  assert(snap.particle_count() == 1);
  assert(near(snap.pos_x[0], kGoldenX, 1e-6));
  assert(near(snap.pos_y[0], kGoldenY, 1e-6));
  assert(near(snap.vel_x[0], 10.0));
  assert(near(snap.vel_y[0], kGoldenVy, 1e-6));
}

void test_step_reset_determinism() {
  wengine::gui::SimController stepped;
  assert(stepped.set_config(default_cfg()).ok());
  for (int i = 0; i < 37; ++i) {
    assert(stepped.run_exact(1).ok());
  }
  wengine::gui::SimController batched;
  assert(batched.set_config(default_cfg()).ok());
  assert(batched.run_exact(37).ok());
  assert(stepped.state() == batched.state());

  // Reset returns to the initial population; re-running reproduces.
  stepped.reset();
  assert(stepped.state().step_index == 0 && stepped.state().sim_time == 0.0);
  assert(stepped.run_exact(37).ok());
  assert(stepped.state() == batched.state());
}

void test_advance_modes() {
  // Substep cap: 0.079s of wall time at dt=0.01 folds into exactly 7 fixed
  // steps (0.001 below the 8-step cap, far from any FP boundary), and two
  // identical drives agree bit-for-bit.
  wengine::gui::SimController sim;
  assert(sim.set_config(default_cfg()).ok());
  assert(sim.advance_frame(0.079).ok());
  assert(sim.state().step_index == 7);
  wengine::gui::SimController twin;
  assert(twin.set_config(default_cfg()).ok());
  assert(twin.advance_frame(0.079).ok());
  assert(sim.state() == twin.state());

  // Small wall chunks fold into exact fixed steps: 7 + 5 == run_exact(12).
  // (0.05 leaves ~1e-17 residue per call, nowhere near another dt.)
  assert(sim.advance_frame(0.05).ok());
  wengine::gui::SimController ref;
  assert(ref.set_config(default_cfg()).ok());
  assert(ref.run_exact(12).ok());
  assert(sim.state() == ref.state());

  // Step budget auto-pauses play: exact batch, then parked at the budget.
  wengine::gui::SimController play;
  assert(play.set_config(default_cfg()).ok());
  play.set_playing(true);
  for (int i = 0; i < 25 && play.playing(); ++i) {
    assert(play.advance_frame(0.05).ok());
  }
  assert(!play.playing());
  assert(play.state().step_index == 100);
  wengine::gui::SimController golden;
  assert(golden.set_config(default_cfg()).ok());
  assert(golden.run_exact(100).ok());
  assert(play.state() == golden.state());
}

void test_config_validation() {
  wengine::gui::SimController sim;
  assert(sim.set_config(default_cfg()).ok());

  wengine::core::RunConfig bad_dt = default_cfg();
  bad_dt.loop.dt = 0.0;
  assert(!sim.set_config(bad_dt).ok());
  assert(!sim.error().empty());

  wengine::core::RunConfig bad_steps = default_cfg();
  bad_steps.steps = 0;
  assert(!sim.set_config(bad_steps).ok());

  wengine::core::RunConfig bad_solver = default_cfg();
  bad_solver.solver = "nope";
  assert(!sim.set_config(bad_solver).ok());
  assert(sim.error().find("nope") != std::string::npos);

  // Rejected edits leave the live run untouched.
  assert(sim.state().step_index == 0);
  assert(sim.run_exact(100).ok());
  assert(near(sim.snapshot().pos_y[0], kGoldenY, 1e-6));
}

void test_gravity_config() {
  // Solver-scoped option through the GUI seam: g=1.62 over 100 steps.
  // vy = 10 - 100*1.62*0.01 = 8.38
  // y = 10 - 1.62*0.01^2*100*101/2 = 9.1819
  wengine::core::RunConfig cfg = default_cfg();
  cfg.gravity = 1.62;
  cfg.has_gravity = true;
  wengine::gui::SimController sim;
  assert(sim.set_config(cfg).ok());
  assert(sim.run_exact(100).ok());
  const wengine::world::Snapshot snap = sim.snapshot();
  assert(near(snap.vel_y[0], 8.38, 1e-9));
  assert(near(snap.pos_y[0], 9.1819, 1e-6));
  assert(near(snap.pos_x[0], 10.0, 1e-9));
}

} // namespace

int main() {
  test_headless_parity_and_goldens();
  test_step_reset_determinism();
  test_advance_modes();
  test_config_validation();
  test_gravity_config();
  std::printf("test_gui_sim_control: OK\n");
  return 0;
}
