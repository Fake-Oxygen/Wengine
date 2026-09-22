// Graphs history: ring bounds, reset clearing, exactness against CsvSink
// output for identical snapshot streams, and controller re-attachment.

#include "controllers/sim_controller.hpp"
#include "models/plot_history.hpp"
#include "views/graphs_panel.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/solvers/projectile.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_fit_bounds() {
  using wengine::gui::graphs_detail::fit_bounds;
  // 5% padding on each side.
  const wengine::gui::graphs_detail::Bounds b =
      fit_bounds({{{0.0F, 0.0F}, {10.0F, 20.0F}}});
  assert(near(b.x0, -0.5) && near(b.x1, 10.5));
  assert(near(b.y0, -1.0) && near(b.y1, 21.0));
  // Degenerate single point pads by +-1.
  const wengine::gui::graphs_detail::Bounds d =
      fit_bounds({{{5.0F, 5.0F}}});
  assert(near(d.x0, 4.0) && near(d.x1, 6.0));
  assert(near(d.y0, 4.0) && near(d.y1, 6.0));
  // Empty series: safe defaults, never NaN.
  const wengine::gui::graphs_detail::Bounds e = fit_bounds({});
  assert(e.x0 == 0.0 && e.x1 == 1.0 && e.y0 == 0.0 && e.y1 == 1.0);
}

wengine::world::Snapshot make_snap(std::uint64_t step) {
  wengine::world::Snapshot s;
  s.step_index = step;
  s.sim_time = static_cast<double>(step) * 0.01;
  s.pos_x.push_back(1.0);
  s.pos_y.push_back(2.0);
  s.vel_x.push_back(3.0);
  s.vel_y.push_back(4.0);
  return s;
}

void test_ring_bounds_and_reset() {
  wengine::gui::PlotHistory h;
  assert(h.empty());
  for (std::uint64_t k = 1; k <= wengine::gui::PlotHistory::kCapacity + 6; ++k) {
    h.on_snapshot(make_snap(k));
  }
  // Bounded drop-oldest: the first 6 samples fell off.
  assert(h.size() == wengine::gui::PlotHistory::kCapacity);
  assert(h.snapshots().front().step_index == 7);
  assert(h.snapshots().back().step_index ==
         wengine::gui::PlotHistory::kCapacity + 6);
  // A non-advancing step means a fresh run: stale series drops first.
  h.on_snapshot(make_snap(2));
  assert(h.size() == 1);
  assert(h.snapshots().front().step_index == 2);
  h.clear();
  assert(h.empty());
}

void test_csv_exactness() {
  // Same emitted snapshots through CsvSink and PlotHistory must agree
  // bit-for-bit (CSV uses max_digits10, which round-trips exactly).
  const std::string path = "/tmp/wengine_test_graphs.csv";
  std::remove(path.c_str());
  wengine::gui::PlotHistory history;
  {
    wengine::core::Runner runner{{.dt = 0.01, .emit_every = 2}};
    wengine::solvers::ProjectileSolver solver;
    runner.add_solver(solver);
    wengine::world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] = 0.0;
    runner.state().particles.pos_y[e.index] = 0.0;
    runner.state().particles.vel_x[e.index] = 10.0;
    runner.state().particles.vel_y[e.index] = 10.0;
    wengine::emit::CsvSink csv(path);
    assert(csv.ok());
    runner.add_sink(csv);
    runner.add_sink(history);
    assert(runner.run(10).ok());
    assert(csv.flush().ok());
    assert(csv.rows_written() == 5); // steps 2,4,6,8,10
  }
  assert(history.size() == 5);

  std::ifstream in(path);
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header == "step,time,slot,x,y,vx,vy");
  std::size_t row = 0;
  std::string line;
  while (std::getline(in, line)) {
    assert(row < history.size());
    const wengine::world::Snapshot &s = history.snapshots()[row];
    const char *p = line.c_str();
    char *end = nullptr;
    const auto step = std::strtoull(p, &end, 10);
    assert(end != p && *end == ',');
    // Parse all six trailing doubles in order: time,slot,x,y,vx,vy.
    double got[6] = {};
    for (int k = 0; k < 6; ++k) {
      assert(*end == ',');
      p = end + 1;
      got[k] = std::strtod(p, &end);
      assert(end != p);
    }
    assert(*end == '\0');
    assert(step == s.step_index);
    assert(got[0] == s.sim_time);
    assert(got[1] == 0.0); // single particle: slot 0
    assert(got[2] == s.pos_x[0]);
    assert(got[3] == s.pos_y[0]);
    assert(got[4] == s.vel_x[0]);
    assert(got[5] == s.vel_y[0]);
    ++row;
  }
  assert(row == 5);
  std::remove(path.c_str());
}

wengine::core::RunConfig batch_cfg() {
  wengine::core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 2;
  cfg.steps = 100;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  return cfg;
}

void test_controller_sink_reattach() {
  wengine::gui::SimController sim;
  assert(sim.set_config(batch_cfg()).ok());
  wengine::gui::PlotHistory history;
  sim.add_sink(history);
  assert(sim.run_exact(5).ok()); // emits at steps 2, 4
  assert(history.size() == 2);
  // Reset rebuilds the Runner; observers must stay attached and the stale
  // series must drop on the fresh run.
  sim.reset();
  assert(sim.run_exact(4).ok());
  assert(history.size() == 2);
  assert(history.snapshots().front().step_index == 2);
  assert(history.snapshots().back().step_index == 4);
}

} // namespace

int main() {
  test_fit_bounds();
  test_ring_bounds_and_reset();
  test_csv_exactness();
  test_controller_sink_reattach();
  std::printf("test_graphs: OK\n");
  return 0;
}
