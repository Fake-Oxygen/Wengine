// Sparse-world / serial-lookup / custom-source scaling benchmark.
//
// Covers the pool-layout scaling risks behind one signal at a high slot:
// every Signals column grows with the slot index, every slot reserves
// kMaxExprNodes (64) + kMaxOps (8) cells even for a sine, find_serial is a
// linear scan inside every sum operand / Lua cache purge / load-time check,
// select_signals is a nested scan, and the table/script stores retain
// despawned assets.
//
// Scenarios (report-only, no thresholds — tests pin correctness):
//   dense-vs-sparse  equal live sine populations, capacity live vs live*gap:
//                    pool bytes vs live/capacity, ns/step, ns/capture, and
//                    the derived per-slot scan vs per-source eval crossover
//   lookup           direct find_serial ns/lookup over a capacity sweep
//                    (slope = ns/slot scanned; cap@1us = capacity where one
//                    lookup costs a microsecond)
//   fanin            K sums over G generators, fan-in {1,2,4,8}: ns/step and
//                    the per-operand slope (lookup cost inside the solver)
//   subs             S subset subscribers partitioning one snapshot over C
//                    captures: ns/tick and per-pair (row x serial) cost of
//                    the nested scan
//   churn            R rounds of add/step/despawn for expression + table +
//                    Lua customs: retained table/lua store bytes per round
//                    with zero live customs, plus the Lua dead-cache purge
//                    step vs steady steps
//   separate         one mixed world (sine/expr/table/lua/sums): native
//                    solver, Lua-attributed, snapshot fill, subset filter,
//                    and save/load costs with byte counts, each checksummed
//   crossover        the measured crossover points in one place plus the
//                    reading guide mapping each to its candidate fix
//
// Repro:
//   bench_signal_scale [live] [gap] [steps] [sums] [fanin] [subs] [customs]
//   defaults:          256     16    2000    8      8       4      8
// Bounds (memory-bounded CLI sizes, clamped with a notice): live 1..4096,
// gap 1..256 with live*gap <= 65536, steps 1..20000, sums 0..64,
// fanin 1..kMaxOps, subs 0..16, customs 0..32. Per-scenario step counts
// derive from `steps` with documented divisors so one knob scales the run.
//
// All waveform params come from a runtime xorshift PRNG (seeded from argc)
// so nothing folds; serials are runtime values by construction. Every
// scenario prints an order-sensitive FNV over the snapshot serial sequence
// plus a value sum, so output order and content are pinned run to run.

#include "wengine/core/runner.hpp"
#include "wengine/emit/routing.hpp"
#include "wengine/signal/lua.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/world/state.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using wengine::world::Entity;
using wengine::world::Signals;
using wengine::world::Snapshot;
using wengine::world::State;

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

template <typename T> inline void do_not_optimize(const T &v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(v) : "memory");
#else
  volatile const T *p = &v;
  (void)p;
#endif
}

using Clock = std::chrono::high_resolution_clock;

