#include "wengine/core/run_config.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

// Tests for the CLI/GUI seam: frontends build core::RunConfig (plain data)
// and solvers::run_headless() drives the engine without frontend types.
// Goldens reuse the semi-implicit Euler closed form (see ProjectileSolver
// docs); tolerances carry ~100x margin over O(n^2 * eps_machine) drift.

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

core::RunConfig make_config() {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = 100;
  cfg.solver = "projectile";
  cfg.particles = 1;
  return cfg; // x0=y0=0, vx0=vy0=10, gravity=9.81 via struct defaults
}

struct RecSink : wengine::emit::ISink {
  std::vector<wengine::world::Snapshot> snaps;
  void on_snapshot(const wengine::world::Snapshot &s) override { snaps.push_back(s); }
};

void test_headless_golden() {
  // Defaults golden: v0 = (10, 10), g = 9.81, dt = 0.01, n = 100 ->
  // x = 10, y = 5.04595, vy = 0.19.
  wengine::world::State out;
  const core::Status st =
      wengine::solvers::run_headless(make_config(), std::span<wengine::emit::ISink *>{}, out);
  assert(st.ok());
  assert(out.step_index == 100);
  assert(near(out.sim_time, 1.0, 1e-12));
  assert(out.particles.live_count == 1);
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(out);
  assert(near(snap.pos_x[0], 10.0, 1e-9));
  assert(near(snap.pos_y[0], 5.04595, 1e-9));
  assert(snap.vel_x[0] == 10.0); // untouched axis stays exact
  assert(near(snap.vel_y[0], 0.19, 1e-9));
}

void test_gravity_flows_through_config() {
  // g = 1.62, n = 10: y = 1 - 1.62*0.01^2*10*11/2 = 0.99109,
  // vy = 10 - 10*1.62*0.01 = 9.838.
  core::RunConfig cfg = make_config();
  cfg.steps = 10;
  cfg.gravity = 1.62;
  cfg.has_gravity = true;
  wengine::world::State out;
  assert(wengine::solvers::run_headless(cfg, std::span<wengine::emit::ISink *>{}, out).ok());
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(out);
  assert(near(snap.pos_x[0], 1.0, 1e-9));
  assert(near(snap.pos_y[0], 0.99109, 1e-9));
  assert(near(snap.vel_y[0], 9.838, 1e-9));
}

void test_bad_config_rejected() {
  wengine::world::State out;
  const std::span<wengine::emit::ISink *> none{};
  {
    core::RunConfig cfg = make_config();
    cfg.solver = "nope";
    const core::Status st = wengine::solvers::run_headless(cfg, none, out);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
    assert(out.step_index == 0); // rejected before stepping
  }
  {
    core::RunConfig cfg = make_config();
    cfg.solver = "";
    assert(!wengine::solvers::run_headless(cfg, none, out).ok());
  }
  {
    core::RunConfig cfg = make_config();
    cfg.loop.dt = 0.0;
    const core::Status st = wengine::solvers::run_headless(cfg, none, out);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  }
  {
    core::RunConfig cfg = make_config();
    cfg.steps = 0;
    assert(!wengine::solvers::run_headless(cfg, none, out).ok());
  }
  {
    // Gravity with a non-projectile solver is rejected even when the
    // name itself is the (only) failure: unknown names fail first.
    core::RunConfig cfg = make_config();
    cfg.solver = "nope";
    cfg.has_gravity = true;
    const core::Status st = wengine::solvers::run_headless(cfg, none, out);
    assert(!st.ok() && st.code == core::StatusCode::kInvalidArgument);
  }
}

void test_emit_every_through_config() {
  core::RunConfig cfg = make_config();
  cfg.loop.emit_every = 10;
  RecSink sink;
  std::array<wengine::emit::ISink *, 1> sinks{&sink};
  wengine::world::State out;
  assert(wengine::solvers::run_headless(cfg, sinks, out).ok());
  assert(sink.snaps.size() == 10);
  for (std::size_t k = 0; k < sink.snaps.size(); ++k) {
    assert(sink.snaps[k].step_index == 10 * (k + 1)); // exact
  }
  // Caller-owned sinks observe snapshots without affecting state.
  assert(out.step_index == 100);
}

void test_chunked_matches_one_shot() {
  // Shared builder drives multiple run(N) chunks over one state: 37 + 63
  // must equal a one-shot headless batch bit-for-bit (PHYSI-35).
  core::RunConfig cfg = make_config();
  wengine::solvers::ConfiguredRun chunked;
  assert(wengine::solvers::ConfiguredRun::build(
             cfg, std::span<wengine::core::ISolver *>{}, false, chunked)
             .ok());
  assert(chunked.run(37).ok());
  assert(chunked.run(63).ok());
  wengine::world::State ref;
  assert(wengine::solvers::run_headless(cfg, std::span<wengine::emit::ISink *>{}, ref)
             .ok());
  assert(chunked.state() == ref);
  assert(wengine::world::take_snapshot(chunked.state()) ==
         wengine::world::take_snapshot(ref));

  // Same for a signal run (covers the signal spawn path, not just particles).
  core::RunConfig sig = make_config();
  sig.solver = "signal";
  sig.particles = 0;
  sig.signal.set = true;
  wengine::solvers::ConfiguredRun sig_chunked;
  assert(wengine::solvers::ConfiguredRun::build(
             sig, std::span<wengine::core::ISolver *>{}, false, sig_chunked)
             .ok());
  assert(sig_chunked.run(40).ok());
  assert(sig_chunked.run(60).ok());
  wengine::world::State sig_ref;
  assert(wengine::solvers::run_headless(sig, std::span<wengine::emit::ISink *>{}, sig_ref)
             .ok());
  assert(sig_chunked.state() == sig_ref);
}

struct DummySolver : wengine::core::ISolver {
  [[nodiscard]] std::string_view name() const noexcept override { return "dummy"; }
  wengine::core::Status step(wengine::world::State &, double) noexcept override {
    return wengine::core::Status::success();
  }
};

void test_dummy_extra_rejected_for_lua() {
  // Explicit Lua requirement (PHYSI-35): an arbitrary nonempty extra span
  // must not satisfy kLua — only a live "lua" solver plus the runtime flag.
  core::RunConfig cfg = make_config();
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.set = true;
  cfg.signal.params.kind = wengine::signal::SignalKind::kLua;
  cfg.signal.lua = "return 1";
  DummySolver dummy;
  std::array<wengine::core::ISolver *, 1> extras{&dummy};
  wengine::world::State out;
  assert(!wengine::solvers::run_headless(cfg, std::span<wengine::emit::ISink *>{}, out,
                                         std::span(extras.data(), extras.size()))
              .ok());
  wengine::solvers::ConfiguredRun run;
  assert(!wengine::solvers::ConfiguredRun::build(
              cfg, std::span(extras.data(), extras.size()), true, run)
              .ok());
  assert(!wengine::solvers::ConfiguredRun::build(
              cfg, std::span<wengine::core::ISolver *>{}, false, run)
              .ok());
}

} // namespace

int main() {
  test_headless_golden();
  test_gravity_flows_through_config();
  test_bad_config_rejected();
  test_emit_every_through_config();
  test_chunked_matches_one_shot();
  test_dummy_extra_rejected_for_lua();
  std::cout << "test_run_config: OK\n";
  return 0;
}
