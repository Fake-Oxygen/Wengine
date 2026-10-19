#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/solvers/projectile.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace wengine::solvers;
using wengine::cli::ArgCursor;
using wengine::cli::kMaxArgs;
using wengine::cli::tokenize_into;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// Golden closed form for the defaults (v0 = (10, 10), g = 9.81,
// dt = 0.01, n = 100, semi-implicit Euler; vx is constant, only y bends):
//   x = 100*10*0.01 = 10
//   vy = 10 - 100*9.81*0.01 = 0.19
//   y = 100*10*0.01 - 9.81*0.01^2*100*101/2 = 5.04595
constexpr double kGoldenX = 10.0;
constexpr double kGoldenY = 5.04595;
constexpr double kGoldenVy = 0.19;

void spawn(wengine::core::Runner &runner, double x, double y, double vx, double vy) {
  wengine::world::Entity e = runner.state().spawn_entity();
  runner.state().particles.attach(e.index);
  runner.state().particles.pos_x[e.index] = x;
  runner.state().particles.pos_y[e.index] = y;
  runner.state().particles.vel_x[e.index] = vx;
  runner.state().particles.vel_y[e.index] = vy;
}

void test_solver_golden() {
  wengine::core::Runner runner{{.dt = 0.01}};
  ProjectileSolver solver;
  assert(solver.name() == "projectile" && solver.gravity() == 9.81);
  runner.add_solver(solver);
  spawn(runner, 0.0, 0.0, 10.0, 10.0);
  assert(runner.run(100).ok());

  const wengine::world::Snapshot snap = wengine::world::take_snapshot(runner.state());
  assert(snap.step_index == 100 && near(snap.sim_time, 1.0));
  assert(snap.particle_count() == 1);
  assert(near(snap.pos_x[0], kGoldenX, 1e-6));
  assert(near(snap.pos_y[0], kGoldenY, 1e-6));
  assert(near(snap.vel_x[0], 10.0));
  assert(near(snap.vel_y[0], kGoldenVy, 1e-6));
}

void test_holes_skipped_and_determinism() {
  auto drive = [] {
    wengine::core::Runner r{{.dt = 0.01}};
    ProjectileSolver s(9.81);
    r.add_solver(s);
    spawn(r, 0.0, 0.0, 10.0, 10.0);
    spawn(r, 1.0, 2.0, 0.0, 0.0);
    // Detach the second particle: solver must skip the hole, values frozen.
    r.state().particles.detach(1);
    assert(r.run(10).ok());
    return r;
  };
  const wengine::core::Runner r1 = drive();
  const wengine::core::Runner r2 = drive();
  assert(r1.state() == r2.state());
  assert(!r1.state().particles.has(1));
  assert(r1.state().particles.pos_x[1] == 1.0); // untouched hole
  assert(near(r1.state().particles.pos_x[0], 10.0 * 0.1)); // vx const: x = v*t
}

void test_bad_dt() {
  wengine::core::Runner runner{{.dt = 0.01}};
  ProjectileSolver solver;
  wengine::world::State s;
  assert(!solver.step(s, 0.0).ok());
  assert(!solver.step(s, -0.01).ok());
  // Non-finite dt and gravity reject before mutating the pool.
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  assert(!solver.step(s, inf).ok());
  assert(!solver.step(s, nan).ok());
  assert(s.particles.live_count == 0 && s.sim_time == 0.0);
  ProjectileSolver inf_g(inf);
  ProjectileSolver nan_g(nan);
  assert(!inf_g.step(s, 0.01).ok());
  assert(!nan_g.step(s, 0.01).ok());
  assert(s.particles.live_count == 0 && s.sim_time == 0.0);
}

