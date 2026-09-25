#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/paced_run.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine;

// Deterministic clock with the SteadyClock shape: manual time, every
// sleep_for advances instantly and records its absolute target so tests
// can prove deadlines are epoch-absolute (no drift accumulation).
struct FakeClock {
  using duration = std::chrono::nanoseconds;
  using time_point = std::chrono::steady_clock::time_point;
  static inline time_point s_now{};
  static inline time_point s_epoch{};
  static inline std::vector<std::int64_t> s_targets; // ns since epoch
  static inline std::uint64_t s_sleeps = 0;

  static time_point now() noexcept { return s_now; }
  static void sleep_for(duration d) {
    s_targets.push_back(
        static_cast<std::int64_t>((s_now + d - s_epoch).count()));
    s_now += d;
    ++s_sleeps;
  }
  static void reset(time_point epoch) {
    s_epoch = epoch;
    s_now = epoch;
    s_targets.clear();
    s_sleeps = 0;
  }
  static void advance(duration d) { s_now += d; }
};

namespace {

bool never_stop() noexcept { return false; }
unsigned g_idle_calls = 0;
void count_idle() noexcept { ++g_idle_calls; }

core::RunConfig make_projectile(std::uint64_t steps) {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "projectile";
  cfg.particles = 1;
  return cfg;
}

core::RunConfig make_signal(std::uint64_t steps) {
  core::RunConfig cfg = make_projectile(steps);
  cfg.solver = "signal";
  cfg.particles = 0;
  cfg.signal.set = true;
  return cfg;
}

void test_validate() {
  assert(!solvers::paced::validate(0.0, 1).ok());
  assert(!solvers::paced::validate(-10.0, 1).ok());
  assert(!solvers::paced::validate(std::numeric_limits<double>::infinity(), 1).ok());
  assert(!solvers::paced::validate(std::numeric_limits<double>::quiet_NaN(), 1).ok());
  assert(!solvers::paced::validate(1000.0, 0).ok());
  assert(solvers::paced::validate(1000.0, 1).ok());
  assert(solvers::paced::period_ns(1000.0) == 1000000);
  assert(solvers::paced::period_ns(1.0) == 1000000000);
  assert(solvers::paced::period_ns(0.0) == 0);
}

void test_paced_matches_unpaced() {
  // Projectile: paced (chunk 1 and chunk 7) vs one-shot headless, exact.
  for (std::uint64_t chunk : {1, 7}) {
    core::RunConfig cfg = make_projectile(50);
    solvers::ConfiguredRun paced_run;
    assert(solvers::ConfiguredRun::build(
               cfg, std::span<core::ISolver *>{}, false, paced_run)
               .ok());
    const FakeClock::time_point epoch{};
    FakeClock::reset(epoch);
    g_idle_calls = 0;
    solvers::paced::Stats stats;
    assert(solvers::paced::run_paced<FakeClock>(paced_run, cfg.steps, 1000.0, chunk,
                                                epoch, never_stop, count_idle, stats)
               .ok());
    assert(stats.steps_done == 50 && stats.late_steps == 0);
    assert(g_idle_calls > 0); // waits happened outside the solver
    world::State ref;
    assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, ref).ok());
    assert(paced_run.state() == ref);
    assert(world::take_snapshot(paced_run.state()) == world::take_snapshot(ref));
  }
  // Signal solver: same contract on the signal spawn path.
  {
    core::RunConfig cfg = make_signal(60);
    solvers::ConfiguredRun paced_run;
    assert(solvers::ConfiguredRun::build(
               cfg, std::span<core::ISolver *>{}, false, paced_run)
               .ok());
    const FakeClock::time_point epoch{};
    FakeClock::reset(epoch);
    solvers::paced::Stats stats;
    assert(solvers::paced::run_paced<FakeClock>(paced_run, cfg.steps, 2000.0, 3, epoch,
                                                never_stop, count_idle, stats)
               .ok());
    world::State ref;
    assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, ref).ok());
    assert(paced_run.state() == ref);
  }
}

void test_absolute_deadlines_no_drift() {
  // Chunk 3 over 10 steps at 10 kHz (period 100 us < 1 ms slice, so one
  // sleep per chunk): sleep targets must be exact multiples of the period
  // from the epoch (3P, 6P, 9P, 10P), never now()+period re-anchored.
  core::RunConfig cfg = make_projectile(10);
  solvers::ConfiguredRun run;
  assert(
      solvers::ConfiguredRun::build(cfg, std::span<core::ISolver *>{}, false, run)
          .ok());
  const FakeClock::time_point epoch{};
  FakeClock::reset(epoch);
  solvers::paced::Stats stats;
  assert(solvers::paced::run_paced<FakeClock>(run, cfg.steps, 10000.0, 3, epoch,
                                              never_stop, count_idle, stats)
             .ok());
  const std::int64_t p = 100000;
  const std::vector<std::int64_t> want{3 * p, 6 * p, 9 * p, 10 * p};
  assert(FakeClock::s_targets == want);
  assert(stats.late_steps == 0);
}

