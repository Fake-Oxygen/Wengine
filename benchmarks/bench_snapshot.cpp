// Snapshot-capture benchmark: capture cost isolated from solver adaptation
// and transport.
//
// Scenarios (report-only, no thresholds — unit tests pin correctness):
//   capture-reuse   warmed fill_snapshot over a fixed population
//                   (ns/capture + allocs; expect 0 allocs after warmup)
//   capture-owning  take_snapshot control (fresh vectors per capture)
//   growth          cold-buffer first fill (bounded allocs) + steady fills
//   fanout          Runner + solver + K synchronous counting sinks over M
//                   steps (ns/emit-step + allocs; multi-sink emission)
// Populations come from a runtime xorshift PRNG (seeded from argc) so the
// compiler cannot fold the loops; the sim clock advances per capture so
// successive fills are not CSE-able. Checksums pin against DCE.
//
// Repro:
//   bench_snapshot [particles] [sources] [captures]

#include "wengine/core/runner.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/world/state.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>

// Allocation counter for the timed regions (steady-state capture must
// perform none; growth is measured separately in the `growth` scenario).
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

template <typename T> inline void do_not_optimize(const T &v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(v) : "memory");
#else
  volatile const T *p = &v;
  (void)p;
#endif
}

namespace {

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

struct CountSink : public wengine::emit::ISink {
  std::uint64_t calls = 0;
  void on_snapshot(const wengine::world::Snapshot &) override { ++calls; }
};

} // namespace