std::pair<int, std::string> run_sim(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::SimSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli_golden() {
  // CLI output carries the same golden numbers as the solver test.
  const auto [rc, s] = run_sim("run --steps 100 --particles 1");
  assert(rc == 0);
  assert(s.find("solver=projectile") != std::string::npos);
  assert(s.find("steps: 100") != std::string::npos);
  assert(s.find("particles: 1") != std::string::npos);
  assert(s.find("x=10") != std::string::npos);
  assert(s.find("y=5.04595") != std::string::npos);
  assert(s.find("vy=0.19") != std::string::npos);
}

void test_cli_csv() {
  const std::string path = "/tmp/wengine_test_sim.csv";
  std::remove(path.c_str());
  const auto [rc, s] =
      run_sim("run --steps 10 --dt 10ms --particles 2 --emit-csv " + path + " --emit-every 5");
  assert(rc == 0);
  assert(s.find("csv: " + path + " (4 rows)") != std::string::npos);

  std::ifstream in(path);
  std::string header;
  std::string tail;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  // Steps 5 and 10, two particles each, slots 0 and 1 in order.
  for (int step : {5, 10}) {
    for (int slot = 0; slot < 2; ++slot) {
      std::string line;
      assert(static_cast<bool>(std::getline(in, line)));
      std::ostringstream want;
      want << step;
      assert(line.rfind(want.str() + ",", 0) == 0);
      assert(line.find("," + std::to_string(slot) + ",") != std::string::npos);
    }
  }
  assert(!static_cast<bool>(std::getline(in, tail))); // exactly 4 rows
  std::remove(path.c_str());
}

void test_cli_solver_selection() {
  // Explicit selection of the registered solver (space and = forms).
  {
    const auto [rc, s] = run_sim("run --solver projectile --steps 5 --particles 1");
    assert(rc == 0 && s.find("solver=projectile") != std::string::npos);
  }
  {
    const auto [rc, s] = run_sim("run --solver=projectile --steps 5");
    assert(rc == 0);
  }
  // Solver-scoped option still applies through the registry path.
  {
    const auto [rc, s] = run_sim("run --solver projectile --steps 5 --gravity 1.62");
    assert(rc == 0 && s.find("g=1.62") != std::string::npos);
  }
  // Unknown names fail with the live registry contents listed.
  {
    const auto [rc, s] = run_sim("run --solver nope --steps 5");
    assert(rc == 2 && s.find("unknown solver 'nope'") != std::string::npos &&
           s.find("projectile") != std::string::npos);
  }
}

void test_cli_errors() {
  assert(run_sim("run --steps 100 --particles 1").first == 0);
  assert(run_sim("run").first == 0); // all defaults
  assert(run_sim("run --steps 5 --particles 0").first == 0);
  assert(run_sim("--steps 10").first == 2); // missing mode
  assert(run_sim("run --steps 0").first == 2);
  assert(run_sim("run --steps -5").first == 2);
  assert(run_sim("run --dt 0s").first == 2);
  assert(run_sim("run --particles -1").first == 2);
  assert(run_sim("run --bogus 1").first == 2);
  assert(run_sim("run extra").first == 2);
  // Non-finite bare-double options (from_chars accepts inf/nan text)
  // reject at the CLI edge with usage errors, never silent inf output.
  {
    const auto [rc, s] = run_sim("run --steps 2 --gravity inf");
    assert(rc == 2 && s.find("gravity must be finite") != std::string::npos);
  }
  {
    const auto [rc, s] = run_sim("run --steps 2 --gravity nan");
    assert(rc == 2 && s.find("gravity must be finite") != std::string::npos);
  }
  {
    const auto [rc, s] = run_sim("run --steps 2 --x0 inf");
    assert(rc == 2 && s.find("initial conditions must be finite") != std::string::npos);
  }
  {
    const auto [rc, s] = run_sim("run --steps 2 --vy0 -inf");
    assert(rc == 2 && s.find("initial conditions must be finite") != std::string::npos);
  }
  assert(run_sim("run --steps 2 --dt inf").first == 2); // scaled parse tier
  // Zero particles: unused initial conditions stay accepted.
  assert(run_sim("run --steps 2 --particles 0 --x0 inf").first == 0);
  {
    const auto [rc, s] = run_sim("--help");
    assert(rc == 0 && s.find("sim run") != std::string::npos);
  }
}

void test_cli_shm() {
  // Flag wiring end-to-end: the publisher count in the output line proves
  // every emitted snapshot reached the bus (bus-content exactness itself
  // is covered in test_link_shm through the same run_headless path; the
  // segment is unlinked when the command's publisher dies, so there is
  // nothing to read back here by design).
  const std::string seg =
      "/wengine-test-sim-" + std::to_string(::getpid());
  const auto [rc, s] =
      run_sim("run --steps 10 --particles 1 --emit-shm " + seg);
  assert(rc == 0);
  // Default output stays concise; the per-cause breakdown needs --emit-health.
  assert(s.find("shm: " + seg + " (10 frames)") != std::string::npos);
  assert(s.find("attempted=") == std::string::npos);
  const auto [rc_h, s_h] =
      run_sim("run --steps 10 --particles 1 --emit-shm " + seg + " --emit-health");
  assert(rc_h == 0);
  assert(s_h.find("shm: " + seg + " (10 frames, attempted=10") != std::string::npos);
  assert(s_h.find("published_local=10") != std::string::npos);
  assert(s_h.find("over_bound=0") != std::string::npos);
  assert(s_h.find("backpressure=0") != std::string::npos);
  assert(s_h.find("pending_blocked=0") != std::string::npos);
  assert(s_h.find("disabled=0") != std::string::npos);
  assert(s_h.find("local acceptance only") != std::string::npos);
  // CSV + SHM fan out together through one run.
  const auto [rc2, s2] = run_sim("run --steps 4 --emit-every 2 --emit-shm " + seg +
                                 " --emit-csv /tmp/wengine_test_sim_shm.csv");
  assert(rc2 == 0);
  assert(s2.find("shm: " + seg + " (2 frames)") != std::string::npos);
  assert(s2.find("csv: /tmp/wengine_test_sim_shm.csv (2 rows)") != std::string::npos);
  std::remove("/tmp/wengine_test_sim_shm.csv");
  // Bad segment name is a usage error, never a crash.
  const auto [rc3, s3] = run_sim("run --steps 5 --emit-shm no-leading-slash");
  assert(rc3 == 2 && s3.find("invalid SHM segment name") != std::string::npos);
  assert(run_sim("run --steps 5 --emit-tcp malformed-endpoint").first == 2);
  assert(run_sim("run --steps 5 --emit-udp host:0").first == 2);
}

std::pair<int, std::string> run_sig(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::SigSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli_signal() {
  // DC golden: exact value in CLI text (three-level: solver -> CLI -> CSV).
  {
    const auto [rc, s] = run_sig("run --sig-kind dc --sig-offset 3.3V");
    assert(rc == 0);
    assert(s.find("sig run: kind=dc") != std::string::npos);
    assert(s.find("signals: 1") != std::string::npos);
    assert(s.find("s0: dc value=3.3 U") != std::string::npos);
  }
  // Sine smoke: 1 Hz at default dt, plus current stamping.
  {
    const auto [rc, s] =
        run_sig("run --sig-kind sine --sig-freq 1Hz --sig-amp 2V --steps 10 --dt 10ms");
    assert(rc == 0 && s.find("s0: sine value=") != std::string::npos &&
           s.find(" U") != std::string::npos);
  }
  {
    const auto [rc, s] = run_sig("run --sig-kind dc --sig-offset 50mA --sig-quantity I "
                                 "--steps 2");
    assert(rc == 0 && s.find("s0: dc value=0.05 I") != std::string::npos);
  }
  // Square duty + multi-source batch.
  {
    const auto [rc, s] = run_sig("run --sig-kind square --sig-freq 50Hz --sig-amp 5V "
                                 "--sig-duty 0.25 --sig-count 2 --steps 3 --dt 1ms");
    assert(rc == 0 && s.find("signals: 2") != std::string::npos &&
           s.find("s1: square value=5 U") != std::string::npos);
  }
  // Signals CSV log carries the full stream for replay (D2b-A).
  {
    const std::string path = "/tmp/wengine_test_sim_sig.csv";
    std::remove(path.c_str());
    const auto [rc, s] =
        run_sig("run --sig-kind dc --sig-offset 1V --steps 4 --emit-csv " + path);
    assert(rc == 0);
    assert(s.find("csv: " + path + " (4 rows)") != std::string::npos);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    assert(header == "step,time,sig_id,value,quantity");
    std::string row;
    assert(static_cast<bool>(std::getline(in, row)));
    assert(row == "1,0.01,0,1,U");
    std::remove(path.c_str());
  }
  // sim run stays vocabulary-free: signal opts there are usage errors, and
  // the bare signal solver runs one default sine.
  assert(run_sim("run --solver projectile --sig-kind sine").first == 2);
  assert(run_sim("run --solver signal --gravity 9.81").first == 2);
  assert(run_sig("run --emit-tcp malformed-endpoint").first == 2);
  assert(run_sig("run --emit-udp :9000").first == 2);
  {
    const auto [rc, s] = run_sim("run --solver signal --steps 2");
    assert(rc == 0 && s.find("solver=signal") != std::string::npos &&
           s.find("signals: 1") != std::string::npos &&
           s.find("s0: sine value=") != std::string::npos);
  }
  // sig usage errors: missing mode, bad vocab, bad ranges, stray positionals.
  assert(run_sig("--sig-kind dc").first == 2);
  assert(run_sig("run extra").first == 2);
  assert(run_sig("run --sig-kind sawtooth").first == 2);
  assert(run_sig("run --sig-quantity W").first == 2);
  assert(run_sig("run --sig-freq 0Hz").first == 2);
  assert(run_sig("run --sig-duty 2 --sig-kind square").first == 2);
  assert(run_sig("run --sig-seed -1").first == 2);
  assert(run_sig("run --sig-kind sum --steps 2").first == 2); // batch rejects sums
  assert(run_sig("run --sig-kind expression --steps 2").first == 2); // needs --sig-expr
  assert(run_sig("run --sig-kind table --steps 2").first == 2); // needs --sig-table
  assert(run_sig("run --sig-kind sine --sig-expr 't'").first == 2); // cross-kind
  assert(run_sig("run --sig-kind sine --sig-table x.csv").first == 2); // cross-kind
  assert(run_sig("run --sig-kind expression --sig-expr '2+'").first == 2); // bad math
  assert(run_sig("run --sig-kind expression --sig-expr 't' --sig-table x.csv").first ==
         2);
  assert(run_sig("run --steps 0").first == 2);
  // Expression golden through the CLI: 2*t+1, dt 100ms, 2 steps -> t = 0.2.
  {
    const auto [rc, s] = run_sig("run --sig-kind expression --sig-expr '2*t+1' "
                                 "--steps 2 --dt 100ms");
    assert(rc == 0);
    assert(s.find("sig run: kind=expression") != std::string::npos);
    assert(s.find("s0: expression value=1.4 U") != std::string::npos);
  }
  // Table golden through the CLI: mid-point linear interp is exact here.
  {
    const std::string path = "/tmp/wengine_test_sim_sigtable.csv";
    {
      std::ofstream out(path);
      out << "time,value\n0,1.0\n1,3.0\n";
    }
    const auto [rc, s] =
        run_sig("run --sig-kind table --sig-table " + path + " --steps 5 --dt 100ms");
    assert(rc == 0);
    assert(s.find("sig run: kind=table") != std::string::npos);
    assert(s.find("table: " + path + " (2 points)") != std::string::npos);
    assert(s.find("s0: table value=2 U") != std::string::npos);
    std::remove(path.c_str());
  }
  // Missing table file is a runtime failure, never a crash.
  assert(run_sig("run --sig-kind table --sig-table /tmp/wengine-no-such-table.csv").first ==
         1);
  // Lua golden through the CLI: scripted sine at t = 0.2 s (2 steps x 0.1 s).
  {
    const std::string path = "/tmp/wengine_test_sim_sig.lua";
    {
      std::ofstream out(path);
      out << "return 2*math.sin(2*math.pi*t)\n";
    }
    const auto [rc, s] =
        run_sig("run --sig-kind lua --sig-lua " + path + " --steps 200 --dt 1ms");
    assert(rc == 0);
    assert(s.find("sig run: kind=lua") != std::string::npos);
    assert(s.find("lua: " + path + " (") != std::string::npos);
    // CLI prints 6 significant digits; bit-exactness is proven engine-side.
    assert(s.find("s0: lua value=1.90211 U") != std::string::npos);
    std::remove(path.c_str());
  }
  // Lua usage errors: missing script, bad syntax, cross-kind, missing file.
  assert(run_sig("run --sig-kind lua --steps 2").first == 2);
  assert(run_sig("run --sig-kind sine --sig-lua x.lua").first == 2);
  {
    const std::string path = "/tmp/wengine_test_sim_sig_bad.lua";
    {
      std::ofstream out(path);
      out << "return 1+\n";
    }
    assert(run_sig("run --sig-kind lua --sig-lua " + path).first == 2);
    std::remove(path.c_str());
  }
  assert(run_sig("run --sig-kind lua --sig-lua /tmp/wengine-no-such.lua").first == 1);
  // Lua runtime failure mid-run: os is absent from the sandbox.
  {
    const std::string path = "/tmp/wengine_test_sim_sig_evil.lua";
    {
      std::ofstream out(path);
      out << "return os.execute('x')\n";
    }
    const auto [rc, s] = run_sig("run --sig-kind lua --sig-lua " + path + " --steps 2");
    assert(rc == 1 && s.find("lua: ") != std::string::npos);
    std::remove(path.c_str());
  }
  {
    const auto [rc, s] = run_sig("--help");
    assert(rc == 0 && s.find("sig run") != std::string::npos);
  }
  // SHM publishes signal channels through the signal adapter.
  // Default output stays concise; --emit-health adds the per-cause breakdown.
  {
    const std::string seg = "/wengine-test-sim-sig-" + std::to_string(::getpid());
    const auto [rc, s] = run_sig("run --sig-kind dc --sig-offset 1V --steps 5 --emit-shm " +
                                 seg);
    assert(rc == 0);
    assert(s.find("shm: " + seg + " (5 frames)") != std::string::npos);
    assert(s.find("attempted=") == std::string::npos);
    const auto [rc_h, s_h] =
        run_sig("run --sig-kind dc --sig-offset 1V --steps 5 --emit-shm " + seg +
                " --emit-health");
    assert(rc_h == 0);
    assert(s_h.find("shm: " + seg + " (5 frames, attempted=5") != std::string::npos);
    assert(s_h.find("published_local=5") != std::string::npos);
    assert(s_h.find("local acceptance only") != std::string::npos);
  }
}

void test_cli_per_output_sampling() {
  // Per-output stride: decimated CSV keeps step-aligned steps only.
  {
    const std::string path = "/tmp/wengine_test_sim_stride.csv";
    std::remove(path.c_str());
    const auto [rc, s] =
        run_sig("run --sig-kind dc --sig-offset 1V --steps 6 --emit-csv " + path +
                " --emit-csv-stride 2");
    assert(rc == 0);
    assert(s.find("stride=2") != std::string::npos);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(in, line)) {
      rows.push_back(line);
    }
    assert(rows.size() == 3);  // steps 2,4,6 (decimation aliases bandwidth)
    assert(rows[0].rfind("2,", 0) == 0);
    assert(rows[1].rfind("4,", 0) == 0);
    assert(rows[2].rfind("6,", 0) == 0);
    std::remove(path.c_str());
  }
  // Per-output select: subset CSV keeps serials only (rows filter, ticks stay).
  {
    const std::string path = "/tmp/wengine_test_sim_select.csv";
    std::remove(path.c_str());
    const auto [rc, s] =
        run_sig("run --sig-kind dc --sig-offset 1V --sig-count 3 --steps 2 --emit-csv " +
                path + " --emit-csv-select 0,2");
    assert(rc == 0);
    assert(s.find("select=0,2") != std::string::npos);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(in, line)) {
      rows.push_back(line);
    }
    assert(rows.size() == 4);  // 2 steps x 2 serials
    for (const std::string &r : rows) {
      // CSV columns: step,time,sig_id,value,quantity — check sig_id exactly.
      const std::size_t c1 = r.find(',');
      const std::size_t c2 = r.find(',', c1 + 1);
      const std::size_t c3 = r.find(',', c2 + 1);
      const std::size_t c4 = r.find(',', c3 + 1);
      assert(c1 != std::string::npos && c2 != std::string::npos &&
             c3 != std::string::npos && c4 != std::string::npos);
      const std::string id = r.substr(c2 + 1, c3 - c2 - 1);
      assert(id == "0" || id == "2");
    }
    std::remove(path.c_str());
  }
  // Global + per-output intersection: emit-every=2, stride=3 -> steps 6,12.
  {
    const std::string path = "/tmp/wengine_test_sim_both.csv";
    std::remove(path.c_str());
    const auto [rc, s] =
        run_sig("run --sig-kind dc --sig-offset 1V --steps 12 --emit-every 2 --emit-csv " +
                path + " --emit-csv-stride 3");
    assert(rc == 0);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(in, line)) {
      rows.push_back(line);
    }
    assert(rows.size() == 2);
    assert(rows[0].rfind("6,", 0) == 0);
    assert(rows[1].rfind("12,", 0) == 0);
    std::remove(path.c_str());
  }
  // Full-rate recorder + decimated live output: identical final State.
  {
    const std::string full = "/tmp/wengine_test_sim_full.csv";
    const std::string seg = "/wengine-test-sim-samp-" + std::to_string(::getpid());
    std::remove(full.c_str());
    const auto [rc1, s1] = run_sig("run --sig-kind dc --sig-offset 2V --steps 6 --emit-csv " +
                                   full + " --emit-shm " + seg + " --emit-shm-stride 3");
    assert(rc1 == 0);
    assert(s1.find("csv: " + full + " (6 rows") != std::string::npos);
    assert(s1.find("shm: " + seg) != std::string::npos);
    assert(s1.find("stride=3") != std::string::npos);
    assert(s1.find("s0: dc value=2 U") != std::string::npos);
    std::ifstream in(full);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    std::size_t n_rows = 0;
    std::string line;
    while (std::getline(in, line)) {
      ++n_rows;
    }
    assert(n_rows == 6);  // recorder stayed complete while live decimated
    std::remove(full.c_str());
    const auto [rc2, s2] = run_sig("run --sig-kind dc --sig-offset 2V --steps 6");
    assert(rc2 == 0);
    // Same final value with and without sampling wrappers.
    assert(s1.find("s0: dc value=2 U") != std::string::npos);
    assert(s2.find("s0: dc value=2 U") != std::string::npos);
  }
  // Muted stride drops everything loudly but still succeeds.
  {
    const std::string path = "/tmp/wengine_test_sim_muted.csv";
    std::remove(path.c_str());
    const auto [rc, s] = run_sig("run --sig-kind dc --steps 3 --emit-csv " + path +
                                 " --emit-csv-stride 0");
    assert(rc == 0);
    assert(s.find("dropped=3") != std::string::npos);
    std::ifstream in(path);
    std::string header;
    assert(static_cast<bool>(std::getline(in, header)));
    std::string tail;
    assert(!static_cast<bool>(std::getline(in, tail)));
    std::remove(path.c_str());
  }
  // Usage errors: orphan stride/select, bad lists, out-of-range, over capacity.
  assert(run_sig("run --steps 2 --emit-csv-stride 2").first == 2);
  assert(run_sig("run --steps 2 --emit-shm-stride 2").first == 2);
  assert(run_sig("run --steps 2 --emit-csv-select 0").first == 2);
  // UDP payload budget (PHYSI-41): orphan/invalid budgets are usage errors.
  assert(run_sig("run --steps 2 --emit-udp-budget 1200").first == 2);
  assert(run_sim("run --steps 2 --emit-udp-budget 1200").first == 2);
  assert(run_sig("run --steps 2 --emit-udp 127.0.0.1:9 --emit-udp-budget 0").first == 2);
  assert(run_sig("run --steps 2 --emit-udp 127.0.0.1:9 --emit-udp-budget 65508").first == 2);
  {
    // Default budget is reported; a single datagram to a closed port still
    // sends (the first send on a fresh connected socket cannot observe the
    // async ICMP refusal yet).
    const auto [rc, s] = run_sig("run --sig-kind dc --steps 1 --emit-udp 127.0.0.1:9");
    assert(rc == 0);
    assert(s.find("budget=1200") != std::string::npos);
  }
  {
    // An indivisible over-budget record fails loudly with the need=
    // diagnostic (dc source: sig.count + sig.0.U = 84 + 18 + 16 = 118 > 100).
    const auto [rc, s] = run_sig(
        "run --sig-kind dc --steps 2 --emit-udp 127.0.0.1:9 --emit-udp-budget 100");
    assert(rc == 1);
    assert(s.find("budget=100") != std::string::npos);
    assert(s.find("need=118") != std::string::npos);
  }
  assert(run_sig("run --sig-kind dc --steps 2 --emit-csv /tmp/x.csv --emit-csv-select 5").first ==
         2);
  {
    const std::string path = "/tmp/wengine_test_sim_dup.csv";
    assert(run_sig("run --sig-kind dc --sig-count 3 --steps 2 --emit-csv " + path +
                   " --emit-csv-select 0,0")
               .first == 2);
    assert(run_sig("run --sig-kind dc --steps 2 --emit-csv " + path +
                   " --emit-csv-select ' '")
               .first == 2);
    assert(run_sig("run --sig-kind dc --steps 2 --emit-csv " + path +
                   " --emit-csv-stride -1")
               .first == 2);
  }
  {
    const std::string seg = "/wengine-test-sim-cap-" + std::to_string(::getpid());
    // 70 sources need 71 channels > 64: unfiltered transport refuses fast.
    assert(run_sig("run --sig-kind dc --sig-count 70 --steps 2 --emit-shm " + seg).first == 2);
    // ...but a fitting subset is accepted.
    const auto [rc, s] = run_sig("run --sig-kind dc --sig-count 70 --steps 2 --emit-shm " +
                                 seg + " --emit-shm-select 0,1,2");
    assert(rc == 0);
    assert(s.find("select=0,1,2") != std::string::npos);
  }
  // sim projectile: strides allowed, selections need the signal solver.
  assert(run_sim("run --steps 2 --emit-shm-stride 2").first == 2);
  assert(run_sim("run --steps 2 --emit-csv /tmp/x.csv --emit-csv-select 0").first == 2);
  {
    const std::string path = "/tmp/wengine_test_sim_pstride.csv";
    std::remove(path.c_str());
    const auto [rc, s] =
        run_sim("run --steps 4 --particles 1 --emit-csv " + path + " --emit-csv-stride 2");
    assert(rc == 0);
    assert(s.find("stride=2") != std::string::npos);
    assert(s.find("csv: " + path + " (2 rows") != std::string::npos);
    std::remove(path.c_str());
  }
}

} // namespace

int main() {
  test_solver_golden();
  test_holes_skipped_and_determinism();
  test_bad_dt();
  test_cli_golden();
  test_cli_csv();
  test_cli_shm();
  test_cli_solver_selection();
  test_cli_errors();
  test_cli_signal();
  test_cli_per_output_sampling();
  std::cout << "test_sim: OK\n";
  return 0;
}