void test_late_counted_never_skipped() {
  // Idle hook burns 5 periods per wait: every chunk lands past its deadline.
  // Policy: count it loudly, keep every physics step and dt unchanged.
  core::RunConfig cfg = make_projectile(8);
  solvers::ConfiguredRun run;
  assert(
      solvers::ConfiguredRun::build(cfg, std::span<core::ISolver *>{}, false, run)
          .ok());
  const FakeClock::time_point epoch{};
  FakeClock::reset(epoch);
  const std::int64_t p = solvers::paced::period_ns(1000.0);
  // Non-multiple burn (5P + 12345ns) so no chunk lands exactly on its
  // deadline: every chunk must count as late, never as on-time.
  auto burn_idle = [&] { FakeClock::advance(FakeClock::duration(5 * p + 12345)); };
  solvers::paced::Stats stats;
  assert(solvers::paced::run_paced<FakeClock>(run, cfg.steps, 1000.0, 2, epoch,
                                              never_stop, burn_idle, stats)
             .ok());
  assert(stats.steps_done == 8);
  assert(stats.late_steps == 8); // all 4 chunks of 2 missed, none skipped
  assert(stats.max_late_s > 0.0);
  world::State ref;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, ref).ok());
  assert(run.state() == ref); // bit-identical despite lateness
}

void test_stop_responsive() {
  // Stop fires once the fake clock reaches 2 periods: the 3rd chunk never
  // runs, and the wait itself aborts instead of sleeping through.
  core::RunConfig cfg = make_projectile(10);
  solvers::ConfiguredRun run;
  assert(
      solvers::ConfiguredRun::build(cfg, std::span<core::ISolver *>{}, false, run)
          .ok());
  const FakeClock::time_point epoch{};
  FakeClock::reset(epoch);
  auto stop_at_two = [&]() noexcept {
    return FakeClock::now() >= epoch + FakeClock::duration(2000000);
  };
  solvers::paced::Stats stats;
  assert(solvers::paced::run_paced<FakeClock>(run, cfg.steps, 1000.0, 1, epoch,
                                              stop_at_two, count_idle, stats)
             .ok());
  assert(stats.steps_done == 2);
  world::State ref2;
  core::RunConfig cfg2 = make_projectile(2);
  assert(solvers::run_headless(cfg2, std::span<emit::ISink *>{}, ref2).ok());
  assert(run.state() == ref2);
}

std::pair<int, std::string> run_line(std::string_view cmd, std::string line) {
  std::array<std::string_view, cli::kMaxArgs> buf{};
  const std::size_t n = cli::tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  cli::ArgCursor cur{args};
  std::ostringstream out;
  int rc = 2;
  if (cmd == "sim") {
    rc = cli::commands::SimSpec::run(cur, out);
  } else {
    rc = cli::commands::SigSpec::run(cur, out);
  }
  return {rc, out.str()};
}

void test_cli_paced() {
  // Fast wall rates keep the real-clock smoke instant (2 steps at 100 kHz).
  {
    const auto [rc, s] = run_line("sim", "run --steps 2 --paced --rate 100kHz");
    assert(rc == 0);
    assert(s.find("paced: rate=") != std::string::npos);
    // Lateness is wall-clock dependent (10 us periods often oversleep on a
    // normal kernel): the contract is that it is counted, never that it is
    // zero. Steps still complete exactly.
    assert(s.find("late=") != std::string::npos);
    assert(s.find("steps=2") != std::string::npos);
  }
  {
    // Real-time default: 2 steps at dt=10ms -> 200Hz, still instant.
    const auto [rc, s] = run_line("sim", "run --steps 2 --dt 10ms --paced");
    assert(rc == 0 && s.find("paced: rate=100Hz") != std::string::npos);
  }
  {
    const auto [rc, s] =
        run_line("sig", "run --sig-kind dc --steps 2 --paced --rate 100kHz");
    assert(rc == 0);
    assert(s.find("paced: rate=") != std::string::npos);
    assert(s.find("s0: dc value=") != std::string::npos);
  }
  // Usage errors: rate without paced, bad/zero rates.
  assert(run_line("sim", "run --steps 2 --rate 1kHz").first == 2);
  assert(run_line("sig", "run --steps 2 --rate 1kHz").first == 2);
  assert(run_line("sim", "run --steps 2 --paced --rate 0Hz").first == 2);
  assert(run_line("sim", "run --steps 2 --paced --rate bogus").first == 2);
  assert(run_line("sig", "run --steps 2 --paced --rate 0Hz").first == 2);
  // Fast batch mode is untouched (no paced line).
  {
    const auto [rc, s] = run_line("sim", "run --steps 2");
    assert(rc == 0 && s.find("paced:") == std::string::npos);
  }
}

} // namespace

int main() {
  test_validate();
  test_paced_matches_unpaced();
  test_absolute_deadlines_no_drift();
  test_late_counted_never_skipped();
  test_stop_responsive();
  test_cli_paced();
  std::cout << "test_paced: OK\n";
  return 0;
}
