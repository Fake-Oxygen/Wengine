#include "wengine/core/runner.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

// Snapshot buffer-reuse allocation test: counts global operator new calls
// across warmed, fixed-population capture/emission and asserts the steady
// state performs none. A take_snapshot control proves the counter observes
// snapshot allocations (the zeros below are not vacuous).

namespace {

std::size_t g_allocs = 0;
bool g_counting = false;

} // namespace

void *operator new(std::size_t n) {
  if (g_counting) {
    ++g_allocs;
  }
  void *p = std::malloc(n != 0 ? n : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

void *operator new[](std::size_t n) {
  if (g_counting) {
    ++g_allocs;
  }
  void *p = std::malloc(n != 0 ? n : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace wengine;

struct CountSink : emit::ISink {
  std::vector<std::uint64_t> steps; // caller pre-reserves: push_back is free
  std::uint64_t calls = 0;
  void on_snapshot(const world::Snapshot &s) override {
    ++calls;
    steps.push_back(s.step_index);
  }
};

struct Capture : emit::ISink {
  std::vector<world::Snapshot> snaps; // owned copies, like a retaining sink
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

signal::SignalParams dc_params(double offset) {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kDc;
  p.offset = offset;
  return p;
}

void start_counting() {
  g_allocs = 0;
  g_counting = true;
}

std::size_t stop_counting() {
  g_counting = false;
  return g_allocs;
}

world::State make_mixed(std::size_t particles, std::size_t signals) {
  world::State s;
  s.sim_time = 0.25;
  s.step_index = 25;
  for (std::size_t k = 0; k < particles; ++k) {
    const world::Entity e = s.spawn_entity();
    s.particles.attach(e.index);
    s.particles.pos_x[e.index] = static_cast<double>(k);
    s.particles.vel_y[e.index] = -static_cast<double>(k);
  }
  for (std::size_t k = 0; k < signals; ++k) {
    const world::Entity e = s.spawn_entity();
    assert(world::configure_source(e, s.signals, dc_params(static_cast<double>(k)),
                                   0.0)
               .ok());
  }
  return s;
}

void test_fill_alloc_free_after_warmup() {
  world::State s = make_mixed(64, 64);
  world::Snapshot buf;
  world::fill_snapshot(s, buf); // warm capacities (not counted)
  assert(buf == world::take_snapshot(s));

  start_counting();
  for (int k = 0; k < 1000; ++k) {
    world::fill_snapshot(s, buf); // same shape every tick: no growth
  }
  assert(stop_counting() == 0);

  assert(buf == world::take_snapshot(s)); // values still exact
}

void test_take_allocates_control() {
  const world::State s = make_mixed(64, 64);
  start_counting();
  for (int k = 0; k < 100; ++k) {
    const world::Snapshot snap = world::take_snapshot(s);
    assert(snap.signal_count() == 64);
  }
  const std::size_t allocs = stop_counting();
  assert(allocs > 0); // the counter observes snapshot allocations
  std::cout << "  take_snapshot control: " << allocs << " allocs / 100 captures\n";
}

void test_growth_bounded_then_free() {
  world::State s = make_mixed(128, 128);
  world::Snapshot buf; // cold
  start_counting();
  world::fill_snapshot(s, buf); // first fill: at most one alloc per vector
  const std::size_t growth = stop_counting();
  assert(buf == world::take_snapshot(s));
  std::cout << "  cold fill growth: " << growth << " allocs (bound: 7)\n";
  assert(growth <= 7);

  start_counting();
  for (int k = 0; k < 500; ++k) {
    world::fill_snapshot(s, buf);
  }
  assert(stop_counting() == 0); // steady same-shape traffic is free

  // Shrinking (despawn to zero) never allocates either.
  s.clear();
  start_counting();
  world::fill_snapshot(s, buf);
  assert(stop_counting() == 0);
  assert(buf.signal_count() == 0 && buf.particle_count() == 0);
}

void test_runner_reuse_zero_allocs() {
  // Fixed population, three synchronous sinks, default emit_every=1.
  core::Runner runner{{.dt = 0.01}};
  solvers::ProjectileSolver solver;
  runner.add_solver(solver);
  for (int k = 0; k < 4; ++k) {
    world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] = static_cast<double>(k);
  }
  CountSink a;
  CountSink b;
  CountSink c;
  a.steps.reserve(205);
  b.steps.reserve(205);
  c.steps.reserve(205);
  runner.add_sink(a);
  runner.add_sink(b);
  runner.add_sink(c);
  assert(runner.run(5).ok()); // warm buffer + solver caches (not counted)

  start_counting();
  assert(runner.run(200).ok());
  const std::size_t allocs = stop_counting();
  std::cout << "  runner fan-out (3 sinks, 200 steps): " << allocs << " allocs\n";
  assert(allocs == 0);
  assert(a.calls == 205 && b.calls == 205 && c.calls == 205);
  assert(a.steps.back() == 205 && b.steps == a.steps && c.steps == a.steps);

  // Emission still observes the documented steps and identical final state.
  core::Runner quiet{{.dt = 0.01, .emit_every = 0}};
  solvers::ProjectileSolver qsolver;
  quiet.add_solver(qsolver);
  for (int k = 0; k < 4; ++k) {
    world::Entity e = quiet.state().spawn_entity();
    quiet.state().particles.attach(e.index);
    quiet.state().particles.pos_x[e.index] = static_cast<double>(k);
  }
  assert(quiet.run(205).ok());
  assert(runner.state() == quiet.state());
}

void test_retained_copies_survive_runner_reuse() {
  // A retaining sink's earlier copies are unaffected by later emissions
  // into the reused buffer (owned Snapshot copies, never references).
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, dc_params(1.0), 0.0)
             .ok());
  Capture cap;
  runner.add_sink(cap);
  assert(runner.run(3).ok());
  assert(cap.snaps.size() == 3);
  const world::Snapshot first = cap.snaps[0];
  assert(runner.run(3).ok());
  assert(cap.snaps.size() == 6);
  assert(cap.snaps[0] == first); // earlier emission intact
  assert(cap.snaps[0].step_index == 1 && cap.snaps[5].step_index == 6);
  assert(cap.snaps[0].sig_value[0] == 1.0);
}

void test_subset_scratch_reuse() {
  // The subset wrapper's reused scratch keeps steady traffic free while
  // retained inner copies stay exact.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  world::Entity e0 = runner.state().spawn_entity();
  assert(world::configure_source(e0, runner.state().signals, dc_params(1.0), 0.0)
             .ok());
  world::Entity e1 = runner.state().spawn_entity();
  assert(world::configure_source(e1, runner.state().signals, dc_params(2.0), 0.0)
             .ok());
  CountSink inner;
  inner.steps.reserve(103);
  emit::SignalSubsetSink sub(inner);
  sub.set_subscription({0});
  runner.add_sink(sub);
  assert(runner.run(3).ok()); // warm scratch (not counted)

  start_counting();
  assert(runner.run(100).ok());
  const std::size_t allocs = stop_counting();
  std::cout << "  subset fan-out (100 steps): " << allocs << " allocs\n";
  assert(allocs == 0);
  assert(inner.calls == 103 && sub.matched() == 103);
  assert(sub.forwarded_rows() == 103 && sub.dropped() == 0);
}

} // namespace

int main() {
  std::cout << "test_snapshot_reuse:\n"; // warm iostream before counting
  test_fill_alloc_free_after_warmup();
  test_take_allocates_control();
  test_growth_bounded_then_free();
  test_runner_reuse_zero_allocs();
  test_retained_copies_survive_runner_reuse();
  test_subset_scratch_reuse();
  std::cout << "test_snapshot_reuse: OK\n";
  return 0;
}