template <typename F> double time_ns(F &&f) {
  const auto t0 = Clock::now();
  f();
  const auto t1 = Clock::now();
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

struct CountSink : public wengine::emit::ISink {
  std::uint64_t calls = 0;
  std::uint64_t rows = 0;
  void on_snapshot(const Snapshot &s) override {
    ++calls;
    rows += static_cast<std::uint64_t>(s.signal_count());
  }
};

// Order-sensitive checksum over a snapshot's signal section: pins both
// content and slot order (serial sequence + value bits).
struct Checksum {
  std::uint64_t fnv = 1469598103934665603ULL;
  double sum = 0.0;
};

void mix_into(Checksum &c, const Snapshot &s) {
  for (std::size_t k = 0; k < s.signal_count(); ++k) {
    c.fnv ^= s.sig_serial[k];
    c.fnv *= 1099511628211ULL;
    c.fnv ^= std::bit_cast<std::uint64_t>(s.sig_value[k]);
    c.fnv *= 1099511628211ULL;
    c.sum += s.sig_value[k];
  }
  do_not_optimize(c.fnv);
  do_not_optimize(c.sum);
}

// Exact pool allocation footprint: every column capacity plus the script
// store. Deterministic — no allocator sampling needed.
std::size_t signals_bytes(const Signals &p) {
  std::size_t b = 0;
  b += p.serial.capacity() * sizeof(p.serial[0]);
  b += p.kind.capacity() * sizeof(p.kind[0]);
  b += p.quantity.capacity() * sizeof(p.quantity[0]);
  b += p.freq.capacity() * sizeof(p.freq[0]);
  b += p.amp.capacity() * sizeof(p.amp[0]);
  b += p.offset.capacity() * sizeof(p.offset[0]);
  b += p.phase.capacity() * sizeof(p.phase[0]);
  b += p.duty.capacity() * sizeof(p.duty[0]);
  b += p.seed.capacity() * sizeof(p.seed[0]);
  b += p.rng.capacity() * sizeof(p.rng[0]);
  b += p.t0.capacity() * sizeof(p.t0[0]);
  b += p.value.capacity() * sizeof(p.value[0]);
  b += p.running.capacity() * sizeof(p.running[0]);
  b += p.ops.capacity() * sizeof(p.ops[0]);
  b += p.op_count.capacity() * sizeof(p.op_count[0]);
  b += p.expr_op.capacity() * sizeof(p.expr_op[0]);
  b += p.expr_arg.capacity() * sizeof(p.expr_arg[0]);
  b += p.expr_len.capacity() * sizeof(p.expr_len[0]);
  b += p.table_off.capacity() * sizeof(p.table_off[0]);
  b += p.table_len.capacity() * sizeof(p.table_len[0]);
  b += p.table_times.capacity() * sizeof(p.table_times[0]);
  b += p.table_values.capacity() * sizeof(p.table_values[0]);
  b += p.lua_off.capacity() * sizeof(p.lua_off[0]);
  b += p.lua_len.capacity() * sizeof(p.lua_len[0]);
  b += p.present.capacity() * sizeof(p.present[0]);
  b += p.lua_text.capacity() * sizeof(p.lua_text[0]);
  return b;
}

std::size_t table_store_bytes(const Signals &p) {
  return (p.table_times.capacity() + p.table_values.capacity()) * sizeof(double);
}

// --- builders (twin builds from equal seeds are bit-identical) ---

bool add_sines(State &st, std::size_t n, std::uint32_t &rng,
               std::vector<std::uint64_t> &serials) {
  for (std::size_t k = 0; k < n; ++k) {
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kSine;
    p.freq_hz = 1.0 + static_cast<double>(xorshift(rng) % 20000) / 10.0;
    p.amp = 0.5 + static_cast<double>(xorshift(rng) % 5000) / 1000.0;
    p.phase_rad = static_cast<double>(xorshift(rng) % 6283) / 1000.0;
    const Entity e = st.spawn_entity();
    if (!wengine::world::configure_source(e, st.signals, p, 0.0).ok()) {
      return false;
    }
    serials.push_back(st.signals.serial[e.index]);
  }
  return true;
}

// Sparse world: live sources on strided slots, fillers despawned. Signal
// capacity stays at the high slot while live_count is `live` — the "one
// signal at a high slot grows every column" shape.
bool build_sparse(State &st, std::size_t live, std::size_t gap, std::uint32_t &rng,
                  std::vector<std::uint64_t> &serials) {
  const std::size_t cap = live * gap;
  std::vector<Entity> ents;
  ents.reserve(cap);
  for (std::size_t k = 0; k < cap; ++k) {
    ents.push_back(st.spawn_entity());
  }
  for (std::size_t k = 0; k < live; ++k) {
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kSine;
    p.freq_hz = 1.0 + static_cast<double>(xorshift(rng) % 20000) / 10.0;
    p.amp = 0.5 + static_cast<double>(xorshift(rng) % 5000) / 1000.0;
    p.phase_rad = static_cast<double>(xorshift(rng) % 6283) / 1000.0;
    const Entity e = ents[k * gap];
    if (!wengine::world::configure_source(e, st.signals, p, 0.0).ok()) {
      return false;
    }
    serials.push_back(st.signals.serial[e.index]);
  }
  for (std::size_t k = 0; k < cap; ++k) {
    if (k % gap != 0 && !st.despawn_entity(ents[k])) {
      return false;
    }
  }
  return st.signals.live_count == live;
}

bool add_expressions(State &st, std::size_t n, std::uint32_t &rng,
                     std::vector<Entity> &ents) {
  for (std::size_t k = 0; k < n; ++k) {
    const double c1 = 0.5 + static_cast<double>(xorshift(rng) % 5000) / 1000.0;
    const double c2 = static_cast<double>(xorshift(rng) % 4000) / 1000.0 - 2.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "sin(%f*t)+%f", c1, c2);
    const auto prog = wengine::signal::parse_expression(buf);
    if (!prog.ok()) {
      return false;
    }
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kExpression;
    p.expr = prog.value;
    const Entity e = st.spawn_entity();
    if (!wengine::world::configure_source(e, st.signals, p, 0.0).ok()) {
      return false;
    }
    ents.push_back(e);
  }
  return true;
}

bool add_tables(State &st, std::size_t n, std::uint32_t &rng,
                std::vector<Entity> &ents) {
  for (std::size_t k = 0; k < n; ++k) {
    std::vector<wengine::signal::TablePoint> pts;
    pts.reserve(8);
    for (std::size_t j = 0; j < 8; ++j) {
      const double v = static_cast<double>(xorshift(rng) % 20000) / 1000.0 - 10.0;
      pts.push_back({0.05 + static_cast<double>(j) * 0.25, v});
    }
    const Entity e = st.spawn_entity();
    if (!wengine::world::configure_table_source(
            e, st.signals, pts, wengine::signal::QuantityId::kVoltage, 0.0)
             .ok()) {
      return false;
    }
    ents.push_back(e);
  }
  return true;
}

