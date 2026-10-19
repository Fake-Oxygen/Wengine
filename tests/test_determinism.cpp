#include "wengine/core/runner.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/solvers/projectile.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Fixed-dt golden + determinism harness for the stepped path.
//
// Reference: semi-implicit Euler under constant g (n steps from v0):
//   v(n) = v0 - n*g*dt
//   x(n) = x0 + n*v0*dt - g*dt^2*n*(n+1)/2
// (see ProjectileSolver docs). Tolerances are justified, not widened to
// fit: O(n^2 * eps_machine * scale) ~ 1e-11 here, so 1e-9 carries ~100x
// margin. Exact == is used wherever no inexact arithmetic intervenes
// (step_index, counts, untouched vel_x, repeated-run equality, CSV
// max_digits10 round-trip).

namespace {

constexpr bool near(double a, double b, double eps) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void spawn(wengine::core::Runner &runner, double x, double y, double vx, double vy) {
  wengine::world::Entity e = runner.state().spawn_entity();
  runner.state().particles.attach(e.index);
  runner.state().particles.pos_x[e.index] = x;
  runner.state().particles.pos_y[e.index] = y;
  runner.state().particles.vel_x[e.index] = vx;
  runner.state().particles.vel_y[e.index] = vy;
}

wengine::core::Runner drive(double dt, std::uint64_t steps) {
  wengine::core::Runner runner{{.dt = dt}};
  wengine::solvers::ProjectileSolver solver;
  runner.add_solver(solver);
  spawn(runner, 0.0, 0.0, 10.0, 10.0);
  assert(runner.run(steps).ok());
  return runner;
}

// Defaults golden: v0 = (10, 10), g = 9.81, dt = 0.01, n = 100.
//   x = 100*10*0.01 = 10
//   vy = 10 - 100*9.81*0.01 = 0.19
//   y = 100*10*0.01 - 9.81*0.01^2*100*101/2 = 5.04595
void test_fixed_dt_golden() {
  const wengine::core::Runner runner = drive(0.01, 100);
  const wengine::world::Snapshot snap =
      wengine::world::take_snapshot(runner.state());
  assert(snap.step_index == 100);         // exact: integer clock
  assert(snap.particle_count() == 1);     // exact: population count
  assert(near(snap.sim_time, 1.0, 1e-12)); // 100 accumulations of 0.01
  assert(snap.vel_x[0] == 10.0);          // exact: solver never writes vel_x
  assert(near(snap.pos_x[0], 10.0, 1e-9));
  assert(near(snap.pos_y[0], 5.04595, 1e-9));
  assert(near(snap.vel_y[0], 0.19, 1e-9));
}

void test_repeat_exact() {
  // Same fixed-dt drive twice: full state and snapshot compare bit-identical.
  const wengine::core::Runner r1 = drive(0.01, 100);
  const wengine::core::Runner r2 = drive(0.01, 100);
  assert(r1.state() == r2.state());
  assert(wengine::world::take_snapshot(r1.state()) ==
         wengine::world::take_snapshot(r2.state()));
  assert(r1.state().sim_time == r2.state().sim_time); // exact FP equality
}

struct RecSink : wengine::emit::ISink {
  std::vector<wengine::world::Snapshot> snaps;
  void on_snapshot(const wengine::world::Snapshot &s) override { snaps.push_back(s); }
};

void test_snapshot_timing() {
  // Decimated emission is deterministic: exact step indices, near times.
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 5}};
  wengine::solvers::ProjectileSolver solver;
  RecSink sink;
  runner.add_solver(solver);
  runner.add_sink(sink);
  spawn(runner, 0.0, 0.0, 10.0, 10.0);
  assert(runner.run(20).ok());
  assert(sink.snaps.size() == 4);
  for (std::size_t k = 0; k < sink.snaps.size(); ++k) {
    assert(sink.snaps[k].step_index == 5 * (k + 1)); // exact
    assert(near(sink.snaps[k].sim_time, 0.01 * 5 * (k + 1), 1e-12));
  }
}

