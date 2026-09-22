// Frame source seam: particle adapter exactness, solver registry,
// default channel, history ring behavior, bit-exactness against CsvSink
// output, and controller sink re-attachment through the recorder.

#include "controllers/sim_controller.hpp"
#include "views/frame_source.hpp"
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
#include <tuple>

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
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

void test_adapter_exactness() {
  const wengine::gui::DataFrame f =
      wengine::gui::frame_from_particles(make_snap(7));
  assert(f.step == 7 && near(f.time, 0.07));
  assert(f.items.size() == 1);
  assert(f.items[0].x == 1.0 && f.items[0].y == 2.0 && f.items[0].z == 0.0);
  assert(f.items[0].vx == 3.0 && f.items[0].vy == 4.0);
  assert(f.items[0].has_velocity);
  // Primary-item channels + count, stable order.
  assert(f.channels.size() == 6);
  assert(f.channels[0].name == "count" && f.channels[0].value == 1.0);
  assert(f.channels[1].name == "p0.x" && f.channels[1].value == 1.0);
  assert(f.channels[2].name == "p0.y" && f.channels[2].value == 2.0);
  assert(f.channels[5].name == "p0.speed" && near(f.channels[5].value, 5.0));
  // Empty snapshot: count channel only, no items.
  const wengine::gui::DataFrame e =
      wengine::gui::frame_from_particles(wengine::world::Snapshot{});
  assert(e.items.empty() && e.channels.size() == 1);
}

void test_registry() {
  // One entry today; update with each new solver adapter.
  static_assert(std::tuple_size_v<decltype(wengine::gui::kFrameSources)> == 1);
  const wengine::world::Snapshot s = make_snap(3);
  const wengine::gui::DataFrame via_reg =
      wengine::gui::adapt_frame(s, "projectile");
  const wengine::gui::DataFrame direct = wengine::gui::frame_from_particles(s);
  assert(via_reg.step == direct.step && via_reg.items.size() == 1);
  // Unknown names fall back to the particle mapping (snapshot is
  // particle-shaped); a future domain entry replaces this path.
  const wengine::gui::DataFrame fallback = wengine::gui::adapt_frame(s, "nope");
  assert(fallback.items.size() == 1 && fallback.items[0].x == 1.0);
}

void test_default_channel() {
  const wengine::gui::DataFrame f =
      wengine::gui::frame_from_particles(make_snap(1));
  assert(wengine::gui::default_channel(f) == "p0.y");
  wengine::gui::DataFrame g;
  g.channels.push_back({"V(out)", 3.3});
  assert(wengine::gui::default_channel(g) == "V(out)");
  assert(wengine::gui::default_channel(wengine::gui::DataFrame{}).empty());
}

void test_fit_bounds() {
  using wengine::gui::graphs_detail::fit_bounds;
  // 5% padding on each side.
  const wengine::gui::graphs_detail::Bounds b =
      fit_bounds({{{0.0F, 0.0F}, {10.0F, 20.0F}}});
  assert(near(b.x0, -0.5) && near(b.x1, 10.5));
  assert(near(b.y0, -1.0) && near(b.y1, 21.0));
  // Degenerate single point pads by +-1.
  const wengine::gui::graphs_detail::Bounds d = fit_bounds({{{5.0F, 5.0F}}});
  assert(near(d.x0, 4.0) && near(d.x1, 6.0));
  assert(near(d.y0, 4.0) && near(d.y1, 6.0));
  // Empty series: safe defaults, never NaN.
  const wengine::gui::graphs_detail::Bounds e = fit_bounds({});
  assert(e.x0 == 0.0 && e.x1 == 1.0 && e.y0 == 0.0 && e.y1 == 1.0);
}

wengine::gui::DataFrame make_frame(std::uint64_t step) {
  return wengine::gui::frame_from_particles(make_snap(step));
}

void test_history_ring_and_reset() {
  wengine::gui::FrameHistory h;
  assert(h.empty());
  for (std::uint64_t k = 1; k <= wengine::gui::FrameHistory::kCapacity + 6; ++k) {
    h.push(make_frame(k));
  }
  // Bounded drop-oldest: the first 6 frames fell off.
  assert(h.size() == wengine::gui::FrameHistory::kCapacity);
  assert(h.frames().front().step == 7);
  assert(h.frames().back().step == wengine::gui::FrameHistory::kCapacity + 6);
  // A non-advancing step means a fresh run: stale series drops first.
  h.push(make_frame(2));
  assert(h.size() == 1);
  assert(h.frames().front().step == 2);
  h.clear();
  assert(h.empty());
}

void test_csv_exactness() {
  // Same emitted snapshots through CsvSink and the recorder must agree
  // bit-for-bit (CSV uses max_digits10, which round-trips exactly).
  const std::string path = "/tmp/wengine_test_frames.csv";
  std::remove(path.c_str());
  wengine::gui::FrameHistory history;
  wengine::gui::FrameRecorder recorder(history);
  recorder.set_solver("projectile");
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
    runner.add_sink(recorder);
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
    const wengine::gui::DataFrame &f = history.frames()[row];
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
    assert(step == f.step);
    assert(got[0] == f.time);
    assert(got[1] == 0.0); // single particle: slot 0
    assert(!f.items.empty());
    assert(got[2] == f.items[0].x);
    assert(got[3] == f.items[0].y);
    assert(got[4] == f.items[0].vx);
    assert(got[5] == f.items[0].vy);
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
  wengine::gui::FrameHistory history;
  wengine::gui::FrameRecorder recorder(history);
  recorder.set_solver("projectile");
  sim.add_sink(recorder);
  assert(sim.run_exact(5).ok()); // emits at steps 2, 4
  assert(history.size() == 2);
  // Reset rebuilds the Runner; observers must stay attached and the stale
  // series must drop on the fresh run.
  sim.reset();
  assert(sim.run_exact(4).ok());
  assert(history.size() == 2);
  assert(history.frames().front().step == 2);
  assert(history.frames().back().step == 4);
}

} // namespace

int main() {
  test_adapter_exactness();
  test_registry();
  test_default_channel();
  test_fit_bounds();
  test_history_ring_and_reset();
  test_csv_exactness();
  test_controller_sink_reattach();
  std::printf("test_frame_source: OK\n");
  return 0;
}