int main(int argc, char **argv) {
  std::size_t particles = 256;
  std::size_t sources = 256;
  std::uint64_t captures = 20000;
  if (argc > 1) {
    particles = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    sources = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
  }
  if (argc > 3) {
    captures = std::strtoull(argv[3], nullptr, 10);
    if (captures == 0) {
      captures = 20000;
    }
  }

  std::uint32_t rng = 0x9E3779B9u ^ (static_cast<std::uint32_t>(argc) * 0x85EBCA6Bu);
  wengine::world::State state;
  for (std::size_t k = 0; k < particles; ++k) {
    wengine::world::Entity e = state.spawn_entity();
    state.particles.attach(e.index);
    state.particles.pos_x[e.index] = -10.0 + (xorshift(rng) % 20000) / 1000.0;
    state.particles.pos_y[e.index] = -10.0 + (xorshift(rng) % 20000) / 1000.0;
    state.particles.vel_x[e.index] = -10.0 + (xorshift(rng) % 20000) / 1000.0;
    state.particles.vel_y[e.index] = -10.0 + (xorshift(rng) % 20000) / 1000.0;
  }
  for (std::size_t k = 0; k < sources; ++k) {
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kSine;
    p.freq_hz = 10.0 + (xorshift(rng) % 10000) / 10.0;
    p.amp = 0.5 + (xorshift(rng) % 5000) / 1000.0;
    p.phase_rad = (xorshift(rng) % 6283) / 1000.0;
    wengine::world::Entity e = state.spawn_entity();
    if (!wengine::world::configure_source(e, state.signals, p, 0.0).ok()) {
      std::cout << "bench_snapshot: setup failed\n";
      return 1;
    }
  }

  // --- capture-reuse: warmed fill_snapshot, fixed population ---
  {
    wengine::world::Snapshot buf;
    wengine::world::fill_snapshot(state, buf); // warm capacities (not counted)
    g_allocs = 0;
    g_counting = true;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (std::uint64_t k = 0; k < captures; ++k) {
      state.sim_time += 0.001; // live clock: every fill observes new data
      state.step_index += 1;
      wengine::world::fill_snapshot(state, buf);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    g_counting = false;
    const std::size_t allocs = g_allocs;
    double checksum = buf.sim_time + static_cast<double>(buf.step_index);
    for (std::size_t k = 0; k < buf.particle_count(); ++k) {
      checksum += buf.pos_x[k] + buf.sig_value[k % buf.signal_count()];
    }
    do_not_optimize(checksum);
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "bench_snapshot capture-reuse: " << particles << " particles + "
              << sources << " sources x " << captures << " captures, "
              << static_cast<double>(ns) / static_cast<double>(captures)
              << " ns/capture allocs=" << allocs << " (checksum=" << checksum
              << ")\n";
  }

  // --- capture-owning: take_snapshot control (fresh vectors per capture) ---
  {
    g_allocs = 0;
    g_counting = true;
    const auto t0 = std::chrono::high_resolution_clock::now();
    double checksum = 0.0;
    for (std::uint64_t k = 0; k < captures; ++k) {
      state.sim_time += 0.001;
      state.step_index += 1;
      const wengine::world::Snapshot snap = wengine::world::take_snapshot(state);
      checksum += snap.sim_time;
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    g_counting = false;
    const std::size_t allocs = g_allocs;
    do_not_optimize(checksum);
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "bench_snapshot capture-owning: " << particles << " particles + "
              << sources << " sources x " << captures << " captures, "
              << static_cast<double>(ns) / static_cast<double>(captures)
              << " ns/capture allocs=" << allocs << " (checksum=" << checksum
              << ")\n";
  }

  // --- growth: cold buffer first fill (bounded) + steady fills (free) ---
  {
    wengine::world::Snapshot cold;
    g_allocs = 0;
    g_counting = true;
    wengine::world::fill_snapshot(state, cold);
    g_counting = false;
    const std::size_t growth_allocs = g_allocs;
    double checksum = 0.0;
    for (std::size_t k = 0; k < cold.particle_count(); ++k) {
      checksum += cold.pos_x[k];
    }
    do_not_optimize(checksum);
    g_allocs = 0;
    g_counting = true;
    for (std::uint64_t k = 0; k < captures; ++k) {
      wengine::world::fill_snapshot(state, cold);
    }
    g_counting = false;
    const std::size_t steady_allocs = g_allocs;
    std::cout << "bench_snapshot growth: cold fill allocs=" << growth_allocs
              << " (bound: 7 vectors), steady allocs=" << steady_allocs << " / "
              << captures << " fills (checksum=" << checksum << ")\n";
  }

  // --- fanout: Runner + solver + K counting sinks over M steps ---
  for (const int sinks : {1, 4}) {
    wengine::core::Runner runner{{.dt = 0.01}};
    wengine::solvers::ProjectileSolver solver;
    runner.add_solver(solver);
    std::uint32_t prng = 0x51ED270Bu ^ static_cast<std::uint32_t>(sinks);
    for (std::size_t k = 0; k < particles; ++k) {
      wengine::world::Entity e = runner.state().spawn_entity();
      runner.state().particles.attach(e.index);
      runner.state().particles.pos_x[e.index] =
          -10.0 + (xorshift(prng) % 20000) / 1000.0;
      runner.state().particles.vel_y[e.index] =
          -10.0 + (xorshift(prng) % 20000) / 1000.0;
    }
    CountSink sink_storage[4];
    for (int k = 0; k < sinks; ++k) {
      runner.add_sink(sink_storage[k]);
    }
    if (!runner.run(10).ok()) { // warm buffer + solver (not counted)
      std::cout << "bench_snapshot: warmup failed\n";
      return 1;
    }
    const std::uint64_t steps = captures / 10;
    g_allocs = 0;
    g_counting = true;
    const auto t0 = std::chrono::high_resolution_clock::now();
    const wengine::core::Status st = runner.run(steps);
    const auto t1 = std::chrono::high_resolution_clock::now();
    g_counting = false;
    if (!st.ok()) {
      std::cout << "bench_snapshot: run failed\n";
      return 1;
    }
    const std::size_t allocs = g_allocs;
    const wengine::world::Snapshot snap =
        wengine::world::take_snapshot(runner.state());
    double checksum = 0.0;
    for (std::size_t k = 0; k < snap.particle_count(); ++k) {
      checksum += snap.pos_x[k] + snap.pos_y[k];
    }
    do_not_optimize(checksum);
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "bench_snapshot fanout-" << sinks << ": " << particles
              << " particles x " << steps << " steps, "
              << static_cast<double>(ns) / static_cast<double>(steps)
              << " ns/emit-step allocs=" << allocs
              << " (checksum=" << checksum << ")\n";
  }

  return 0;
}
