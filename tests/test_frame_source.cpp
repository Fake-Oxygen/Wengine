// Frame source seam: solver registry, default channel, history ring
// behavior, bit-exactness against CsvSink output, and controller sink
// re-attachment through the recorder. Adapter exactness lives with the
// adapter itself (test_link_protocol covers link::frame_from_snapshot).

#include "controllers/sim_controller.hpp"
#include "views/frame_source.hpp"
#include "views/graphs_panel.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/signal.hpp"

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

void test_registry() {
  // One entry per solver adapter; update with each new domain.
  static_assert(std::tuple_size_v<decltype(wengine::gui::kFrameSources)> == 2);
  const wengine::world::Snapshot s = make_snap(3);
  const wengine::link::Frame via_reg =
      wengine::gui::adapt_frame(s, "projectile");
  const wengine::link::Frame direct = wengine::link::frame_from_snapshot(s);
  assert(via_reg == direct);
  // Unknown names fall back to the particle mapping; the signal domain
  // has its own entry (channels only, no items).
  const wengine::link::Frame fallback = wengine::gui::adapt_frame(s, "nope");
  assert(fallback.items.size() == 1 && fallback.items[0].x == 1.0);
  wengine::world::Snapshot sig;
  sig.step_index = 7;
  sig.sim_time = 0.07;
  sig.sig_serial.push_back(0);
  sig.sig_value.push_back(3.3);
  sig.sig_quantity.push_back(0);
  const wengine::link::Frame via_sig = wengine::gui::adapt_frame(sig, "signal");
  assert(via_sig.items.empty() && via_sig.channels.size() == 2);
  assert(via_sig.channels[0].name == "sig.count");
  assert(via_sig.channels[1].name == "sig.0.U" && via_sig.channels[1].value == 3.3);
}

void test_default_channel() {
  const wengine::link::Frame f =
      wengine::link::frame_from_snapshot(make_snap(1));
  assert(wengine::gui::default_channel(f) == "p0.y");
  wengine::link::Frame g;
  g.channels.push_back({"V(out)", 3.3});
  assert(wengine::gui::default_channel(g) == "V(out)");
  assert(wengine::gui::default_channel(wengine::link::Frame{}).empty());
  // Signal frames default to the first signal channel, never the
  // "sig.count" metadata row.
  wengine::link::Frame s;
  s.channels.push_back({"sig.count", 2.0});
  s.channels.push_back({"sig.0.U", 1.0});
  s.channels.push_back({"sig.1.U", 2.0});
  assert(wengine::gui::default_channel(s) == "sig.0.U");
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

wengine::link::Frame make_frame(std::uint64_t step) {
  return wengine::link::frame_from_snapshot(make_snap(step));
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
  assert(header == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  std::size_t row = 0;
  std::string line;
  while (std::getline(in, line)) {
    assert(row < history.size());
    const wengine::link::Frame &f = history.frames()[row];
    const char *p = line.c_str();
    char *end = nullptr;
    const auto step = std::strtoull(p, &end, 10);
    assert(end != p && *end == ',');
    // Parse all eight trailing fields in order:
    // time,slot,entity_index,entity_generation,x,y,vx,vy (slot/ids as doubles).
    double got[8] = {};
    for (int k = 0; k < 8; ++k) {
      assert(*end == ',');
      p = end + 1;
      got[k] = std::strtod(p, &end);
      assert(end != p);
    }
    assert(*end == '\0');
    assert(step == f.step);
    assert(got[0] == f.time);
    assert(got[1] == 0.0); // single particle: slot 0
    assert(got[2] == 0.0 && got[3] == 0.0); // identity: index 0, gen 0
    assert(!f.items.empty());
    assert(f.items[0].has_identity);
    assert(f.items[0].entity_index == 0 && f.items[0].entity_generation == 0);
    assert(got[4] == f.items[0].x);
    assert(got[5] == f.items[0].y);
    assert(got[6] == f.items[0].vx);
    assert(got[7] == f.items[0].vy);
    ++row;
  }
  assert(row == 5);
  std::remove(path.c_str());
}

void test_signals_csv_exactness() {
  // Signal-domain three-level parity: emitted snapshots through
  // SignalsCsvSink and the recorder agree bit-for-bit, and re-reading the
  // artifact adapts to the same frames (== on re-read, max_digits10).
  const std::string path = "/tmp/wengine_test_frames_sig.csv";
  std::remove(path.c_str());
  wengine::gui::FrameHistory history;
  wengine::gui::FrameRecorder recorder(history);
  recorder.set_solver("signal");
  {
    wengine::core::Runner runner{{.dt = 0.01, .emit_every = 2}};
    wengine::solvers::SignalSolver solver;
    runner.add_solver(solver);
    wengine::world::Entity e = runner.state().spawn_entity();
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kDc;
    p.offset = 2.5;
    assert(wengine::world::configure_source(e, runner.state().signals, p, 0.0).ok());
    wengine::emit::SignalsCsvSink csv(path);
    assert(csv.ok());
    runner.add_sink(csv);
    runner.add_sink(recorder);
    assert(runner.run(10).ok());
    assert(csv.flush().ok());
    assert(csv.rows_written() == 5); // steps 2,4,6,8,10
  }
  assert(history.size() == 5);

  std::ifstream in(path);
  std::vector<wengine::world::Snapshot> snaps;
  std::string error;
  assert(wengine::emit::parse_signals_log(in, snaps, error).ok());
  assert(snaps.size() == history.size());
  for (std::size_t k = 0; k < snaps.size(); ++k) {
    const wengine::link::Frame want =
        wengine::link::frame_from_signal_snapshot(snaps[k]);
    assert(want == history.frames()[k]);
  }
  assert(history.frames().back().channels.size() == 2);
  assert(history.frames().back().channels[1].name == "sig.0.U");
  assert(history.frames().back().channels[1].value == 2.5);
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
  test_registry();
  test_default_channel();
  test_fit_bounds();
  test_history_ring_and_reset();
  test_csv_exactness();
  test_signals_csv_exactness();
  test_controller_sink_reattach();
  std::printf("test_frame_source: OK\n");
  return 0;
}