bool add_luas(State &st, std::size_t n, std::uint32_t &rng,
              std::vector<Entity> &ents) {
  for (std::size_t k = 0; k < n; ++k) {
    const double c = 0.1 + static_cast<double>(xorshift(rng) % 4900) / 1000.0;
    const double d = static_cast<double>(xorshift(rng) % 4000) / 1000.0 - 2.0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "return %f*t+%f", c, d);
    const Entity e = st.spawn_entity();
    if (!wengine::lua::configure_lua_source(e, st.signals, buf,
                                            wengine::signal::QuantityId::kVoltage,
                                            0.0)
             .ok()) {
      return false;
    }
    ents.push_back(e);
  }
  return true;
}

bool add_sums(State &st, std::size_t n, std::size_t fanin,
              const std::vector<std::uint64_t> &gens) {
  if (gens.empty()) {
    return n == 0;
  }
  for (std::size_t s = 0; s < n; ++s) {
    wengine::signal::SignalParams p;
    p.kind = wengine::signal::SignalKind::kSum;
    p.op_count = static_cast<std::uint8_t>(fanin);
    for (std::size_t k = 0; k < fanin; ++k) {
      p.ops[k] = gens[(s * fanin + k) % gens.size()];
    }
    const Entity e = st.spawn_entity();
    if (!wengine::world::configure_source(e, st.signals, p, 0.0).ok()) {
      return false;
    }
  }
  return true;
}

std::uint64_t clamp_u64(std::uint64_t v, std::uint64_t lo, std::uint64_t hi,
                        const char *name) {
  const std::uint64_t c = v < lo ? lo : (v > hi ? hi : v);
  if (c != v) {
    std::cout << "bench_signal_scale: clamped " << name << " " << v << " -> " << c
              << "\n";
  }
  return c;
}

} // namespace