std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void test_csv_replay_exact() {
  // Runner -> CsvSink -> re-parse: max_digits10 round-trips exactly, so
  // parsed values compare with ==. A second identical run must be
  // byte-identical on disk (deterministic serialization formatting).
  const std::string p1 = "/tmp/wengine_test_determinism_a.csv";
  const std::string p2 = "/tmp/wengine_test_determinism_b.csv";
  std::remove(p1.c_str());
  std::remove(p2.c_str());

  auto run_to_csv = [](const std::string &path) {
    wengine::core::Runner runner{{.dt = 0.01, .emit_every = 10}};
    wengine::solvers::ProjectileSolver solver;
    RecSink mem;
    wengine::emit::CsvSink file(path);
    runner.add_solver(solver);
    runner.add_sink(mem);
    runner.add_sink(file);
    spawn(runner, 0.0, 0.0, 10.0, 10.0);
    assert(runner.run(100).ok());
    assert(file.ok() && mem.snaps.size() == 10);
    assert(file.flush().ok());
    return mem.snaps;
  };

  const std::vector<wengine::world::Snapshot> snaps = run_to_csv(p1);
  run_to_csv(p2);
  assert(read_file(p1) == read_file(p2)); // byte-identical rerun
  assert(!read_file(p1).empty());

  // Re-read: header + 10 rows of one particle each, values == memory.
  std::ifstream in(p1);
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  for (std::size_t k = 0; k < snaps.size(); ++k) {
    std::string line;
    assert(static_cast<bool>(std::getline(in, line)));
    std::istringstream row(line);
    std::string cell;
    std::vector<std::string> f;
    while (std::getline(row, cell, ',')) {
      f.push_back(cell);
    }
    assert(f.size() == 9);
    assert(std::stoull(f[0]) == snaps[k].step_index); // exact
    assert(std::stod(f[1]) == snaps[k].sim_time);     // exact round-trip
    assert(std::stoull(f[2]) == 0);
    assert(std::stoull(f[3]) == snaps[k].particle_index[0]);
    assert(std::stoull(f[4]) == snaps[k].particle_generation[0]);
    assert(std::stod(f[5]) == snaps[k].pos_x[0]);
    assert(std::stod(f[6]) == snaps[k].pos_y[0]);
    assert(std::stod(f[7]) == snaps[k].vel_x[0]);
    assert(std::stod(f[8]) == snaps[k].vel_y[0]);
  }
  std::string tail;
  assert(!static_cast<bool>(std::getline(in, tail))); // exactly 10 rows
  std::remove(p1.c_str());
  std::remove(p2.c_str());
}

void test_dt_sensitivity() {
  // Same physical time (T = 1 s) with halved dt follows its own closed
  // form: y = 200*10*0.005 - 9.81*0.005^2*200*201/2 = 5.070475.
  // The 0.024525 gap to the dt = 0.01 golden is integration error O(dt),
  // not a determinism violation: identical inputs still repeat exactly.
  const wengine::world::Snapshot fine =
      wengine::world::take_snapshot(drive(0.005, 200).state());
  const wengine::world::Snapshot coarse =
      wengine::world::take_snapshot(drive(0.01, 100).state());
  assert(fine.step_index == 200 && coarse.step_index == 100);
  assert(near(fine.pos_y[0], 5.070475, 1e-9));
  assert(near(fine.pos_y[0] - coarse.pos_y[0], 0.024525, 1e-9));
  assert(fine.pos_y[0] != coarse.pos_y[0]); // dt changes the answer
  assert(drive(0.005, 200).state() == drive(0.005, 200).state()); // yet repeats
}

} // namespace

int main() {
  test_fixed_dt_golden();
  test_repeat_exact();
  test_snapshot_timing();
  test_csv_replay_exact();
  test_dt_sensitivity();
  std::cout << "test_determinism: OK\n";
  return 0;
}