int main(int argc, char **argv) {
  std::uint64_t live = 256, gap = 16, steps = 2000, sums = 8, fanin = 8, subs = 4,
                customs = 8;
  if (argc > 1) {
    live = std::strtoull(argv[1], nullptr, 10);
  }
  if (argc > 2) {
    gap = std::strtoull(argv[2], nullptr, 10);
  }
  if (argc > 3) {
    steps = std::strtoull(argv[3], nullptr, 10);
  }
  if (argc > 4) {
    sums = std::strtoull(argv[4], nullptr, 10);
  }
  if (argc > 5) {
    fanin = std::strtoull(argv[5], nullptr, 10);
  }
  if (argc > 6) {
    subs = std::strtoull(argv[6], nullptr, 10);
  }
  if (argc > 7) {
    customs = std::strtoull(argv[7], nullptr, 10);
  }
  live = clamp_u64(live, 1, 4096, "live");
  gap = clamp_u64(gap, 1, 256, "gap");
  if (live * gap > 65536) {
    gap = 65536 / live;
    std::cout << "bench_signal_scale: clamped gap for live*gap <= 65536 -> " << gap
              << "\n";
  }
  steps = clamp_u64(steps, 1, 20000, "steps");
  sums = clamp_u64(sums, 0, 64, "sums");
  fanin = clamp_u64(fanin, 1, wengine::signal::SignalParams::kMaxOps, "fanin");
  subs = clamp_u64(subs, 0, 16, "subs");
  customs = clamp_u64(customs, 0, 32, "customs");
  std::cout << "bench_signal_scale: live=" << live << " gap=" << gap
            << " steps=" << steps << " sums=" << sums << " fanin=" << fanin
            << " subs=" << subs << " customs=" << customs << "\n";

  const std::uint32_t base =
      0x51ED270Bu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u + 0xC2B2AE35u);

  // --- dense-vs-sparse: equal live populations, different capacities ---
  double dense_step_ns = 0.0, sparse_step_ns = 0.0;
  std::size_t dense_cap = 0, sparse_cap = 0, dense_bytes = 0, sparse_bytes = 0;
  {
    std::uint32_t rng = base ^ 0xD15EA5E1u;
    State dense;
    std::vector<std::uint64_t> serials;
    if (!add_sines(dense, static_cast<std::size_t>(live), rng, serials)) {
      std::cout << "bench_signal_scale: dense setup failed\n";
      return 1;
    }
    dense_cap = dense.signals.capacity();
    dense_bytes = signals_bytes(dense.signals);
    wengine::core::Runner runner{{.dt = 0.001}};
    wengine::solvers::SignalSolver solver;
    runner.add_solver(solver);
    runner.state() = dense;
    if (!runner.run(5).ok()) {
      std::cout << "bench_signal_scale: dense warmup failed\n";
      return 1;
    }
    dense_step_ns = time_ns([&] {
      if (!runner.run(steps).ok()) {
        std::cout << "bench_signal_scale: dense run failed\n";
        std::exit(1);
      }
    }) / static_cast<double>(steps);
    Checksum c;
    mix_into(c, wengine::world::take_snapshot(runner.state()));
    Snapshot buf;
    wengine::world::fill_snapshot(runner.state(), buf);
    const std::uint64_t captures = 2000;
    const double dense_cap_ns = time_ns([&] {
      for (std::uint64_t k = 0; k < captures; ++k) {
        runner.state().sim_time += 0.001;
        runner.state().step_index += 1;
        wengine::world::fill_snapshot(runner.state(), buf);
      }
    }) / static_cast<double>(captures);
    mix_into(c, buf);
    std::cout << "bench_signal_scale dense-vs-sparse/dense: live=" << live
              << " cap=" << dense_cap << " bytes=" << dense_bytes << " ("
              << dense_bytes / static_cast<std::size_t>(live) << "/live) "
              << dense_step_ns << " ns/step " << dense_cap_ns
              << " ns/capture (fnv=" << c.fnv << " sum=" << c.sum << ")\n";

    rng = base ^ 0xD15EA5E1u; // twin seed: identical live params
    State sparse;
    std::vector<std::uint64_t> sserials;
    if (!build_sparse(sparse, static_cast<std::size_t>(live),
                      static_cast<std::size_t>(gap), rng, sserials)) {
      std::cout << "bench_signal_scale: sparse setup failed\n";
      return 1;
    }
    sparse_cap = sparse.signals.capacity();
    sparse_bytes = signals_bytes(sparse.signals);
    wengine::core::Runner srunner{{.dt = 0.001}};
    wengine::solvers::SignalSolver ssolver;
    srunner.add_solver(ssolver);
    srunner.state() = sparse;
    if (!srunner.run(5).ok()) {
      std::cout << "bench_signal_scale: sparse warmup failed\n";
      return 1;
    }
    sparse_step_ns = time_ns([&] {
      if (!srunner.run(steps).ok()) {
        std::cout << "bench_signal_scale: sparse run failed\n";
        std::exit(1);
      }
    }) / static_cast<double>(steps);
    Checksum sc;
    mix_into(sc, wengine::world::take_snapshot(srunner.state()));
    Snapshot sbuf;
    wengine::world::fill_snapshot(srunner.state(), sbuf);
    const double sparse_cap_ns = time_ns([&] {
      for (std::uint64_t k = 0; k < captures; ++k) {
        srunner.state().sim_time += 0.001;
        srunner.state().step_index += 1;
        wengine::world::fill_snapshot(srunner.state(), sbuf);
      }
    }) / static_cast<double>(captures);
    mix_into(sc, sbuf);
    std::cout << "bench_signal_scale dense-vs-sparse/sparse: live=" << live
              << " cap=" << sparse_cap << " bytes=" << sparse_bytes << " ("
              << sparse_bytes / static_cast<std::size_t>(live) << "/live) "
              << sparse_step_ns << " ns/step " << sparse_cap_ns
              << " ns/capture (fnv=" << sc.fnv << " sum=" << sc.sum << ")\n";
    if (c.fnv != sc.fnv) {
      // Twin seeds make the live populations identical: same order, same
      // values. A mismatch means the sparse shape changed live semantics.
      std::cout << "bench_signal_scale: dense/sparse checksum mismatch\n";
      return 1;
    }
  }

  // --- lookup: direct find_serial sweep over capacities ---
  double lookup_slope = 0.0, lookup_intercept = 0.0;
  {
    const std::uint64_t gaps[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    double xs[9] = {};
    double ys[9] = {};
    std::size_t points = 0;
    for (std::uint64_t g : gaps) {
      if (g != 1 && (g > gap * 4 || live * g > 65536)) {
        continue; // keep the sweep near the configured shape, bounded
      }
      std::uint32_t rng = base ^ 0xF1D5EEDu;
      State st;
      std::vector<std::uint64_t> serials;
      if (g == 1) {
        if (!add_sines(st, static_cast<std::size_t>(live), rng, serials)) {
          std::cout << "bench_signal_scale: lookup setup failed\n";
          return 1;
        }
      } else {
        if (!build_sparse(st, static_cast<std::size_t>(live),
                          static_cast<std::size_t>(g), rng, serials)) {
          std::cout << "bench_signal_scale: lookup setup failed\n";
          return 1;
        }
      }
      // Opaque query order: PRNG shuffle of the live serials.
      for (std::size_t k = serials.size(); k > 1; --k) {
        const std::size_t j =
            static_cast<std::size_t>(xorshift(rng) % static_cast<std::uint32_t>(k));
        const std::uint64_t t = serials[k - 1];
        serials[k - 1] = serials[j];
        serials[j] = t;
      }
      const std::size_t cap = st.signals.capacity();
      std::uint64_t reps =
          20000000ULL / (static_cast<std::uint64_t>(serials.size() * cap) + 1) + 1;
      if (reps > 100000) {
        reps = 100000;
      }
      std::uint64_t hits = 0;
      const double ns = time_ns([&] {
        for (std::uint64_t r = 0; r < reps; ++r) {
          for (std::uint64_t q : serials) {
            hits += wengine::world::find_serial(st.signals, q).has_value() ? 1 : 0;
          }
        }
      });
      do_not_optimize(hits);
      if (hits != reps * serials.size()) {
        std::cout << "bench_signal_scale: lookup lost live serials\n";
        return 1;
      }
      const double per =
          ns / (static_cast<double>(reps) * static_cast<double>(serials.size()));
      xs[points] = static_cast<double>(cap);
      ys[points] = per;
      ++points;
      std::cout << "bench_signal_scale lookup: cap=" << cap << " live=" << live
                << " " << per << " ns/lookup\n";
    }
    // Least-squares slope (ns/slot) + intercept (fixed cost).
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t k = 0; k < points; ++k) {
      sx += xs[k];
      sy += ys[k];
      sxx += xs[k] * xs[k];
      sxy += xs[k] * ys[k];
    }
    const double n = static_cast<double>(points);
    const double denom = n * sxx - sx * sx;
    if (points >= 3 && denom > 0) {
      lookup_slope = (n * sxy - sx * sy) / denom;
      lookup_intercept = (sy - lookup_slope * sx) / n;
    } else if (points > 0) {
      lookup_slope = 0.0;
      lookup_intercept = sy / n;
    }
  }

  // --- fanin: K sums over the sparse generators, fan-in sweep ---
  double fanin_slope = 0.0;
  {
    const std::size_t fs[] = {1, 2, 4, 8};
    double sfx = 0, sfy = 0, sfxx = 0, sfxy = 0, sfn = 0; // LSQ over points
    for (std::size_t f : fs) {
      if (f > static_cast<std::size_t>(fanin)) {
        continue;
      }
      std::uint32_t rng = base ^ 0xFAA4EEDu;
      State st;
      std::vector<std::uint64_t> gens;
      if (!build_sparse(st, static_cast<std::size_t>(live),
                        static_cast<std::size_t>(gap), rng, gens)) {
        std::cout << "bench_signal_scale: fanin setup failed\n";
        return 1;
      }
      if (!add_sums(st, static_cast<std::size_t>(sums), f, gens)) {
        std::cout << "bench_signal_scale: fanin sums failed\n";
        return 1;
      }
      wengine::core::Runner runner{{.dt = 0.001}};
      wengine::solvers::SignalSolver solver;
      runner.add_solver(solver);
      runner.state() = st;
      if (!runner.run(5).ok()) {
        std::cout << "bench_signal_scale: fanin warmup failed\n";
        return 1;
      }
      // Visit-budgeted horizon: small worlds average more steps so the
      // per-operand slope stays above timer/scheduling noise.
      const std::size_t cap = runner.state().signals.capacity();
      std::uint64_t fsteps = 4000000ULL / (static_cast<std::uint64_t>(cap) + 1) + 10;
      if (fsteps > 1000) {
        fsteps = 1000;
      }
      const double ns = time_ns([&] {
        if (!runner.run(fsteps).ok()) {
          std::cout << "bench_signal_scale: fanin run failed\n";
          std::exit(1);
        }
      }) / static_cast<double>(fsteps);
      Checksum c;
      mix_into(c, wengine::world::take_snapshot(runner.state()));
      std::cout << "bench_signal_scale fanin: sums=" << sums << " fanin=" << f
                << " cap=" << cap << " " << ns << " ns/step (fnv=" << c.fnv
                << " sum=" << c.sum << ")\n";
      sfx += static_cast<double>(f);
      sfy += ns;
      sfxx += static_cast<double>(f * f);
      sfxy += static_cast<double>(f) * ns;
      sfn += 1.0;
    }
    const double fden = sfn * sfxx - sfx * sfx;
    if (sfn >= 2.0 && fden > 0 && sums != 0) {
      fanin_slope = (sfn * sfxy - sfx * sfy) / fden / static_cast<double>(sums);
    }
  }

  // --- subs: S subset subscribers partitioning one snapshot ---
  {
    if (subs == 0) {
      std::cout << "bench_signal_scale subs: skipped (subs=0)\n";
    } else {
      std::uint32_t rng = base ^ 0x5EED5EEDu;
      State st;
      std::vector<std::uint64_t> serials;
      if (!build_sparse(st, static_cast<std::size_t>(live),
                        static_cast<std::size_t>(gap), rng, serials)) {
        std::cout << "bench_signal_scale: subs setup failed\n";
        return 1;
      }
      wengine::core::Runner runner{{.dt = 0.001}};
      wengine::solvers::SignalSolver solver;
      runner.add_solver(solver);
      runner.state() = st;
      if (!runner.run(10).ok()) {
        std::cout << "bench_signal_scale: subs warmup failed\n";
        return 1;
      }
      const Snapshot snap = wengine::world::take_snapshot(runner.state());
      const std::size_t m = snap.signal_count();
      std::vector<CountSink> inners(static_cast<std::size_t>(subs));
      std::vector<wengine::emit::SignalSubsetSink> routers;
      routers.reserve(static_cast<std::size_t>(subs));
      for (std::size_t s = 0; s < static_cast<std::size_t>(subs); ++s) {
        routers.emplace_back(inners[s]);
        std::vector<std::uint64_t> list;
        for (std::size_t k = s; k < serials.size();
             k += static_cast<std::size_t>(subs)) {
          list.push_back(serials[k]);
        }
        routers.back().set_subscription(std::move(list));
      }
      const std::uint64_t captures = 2000;
      const double ns = time_ns([&] {
        for (std::uint64_t k = 0; k < captures; ++k) {
          for (auto &r : routers) {
            r.on_snapshot(snap);
          }
        }
      });
      std::uint64_t rows = 0;
      for (const auto &in : inners) {
        rows += in.rows;
      }
      if (rows != static_cast<std::uint64_t>(m) * captures) {
        std::cout << "bench_signal_scale: subs lost rows (" << rows << ")\n";
        return 1;
      }
      const double pairs =
          static_cast<double>(captures) * static_cast<double>(m) *
          static_cast<double>(serials.size()); // rows x serials scanned
      std::cout << "bench_signal_scale subs: subs=" << subs << " rows=" << m
                << " x " << captures << " captures "
                << ns / static_cast<double>(captures) << " ns/tick "
                << ns / pairs << " ns/pair (rows=" << rows << ")\n";
    }
  }

  // --- churn: repeated custom-source add/step/drop; stores never compact ---
  std::size_t churn_table_bytes = 0, churn_lua_bytes = 0;
  double purge_ns = 0.0, steady_ns = 0.0;
  {
    const std::size_t k = static_cast<std::size_t>(customs);
    const std::uint64_t rounds = 8;
    std::uint32_t rng = base ^ 0xC444EDu;
    State st;
    std::vector<std::uint64_t> serials;
    if (!build_sparse(st, static_cast<std::size_t>(live),
                      static_cast<std::size_t>(gap), rng, serials)) {
      std::cout << "bench_signal_scale: churn setup failed\n";
      return 1;
    }
    auto lua = wengine::lua::create_lua_solver();
    wengine::core::Runner runner{{.dt = 0.001}};
    wengine::solvers::SignalSolver solver;
    runner.add_solver(*lua);
    runner.add_solver(solver);
    runner.state() = st;
    for (std::uint64_t r = 0; r < rounds; ++r) {
      std::vector<Entity> added;
      if (!add_expressions(runner.state(), k, rng, added) ||
          !add_tables(runner.state(), k, rng, added) ||
          !add_luas(runner.state(), k, rng, added)) {
        std::cout << "bench_signal_scale: churn add failed\n";
        return 1;
      }
      if (!runner.run(3).ok()) {
        std::cout << "bench_signal_scale: churn run failed\n";
        return 1;
      }
      for (const Entity e : added) {
        if (!runner.state().despawn_entity(e)) {
          std::cout << "bench_signal_scale: churn drop failed\n";
          return 1;
        }
      }
    }
    churn_table_bytes = table_store_bytes(runner.state().signals);
    churn_lua_bytes = runner.state().signals.lua_text.size();
    Checksum c;
    mix_into(c, wengine::world::take_snapshot(runner.state()));
    std::cout << "bench_signal_scale churn: rounds=" << rounds << " customs/round="
              << 3 * k << " live=" << runner.state().signals.live_count
              << " table_store=" << churn_table_bytes << " lua_blob="
              << churn_lua_bytes << " retained/round="
              << (churn_table_bytes + churn_lua_bytes) /
                     static_cast<std::size_t>(rounds)
              << " B (fnv=" << c.fnv << " sum=" << c.sum << ")\n";
    // Lua dead-cache purge: the first step drops 3*k dead serials (one
    // find_serial per cached serial); later steps are steady.
    purge_ns = time_ns([&] {
      if (lua->step(runner.state(), 0.001).ok()) {
      } else {
        std::cout << "bench_signal_scale: churn purge step failed\n";
        std::exit(1);
      }
    });
    steady_ns = time_ns([&] {
      for (int t = 0; t < 30; ++t) {
        if (!lua->step(runner.state(), 0.001).ok()) {
          std::cout << "bench_signal_scale: churn steady step failed\n";
          std::exit(1);
        }
      }
    }) / 30.0;
    // Reference: same shape, customs never existed (no cache, empty stores).
    std::uint32_t rrng = base ^ 0xC444EDu;
    State ref;
    std::vector<std::uint64_t> rserials;
    if (!build_sparse(ref, static_cast<std::size_t>(live),
                      static_cast<std::size_t>(gap), rrng, rserials)) {
      std::cout << "bench_signal_scale: churn ref setup failed\n";
      return 1;
    }
    auto lua2 = wengine::lua::create_lua_solver();
    const double ref_ns = time_ns([&] {
      for (int t = 0; t < 30; ++t) {
        if (!lua2->step(ref, 0.001).ok()) {
          std::cout << "bench_signal_scale: churn ref step failed\n";
          std::exit(1);
        }
      }
    }) / 30.0;
    std::cout << "bench_signal_scale churn/lua-cache: purge=" << purge_ns
              << " ns steady=" << steady_ns << " ns ref=" << ref_ns << " ns\n";
    // Serialization sees the retained stores: one measured round-trip.
    std::ostringstream out;
    const double save_ns = time_ns([&] {
      if (!wengine::world::save_state(runner.state(), out).ok()) {
        std::cout << "bench_signal_scale: churn save failed\n";
        std::exit(1);
      }
    });
    const std::string blob = out.str();
    State loaded0;
    {
      std::istringstream lin(blob);
      if (!wengine::world::load_state(loaded0, lin).ok()) {
        std::cout << "bench_signal_scale: churn load failed\n";
        return 1;
      }
    }
    const double load_ns = time_ns([&] {
      std::istringstream lin(blob);
      State reloaded;
      if (!wengine::world::load_state(reloaded, lin).ok()) {
        std::cout << "bench_signal_scale: churn load failed\n";
        std::exit(1);
      }
      do_not_optimize(reloaded.signals.live_count);
    });
    if (!(loaded0 == runner.state())) {
      std::cout << "bench_signal_scale: churn round-trip mismatch\n";
      return 1;
    }
    std::cout << "bench_signal_scale churn/serial: blob=" << blob.size()
              << " B save=" << save_ns << " ns load=" << load_ns << " ns\n";
  }

  // --- separate: native / Lua / snapshot / filter / serialize on one mix ---
  {
    const std::size_t k = static_cast<std::size_t>(customs);
    const std::uint64_t short_steps = steps > 200 ? 200 : steps;
    std::uint32_t rng = base ^ 0xE17AEDu;
    State mix;
    std::vector<std::uint64_t> gens;
    std::vector<Entity> tmp;
    if (!build_sparse(mix, static_cast<std::size_t>(live),
                      static_cast<std::size_t>(gap), rng, gens) ||
        !add_expressions(mix, k, rng, tmp) || !add_tables(mix, k, rng, tmp) ||
        !add_luas(mix, k, rng, tmp) ||
        !add_sums(mix, static_cast<std::size_t>(sums),
                  static_cast<std::size_t>(fanin), gens)) {
      std::cout << "bench_signal_scale: mixed setup failed\n";
      return 1;
    }
    // Native solver over the full horizon (kLua slots skipped in-solver).
    wengine::core::Runner native{{.dt = 0.001}};
    wengine::solvers::SignalSolver nsolver;
    native.add_solver(nsolver);
    native.state() = mix;
    if (!native.run(5).ok()) {
      std::cout << "bench_signal_scale: native warmup failed\n";
      return 1;
    }
    const double native_ns = time_ns([&] {
      if (!native.run(steps).ok()) {
        std::cout << "bench_signal_scale: native run failed\n";
        std::exit(1);
      }
    }) / static_cast<double>(steps);
    Checksum nc;
    mix_into(nc, wengine::world::take_snapshot(native.state()));
    // Short-horizon twins: native-only vs lua+signal isolates Lua cost.
    std::uint32_t rng2 = base ^ 0xE17AEDu;
    State twin;
    std::vector<std::uint64_t> gens2;
    std::vector<Entity> tmp2;
    if (!build_sparse(twin, static_cast<std::size_t>(live),
                      static_cast<std::size_t>(gap), rng2, gens2) ||
        !add_expressions(twin, k, rng2, tmp2) || !add_tables(twin, k, rng2, tmp2) ||
        !add_luas(twin, k, rng2, tmp2) ||
        !add_sums(twin, static_cast<std::size_t>(sums),
                  static_cast<std::size_t>(fanin), gens2)) {
      std::cout << "bench_signal_scale: twin setup failed\n";
      return 1;
    }
    wengine::core::Runner nshort{{.dt = 0.001}};
    wengine::solvers::SignalSolver nssolver;
    nshort.add_solver(nssolver);
    nshort.state() = twin;
    const double nshort_ns = time_ns([&] {
      if (!nshort.run(short_steps).ok()) {
        std::cout << "bench_signal_scale: twin native run failed\n";
        std::exit(1);
      }
    }) / static_cast<double>(short_steps);
    auto lua = wengine::lua::create_lua_solver();
    wengine::core::Runner full{{.dt = 0.001}};
    wengine::solvers::SignalSolver fsolver;
    full.add_solver(*lua);
    full.add_solver(fsolver);
    full.state() = twin; // twin worlds are bit-identical builds
    const double full_ns = time_ns([&] {
      if (!full.run(short_steps).ok()) {
        std::cout << "bench_signal_scale: full run failed\n";
        std::exit(1);
      }
    }) / static_cast<double>(short_steps);
    Checksum fc;
    mix_into(fc, wengine::world::take_snapshot(full.state()));
    const double lua_ns =
        k == 0 ? 0.0 : (full_ns - nshort_ns) / static_cast<double>(k);
    std::cout << "bench_signal_scale separate/native: " << native_ns
              << " ns/step (fnv=" << nc.fnv << " sum=" << nc.sum << ")\n";
    std::cout << "bench_signal_scale separate/lua: full=" << full_ns
              << " ns/step native-short=" << nshort_ns << " ns/step lua~" << lua_ns
              << " ns/step/source x " << k << " (fnv=" << fc.fnv
              << " sum=" << fc.sum << ")\n";
    // Snapshot + filter on the mixed state.
    Snapshot buf;
    wengine::world::fill_snapshot(full.state(), buf);
    const std::uint64_t captures = 5000;
    const double snap_ns = time_ns([&] {
      for (std::uint64_t t = 0; t < captures; ++t) {
        full.state().sim_time += 0.001;
        full.state().step_index += 1;
        wengine::world::fill_snapshot(full.state(), buf);
      }
    }) / static_cast<double>(captures);
    mix_into(fc, buf);
    std::vector<std::uint64_t> half(gens2.begin(),
                                    gens2.begin() + gens2.size() / 2);
    Snapshot felt;
    const std::uint64_t fcaptures = 2000;
    const double felt_ns = time_ns([&] {
      for (std::uint64_t t = 0; t < fcaptures; ++t) {
        wengine::world::select_signals_into(buf, half, felt);
      }
    }) / static_cast<double>(fcaptures);
    Checksum fltc;
    mix_into(fltc, felt);
    std::cout << "bench_signal_scale separate/snapshot: " << snap_ns
              << " ns/capture rows=" << buf.signal_count() << "\n";
    std::cout << "bench_signal_scale separate/filter: " << felt_ns
              << " ns/filter rows=" << felt.signal_count() << " (fnv=" << fltc.fnv
              << " sum=" << fltc.sum << ")\n";
    // Serialization round-trip (equality = order + content preserved).
    std::ostringstream out;
    if (!wengine::world::save_state(full.state(), out).ok()) {
      std::cout << "bench_signal_scale: mixed save failed\n";
      return 1;
    }
    const std::string blob = out.str();
    const double save10_ns = time_ns([&] {
      for (int t = 0; t < 10; ++t) {
        std::ostringstream o;
        if (!wengine::world::save_state(full.state(), o).ok()) {
          std::cout << "bench_signal_scale: mixed save failed\n";
          std::exit(1);
        }
      }
    }) / 10.0;
    const double load10_ns = time_ns([&] {
      for (int t = 0; t < 10; ++t) {
        std::istringstream lin(blob);
        State loaded;
        if (!wengine::world::load_state(loaded, lin).ok()) {
          std::cout << "bench_signal_scale: mixed load failed\n";
          std::exit(1);
        }
        do_not_optimize(loaded.signals.live_count);
      }
    }) / 10.0;
    std::istringstream lin(blob);
    State loaded;
    if (!wengine::world::load_state(loaded, lin).ok() || !(loaded == full.state())) {
      std::cout << "bench_signal_scale: mixed round-trip mismatch\n";
      return 1;
    }
    std::cout << "bench_signal_scale separate/serial: blob=" << blob.size()
              << " B save=" << save10_ns << " ns load=" << load10_ns << " ns\n";
  }

  // --- crossover: measured points + reading guide ---
  {
    const double dcap = static_cast<double>(dense_cap);
    const double scap = static_cast<double>(sparse_cap);
    const double scan =
        (scap > dcap) ? (sparse_step_ns - dense_step_ns) / (scap - dcap) : 0.0;
    const double eval =
        (dense_step_ns - scan * dcap) / static_cast<double>(live);
    const double gap_star = (scan > 0 && eval > 0) ? eval / scan : 0.0;
    const double cap_at_1us = (lookup_slope > 0) ? 1000.0 / lookup_slope : 0.0;
    std::cout << "bench_signal_scale crossover: scan=" << scan
              << " ns/slot/step eval=" << eval
              << " ns/source/step gap*=" << gap_star
              << " (scan==eval per live source)\n";
    std::cout << "bench_signal_scale crossover: lookup_slope=" << lookup_slope
              << " ns/slot intercept=" << lookup_intercept
              << " ns cap@1us=" << cap_at_1us << "\n";
    std::cout << "bench_signal_scale crossover: fanin_slope=" << fanin_slope
              << " ns/operand/step (== one find_serial over the live capacity)\n";
    std::cout << "bench_signal_scale crossover: churn_retained="
              << churn_table_bytes + churn_lua_bytes
              << " B purge=" << purge_ns << " ns steady=" << steady_ns << " ns\n";
    std::cout << "bench_signal_scale guide: gap* small -> active-slot iteration; "
                 "lookup/fanin slope dominant -> serial index; bytes/live ~758B+ "
                 "-> split cold program columns; retained/round > 0 at 0 live -> "
                 "table/lua store compaction\n";
  }

  return 0;
}
