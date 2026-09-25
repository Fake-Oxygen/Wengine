// End-to-end goodput/latency benchmark for the wengine-link SHM bus
// (PHYSI-29). Covers solver-only, snapshot-only, flood with an ACTIVE
// concurrent reader, paced 1 kHz with an active reader, stalled (late
// attach) reader, and fan-out — for one-signal and representative
// particle payloads.
//
// Delivery vocabulary follows docs/delivery.md: generated (Runner emit
// steps) vs. published vs. reader received/missed/max_gap vs. incomplete
// tail (published but never observed). Delivery age is recv_time -
// send_time per frame_seq; p50/p99 come from the received set. CPU is
// process user+sys seconds around each scenario (getrusage). No timing
// thresholds are asserted — this binary reports, unit tests pin
// correctness separately.
//
// Repro:
//   bench_link [--payload signal|particle] [--particles N] [--steps N]
//              [--mode all|solo|snap|flood|paced|stalled|fanout]
//              [--rate HZ]
//   bench_link [particles] [steps]   (legacy: particle flood+stalled)
// Compiler/settings print on every run; each scenario prints one
// `bench_link <scenario> ...` line plus a human `slow reader` line for
// the stalled case.

#include "wengine/core/runner.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/signals.hpp"
#include "wengine/world/state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

template <typename T> inline void do_not_optimize(const T &v) {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(v) : "memory");
#else
  volatile const T *p = &v;
  (void)p;
#endif
}

std::uint32_t xorshift(std::uint32_t &s) noexcept {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

using Clock = std::chrono::steady_clock;

double cpu_seconds() {
  rusage ru{};
  ::getrusage(RUSAGE_SELF, &ru);
  const double user = static_cast<double>(ru.ru_utime.tv_sec) +
                      static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
  const double sys = static_cast<double>(ru.ru_stime.tv_sec) +
                     static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
  return user + sys;
}

std::string compiler_string() {
#if defined(__VERSION__)
  return __VERSION__;
#else
  return "unknown-compiler";
#endif
}

std::string opt_string() {
#if defined(__OPTIMIZE__)
  return "optimized";
#else
  return "unoptimized";
#endif
}

double percentile_sorted(const std::vector<double> &sorted, double q) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(pos);
  const std::size_t hi = lo + 1 < sorted.size() ? lo + 1 : lo;
  const double frac = pos - static_cast<double>(lo);
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// Non-blocking send-timestamp table: index by frame_seq (== step_index at
// emit_every=1). Writer stores before publish, reader loads after observing
// the seq. Relaxed atomics suffice: the SHM seqlock publish/poll is the
// release/acquire edge, the table itself never tears.
struct SendTable {
  explicit SendTable(std::size_t n) : ns(n) {
    for (auto &a : ns) {
      a.store(0, std::memory_order_relaxed);
    }
  }
  std::vector<std::atomic<std::int64_t>> ns;
};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

struct CountSink : public wengine::emit::ISink {
  std::uint64_t n = 0;
  void on_snapshot(const wengine::world::Snapshot &) override { ++n; }
};

// Timed SHM publisher: records a send timestamp per emitted seq, then
// publishes. Owns the ShmPublisher; the benchmark joins its reader thread
// before this object dies (stop/join before close).
class TimedPublisher : public wengine::emit::ISink {
public:
  TimedPublisher(std::string seg, std::string domain,
                 wengine::link::FrameAdapter adapter, SendTable &table)
      : table_(table), pub_(seg, domain, adapter), seg_(std::move(seg)) {
    ok_ = pub_.ok();
  }
  void on_snapshot(const wengine::world::Snapshot &s) override {
    const std::uint64_t seq = s.step_index;
    if (seq < table_.ns.size()) {
      table_.ns[static_cast<std::size_t>(seq)].store(now_ns(),
                                                     std::memory_order_relaxed);
    }
    pub_.on_snapshot(s);
  }
  [[nodiscard]] bool ok() const noexcept { return ok_ && pub_.ok(); }
  [[nodiscard]] const wengine::link::PublisherStats &stats() const noexcept {
    return pub_.stats();
  }
  [[nodiscard]] const std::string &seg() const noexcept { return seg_; }

private:
  SendTable &table_;
  wengine::link::ShmPublisher pub_;
  std::string seg_;
  bool ok_ = false;
};

struct ReaderResult {
  wengine::link::LagStats lag{};
  std::uint64_t last_seq = 0;
  double checksum = 0.0;
  std::uint64_t decoded = 0;
  std::uint64_t bad_decode = 0;
  std::vector<double> ages_us;
};

// Concurrent SHM reader: polls for frames strictly newer than its cursor,
// decodes every frame (validates complete framing + values), and records
// per-seq delivery age against the shared SendTable.
void reader_loop(const std::string &seg, SendTable &table,
                 std::atomic<bool> &stop, ReaderResult &out) {
  wengine::link::ShmReader reader(seg);
  if (!reader.ok()) {
    return;
  }
  std::uint64_t cursor = 0;
  wengine::link::LinkFrame wire{};
  wengine::link::Frame frame;
  while (!stop.load(std::memory_order_relaxed)) {
    if (reader.poll_newer(cursor, wire)) {
      (void)wengine::link::lag_observe(out.lag, wire.frame_seq);
      out.last_seq = wire.frame_seq;
      const std::int64_t recv = now_ns();
      if (!wengine::link::decode_frame(wire, frame).ok()) {
        ++out.bad_decode;
        continue;
      }
      ++out.decoded;
      for (const auto &it : frame.items) {
        out.checksum += it.x + it.y + it.vx + it.vy;
      }
      for (const auto &c : frame.channels) {
        out.checksum += c.value;
      }
      if (wire.frame_seq < table.ns.size()) {
        const std::int64_t sent =
            table.ns[static_cast<std::size_t>(wire.frame_seq)].load(
                std::memory_order_relaxed);
        if (sent != 0) {
          out.ages_us.push_back(static_cast<double>(recv - sent) / 1000.0);
        }
      }
      continue;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  // Drain once more after stop so the tail is not artificially cut.
  while (reader.poll_newer(cursor, wire)) {
    (void)wengine::link::lag_observe(out.lag, wire.frame_seq);
    out.last_seq = wire.frame_seq;
    const std::int64_t recv = now_ns();
    if (!wengine::link::decode_frame(wire, frame).ok()) {
      ++out.bad_decode;
      continue;
    }
    ++out.decoded;
    for (const auto &it : frame.items) {
      out.checksum += it.x + it.y + it.vx + it.vy;
    }
    for (const auto &c : frame.channels) {
      out.checksum += c.value;
    }
    if (wire.frame_seq < table.ns.size()) {
      const std::int64_t sent =
          table.ns[static_cast<std::size_t>(wire.frame_seq)].load(
              std::memory_order_relaxed);
      if (sent != 0) {
        out.ages_us.push_back(static_cast<double>(recv - sent) / 1000.0);
      }
    }
  }
}

struct Payload {
  std::string name = "particle";
  std::size_t particles = 64;
  bool is_signal = false;
};

void setup_world(wengine::core::Runner &runner, const Payload &p,
                 std::uint32_t &rng) {
  if (p.is_signal) {
    return;
  }
  for (std::size_t k = 0; k < p.particles; ++k) {
    wengine::world::Entity e = runner.state().spawn_entity();
    runner.state().particles.attach(e.index);
    runner.state().particles.pos_x[e.index] =
        -10.0 + static_cast<double>(xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.pos_y[e.index] =
        -10.0 + static_cast<double>(xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.vel_x[e.index] =
        -10.0 + static_cast<double>(xorshift(rng) % 20000) / 1000.0;
    runner.state().particles.vel_y[e.index] =
        -10.0 + static_cast<double>(xorshift(rng) % 20000) / 1000.0;
  }
}

bool setup_signal(wengine::core::Runner &runner, std::uint32_t &rng) {
  wengine::signal::SignalParams params;
  params.kind = wengine::signal::SignalKind::kSine;
  params.quantity = wengine::signal::QuantityId::kVoltage;
  params.freq_hz = 10.0 + static_cast<double>(xorshift(rng) % 10000) / 10.0;
  params.amp = 0.5 + static_cast<double>(xorshift(rng) % 5000) / 1000.0;
  params.phase_rad = static_cast<double>(xorshift(rng) % 6283) / 1000.0;
  wengine::world::Entity e = runner.state().spawn_entity();
  return wengine::world::configure_source(e, runner.state().signals, params,
                                          0.0)
      .ok();
}

void print_header(std::string_view repro) {
  std::cout << "bench_link: compiler=\"" << compiler_string() << "\" opt="
            << opt_string() << " link_frame_bytes=" << sizeof(wengine::link::LinkFrame)
            << " version=" << wengine::link::kVersion
#ifdef NDEBUG
            << " ndebug=1"
#else
            << " ndebug=0"
#endif
            << "\nbench_link: repro: " << repro << "\n";
}

void print_row(std::string_view scenario, const Payload &p,
               std::uint64_t steps, double wall_s, double cpu_s,
               std::uint64_t published, std::uint64_t dropped_bound,
               const ReaderResult &r, std::uint64_t bytes_per_frame) {
  std::vector<double> ages = r.ages_us;
  std::sort(ages.begin(), ages.end());
  const double p50 = percentile_sorted(ages, 0.50);
  const double p99 = percentile_sorted(ages, 0.99);
  const double attempted = wall_s > 0.0 ? static_cast<double>(steps) / wall_s : 0.0;
  const double goodput =
      wall_s > 0.0 ? static_cast<double>(r.lag.received) / wall_s : 0.0;
  const std::uint64_t tail =
      published > r.last_seq ? published - r.last_seq : 0;
  const double cpu_pct =
      wall_s > 0.0 ? 100.0 * cpu_s / wall_s : 0.0;
  const std::uint64_t bytes =
      r.lag.received * bytes_per_frame;
  std::cout << "bench_link " << scenario << ": payload=" << p.name
            << " steps=" << steps << " attempted=" << attempted << "/s"
            << " goodput=" << goodput << "/s"
            << " published=" << published
            << " dropped_over_bound=" << dropped_bound
            << " received=" << r.lag.received << " missed=" << r.lag.missed
            << " max_gap=" << r.lag.max_gap << " tail=" << tail
            << " decoded=" << r.decoded << " bad=" << r.bad_decode
            << " bytes=" << bytes << " p50_age_us=" << p50
            << " p99_age_us=" << p99 << " cpu_s=" << cpu_s
            << " cpu_pct=" << cpu_pct << " checksum=" << r.checksum << "\n";
  do_not_optimize(r.checksum);
}

int run_solo(const Payload &p, std::uint64_t steps, std::uint32_t rng) {
  // Solver-only: no sinks, so no snapshot/transport cost.
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver proj;
  wengine::solvers::SignalSolver sig;
  if (p.is_signal) {
    runner.add_solver(sig);
    if (!setup_signal(runner, rng)) {
      std::cout << "bench_link solo: signal setup failed\n";
      return 1;
    }
  } else {
    runner.add_solver(proj);
    setup_world(runner, p, rng);
  }
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  if (!runner.run(steps).ok()) {
    std::cout << "bench_link solo: run failed\n";
    return 1;
  }
  const auto t1 = Clock::now();
  const double wall =
      std::chrono::duration<double>(t1 - t0).count();
  const double cpu = cpu_seconds() - c0;
  const double rate = wall > 0.0 ? static_cast<double>(steps) / wall : 0.0;
  wengine::world::Snapshot snap = wengine::world::take_snapshot(runner.state());
  double checksum = 0.0;
  for (double v : snap.pos_x) {
    checksum += v;
  }
  for (double v : snap.sig_value) {
    checksum += v;
  }
  do_not_optimize(checksum);
  std::cout << "bench_link solo: payload=" << p.name << " steps=" << steps
            << " solver_rate=" << rate << "/s cpu_s=" << cpu
            << " checksum=" << checksum << "\n";
  return 0;
}

int run_snap(const Payload &p, std::uint64_t steps, std::uint32_t rng) {
  // Snapshot-only: solver + one counting observer (Runner still snapshots
  // on every emit) plus a separate adapt+encode loop to localize cost.
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver proj;
  wengine::solvers::SignalSolver sig;
  if (p.is_signal) {
    runner.add_solver(sig);
    if (!setup_signal(runner, rng)) {
      std::cout << "bench_link snap: signal setup failed\n";
      return 1;
    }
  } else {
    runner.add_solver(proj);
    setup_world(runner, p, rng);
  }
  CountSink counter;
  runner.add_sink(counter);
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  if (!runner.run(steps).ok()) {
    std::cout << "bench_link snap: run failed\n";
    return 1;
  }
  const auto t1 = Clock::now();
  const double wall = std::chrono::duration<double>(t1 - t0).count();
  const double cpu = cpu_seconds() - c0;
  if (counter.n != steps) {
    std::cout << "bench_link snap: sink saw " << counter.n << " want " << steps
              << "\n";
    return 1;
  }
  // Adapt + encode micro-loop over the final snapshot (opaque: the state
  // came from the xorshift-seeded run above).
  wengine::world::Snapshot snap = wengine::world::take_snapshot(runner.state());
  wengine::link::Frame frame =
      p.is_signal ? wengine::link::frame_from_signal_snapshot(snap)
                  : wengine::link::frame_from_snapshot(snap);
  wengine::link::LinkFrame wire{};
  const double c1 = cpu_seconds();
  const auto t2 = Clock::now();
  for (std::uint64_t k = 0; k < steps; ++k) {
    if (!wengine::link::encode_frame(frame, wire, k + 1).ok()) {
      std::cout << "bench_link snap: encode failed\n";
      return 1;
    }
    do_not_optimize(wire.frame_seq);
  }
  const auto t3 = Clock::now();
  const double enc_wall = std::chrono::duration<double>(t3 - t2).count();
  const double enc_cpu = cpu_seconds() - c1;
  const double rate = wall > 0.0 ? static_cast<double>(steps) / wall : 0.0;
  const double enc_rate =
      enc_wall > 0.0 ? static_cast<double>(steps) / enc_wall : 0.0;
  std::cout << "bench_link snap: payload=" << p.name << " steps=" << steps
            << " snapshot_rate=" << rate << "/s cpu_s=" << cpu
            << " encode_rate=" << enc_rate << "/s encode_cpu_s=" << enc_cpu
            << "\n";
  return 0;
}

int run_flood_or_paced(const Payload &p, std::uint64_t steps, double rate_hz,
                       bool paced, std::uint32_t rng, std::string_view label) {
  const std::string seg =
      "/wengine-bench-link-" + std::to_string(::getpid()) + (paced ? "-p" : "-f");
  SendTable table(static_cast<std::size_t>(steps + 1));
  const char *domain = p.is_signal ? "signal" : "projectile";
  wengine::link::FrameAdapter adapter = p.is_signal
                                            ? &wengine::link::frame_from_signal_snapshot
                                            : &wengine::link::frame_from_snapshot;
  int rc = 0;
  ReaderResult result;
  std::uint64_t published = 0;
  std::uint64_t dropped_bound = 0;
  double wall = 0.0;
  double cpu = 0.0;
  std::uint64_t late = 0;
  double max_late_us = 0.0;
  {
    TimedPublisher timed(seg, domain, adapter, table);
    if (!timed.ok()) {
      std::cout << "bench_link: publisher open failed\n";
      return 1;
    }
    wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
    wengine::solvers::ProjectileSolver proj;
    wengine::solvers::SignalSolver sig;
    if (p.is_signal) {
      runner.add_solver(sig);
      if (!setup_signal(runner, rng)) {
        std::cout << "bench_link: signal setup failed\n";
        return 1;
      }
    } else {
      runner.add_solver(proj);
      setup_world(runner, p, rng);
    }
    runner.add_sink(timed);
    // Active reader attaches BEFORE the run: this is the end-to-end path
    // the old benchmark never exercised.
    std::atomic<bool> stop{false};
    std::thread th(reader_loop, std::cref(timed.seg()), std::ref(table),
                   std::ref(stop), std::ref(result));
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    if (paced) {
      const auto period = std::chrono::nanoseconds(
          static_cast<long long>(1e9 / rate_hz));
      auto deadline = t0;
      for (std::uint64_t k = 0; k < steps; ++k) {
        deadline += period;
        if (!runner.run(1).ok()) {
          std::cout << "bench_link paced: run failed\n";
          rc = 1;
          break;
        }
        const auto now = Clock::now();
        if (now < deadline) {
          std::this_thread::sleep_until(deadline);
        } else {
          ++late;
          const double late_us =
              std::chrono::duration<double>(now - deadline).count() * 1e6;
          if (late_us > max_late_us) {
            max_late_us = late_us;
          }
        }
        if (!timed.ok()) {
          std::cout << "bench_link paced: publisher failed\n";
          rc = 1;
          break;
        }
      }
      const auto t1 = Clock::now();
      wall = std::chrono::duration<double>(t1 - t0).count();
    } else {
      const wengine::core::Status st = runner.run(steps);
      const auto t1 = Clock::now();
      wall = std::chrono::duration<double>(t1 - t0).count();
      if (!st.ok() || !timed.ok()) {
        std::cout << "bench_link flood: run failed\n";
        rc = 1;
      }
    }
    cpu = cpu_seconds() - c0;
    published = timed.stats().published;
    dropped_bound = timed.stats().dropped_over_bound;
    // Stop/join the reader BEFORE the publisher dies (descriptor order).
    stop.store(true, std::memory_order_relaxed);
    th.join();
  }
  if (rc != 0) {
    return rc;
  }
  if (published != steps || dropped_bound != 0) {
    std::cout << "bench_link " << label << ": published=" << published
              << " want " << steps << " over_bound=" << dropped_bound << "\n";
    return 1;
  }
  if (result.bad_decode != 0 || result.decoded == 0) {
    std::cout << "bench_link " << label << ": decoded=" << result.decoded
              << " bad=" << result.bad_decode << "\n";
    return 1;
  }
  print_row(label, p, steps, wall, cpu, published, dropped_bound, result,
            sizeof(wengine::link::LinkFrame));
  if (paced) {
    std::cout << "bench_link paced: rate_hz=" << rate_hz << " late_steps=" << late
              << " max_late_us=" << max_late_us << "\n";
  } else {
    const double ns_per =
        wall * 1e9 / static_cast<double>(steps);
    std::cout << "bench_link flood: " << ns_per << " ns/frame ("
              << static_cast<double>(steps) / wall << " frames/s)\n";
  }
  return 0;
}

int run_stalled(const Payload &p, std::uint64_t steps, std::uint32_t rng) {
  // Intentionally stalled reader: publish with nobody attached, attach
  // after — latest-only view of a full ring, loss is policy.
  const std::string seg =
      "/wengine-bench-link-" + std::to_string(::getpid()) + "-s";
  const char *domain = p.is_signal ? "signal" : "projectile";
  wengine::link::FrameAdapter adapter = p.is_signal
                                            ? &wengine::link::frame_from_signal_snapshot
                                            : &wengine::link::frame_from_snapshot;
  wengine::link::ShmPublisher pub(seg, domain, adapter);
  if (!pub.ok()) {
    std::cout << "bench_link: publisher open failed\n";
    return 1;
  }
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver proj;
  wengine::solvers::SignalSolver sig;
  if (p.is_signal) {
    runner.add_solver(sig);
    if (!setup_signal(runner, rng)) {
      std::cout << "bench_link stalled: signal setup failed\n";
      return 1;
    }
  } else {
    runner.add_solver(proj);
    setup_world(runner, p, rng);
  }
  runner.add_sink(pub);
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  const wengine::core::Status st = runner.run(steps);
  const auto t1 = Clock::now();
  const double wall = std::chrono::duration<double>(t1 - t0).count();
  const double cpu = cpu_seconds() - c0;
  if (!st.ok() || !pub.ok() || pub.stats().published != steps) {
    std::cout << "bench_link: stalled flood failed\n";
    return 1;
  }
  // Attach after the flood; take latest only; destroy the reader before
  // the publisher (descriptor order).
  ReaderResult r;
  {
    wengine::link::ShmReader reader(seg);
    if (!reader.ok()) {
      std::cout << "bench_link: reader attach failed\n";
      return 1;
    }
    wengine::link::LinkFrame wire{};
    double checksum = 0.0;
    if (reader.read_latest(wire)) {
      (void)wengine::link::lag_observe(r.lag, wire.frame_seq);
      r.last_seq = wire.frame_seq;
      wengine::link::Frame frame;
      if (!wengine::link::decode_frame(wire, frame).ok()) {
        std::cout << "bench_link: decode failed\n";
        return 1;
      }
      ++r.decoded;
      for (const auto &it : frame.items) {
        checksum += it.x + it.y + it.vx + it.vy;
      }
      for (const auto &c : frame.channels) {
        checksum += c.value;
      }
      r.checksum = checksum;
      if (wire.frame_seq != steps) {
        std::cout << "bench_link: latest seq " << wire.frame_seq << " want "
                  << steps << "\n";
        return 1;
      }
    } else {
      std::cout << "bench_link: reader saw nothing\n";
      return 1;
    }
    do_not_optimize(checksum);
  }
  const double ns_per = wall * 1e9 / static_cast<double>(steps);
  std::cout << "bench_link stalled: payload=" << p.name << " steps=" << steps
            << " " << ns_per << " ns/frame ("
            << static_cast<double>(steps) / wall << " frames/s)"
            << " cpu_s=" << cpu << " checksum=" << r.checksum << "\n";
  std::cout << "bench_link: stalled reader: published=" << steps
            << " received=" << r.lag.received << " missed=" << r.lag.missed
            << " max_gap=" << r.lag.max_gap << "\n";
  return 0;
}

int run_fanout(const Payload &p, std::uint64_t steps, std::uint32_t rng) {
  // Fan-out: SHM publisher plus a second counting observer on the same
  // Runner — localizes the marginal cost of an extra sink.
  const std::string seg =
      "/wengine-bench-link-" + std::to_string(::getpid()) + "-m";
  SendTable table(static_cast<std::size_t>(steps + 1));
  const char *domain = p.is_signal ? "signal" : "projectile";
  wengine::link::FrameAdapter adapter = p.is_signal
                                            ? &wengine::link::frame_from_signal_snapshot
                                            : &wengine::link::frame_from_snapshot;
  ReaderResult result;
  std::uint64_t published = 0;
  int rc = 0;
  double wall = 0.0;
  double cpu = 0.0;
  std::uint64_t counted = 0;
  {
    TimedPublisher timed(seg, domain, adapter, table);
    if (!timed.ok()) {
      std::cout << "bench_link: publisher open failed\n";
      return 1;
    }
    CountSink extra;
    wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
    wengine::solvers::ProjectileSolver proj;
    wengine::solvers::SignalSolver sig;
    if (p.is_signal) {
      runner.add_solver(sig);
      if (!setup_signal(runner, rng)) {
        std::cout << "bench_link fanout: signal setup failed\n";
        return 1;
      }
    } else {
      runner.add_solver(proj);
      setup_world(runner, p, rng);
    }
    runner.add_sink(timed);
    runner.add_sink(extra);
    std::atomic<bool> stop{false};
    std::thread th(reader_loop, std::cref(timed.seg()), std::ref(table),
                   std::ref(stop), std::ref(result));
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    const wengine::core::Status st = runner.run(steps);
    const auto t1 = Clock::now();
    wall = std::chrono::duration<double>(t1 - t0).count();
    cpu = cpu_seconds() - c0;
    counted = extra.n;
    published = timed.stats().published;
    if (!st.ok() || !timed.ok()) {
      std::cout << "bench_link fanout: run failed\n";
      rc = 1;
    }
    stop.store(true, std::memory_order_relaxed);
    th.join();
  }
  if (rc != 0) {
    return rc;
  }
  if (published != steps || counted != steps) {
    std::cout << "bench_link fanout: published=" << published
              << " counted=" << counted << " want " << steps << "\n";
    return 1;
  }
  print_row("fanout", p, steps, wall, cpu, published, 0, result,
            sizeof(wengine::link::LinkFrame));
  return 0;
}

bool parse_u64(const char *text, std::uint64_t &out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(v);
  return true;
}

void usage() {
  std::cout << "usage: bench_link [--payload signal|particle] [--particles N] "
               "[--steps N] [--mode all|solo|snap|flood|paced|stalled|fanout] "
               "[--rate HZ]\n"
               "       bench_link [particles] [steps]\n";
}

}  // namespace

int main(int argc, char **argv) {
  Payload payload;
  std::uint64_t steps = 20000;
  std::string mode = "all";
  double rate_hz = 1000.0;
  bool legacy = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--payload" && i + 1 < argc) {
      const std::string_view v = argv[++i];
      if (v == "signal") {
        payload.is_signal = true;
        payload.name = "signal";
        payload.particles = 1;
      } else if (v == "particle") {
        payload.is_signal = false;
        payload.name = "particle";
      } else {
        usage();
        return 2;
      }
    } else if (a == "--particles" && i + 1 < argc) {
      std::uint64_t v = 0;
      if (!parse_u64(argv[++i], v) || v == 0 ||
          v > wengine::link::kMaxItems) {
        usage();
        return 2;
      }
      payload.particles = static_cast<std::size_t>(v);
      payload.is_signal = false;
      payload.name = "particle";
    } else if (a == "--steps" && i + 1 < argc) {
      if (!parse_u64(argv[++i], steps) || steps == 0) {
        usage();
        return 2;
      }
    } else if (a == "--mode" && i + 1 < argc) {
      mode = argv[++i];
      if (mode != "all" && mode != "solo" && mode != "snap" &&
          mode != "flood" && mode != "paced" && mode != "stalled" &&
          mode != "fanout") {
        usage();
        return 2;
      }
    } else if (a == "--rate" && i + 1 < argc) {
      const double r = std::strtod(argv[++i], nullptr);
      if (!(r > 0.0)) {
        usage();
        return 2;
      }
      rate_hz = r;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (!a.empty() && a.front() != '-') {
      legacy = true;
      break;
    } else {
      usage();
      return 2;
    }
  }
  if (legacy) {
    // bench_link [particles] [steps]: historic positional form.
    std::uint64_t part = 64;
    std::uint64_t st = 20000;
    if (argc > 1) {
      if (!parse_u64(argv[1], part) || part == 0 ||
          part > wengine::link::kMaxItems) {
        part = 64;
      }
    }
    if (argc > 2) {
      if (!parse_u64(argv[2], st) || st == 0) {
        st = 20000;
      }
    }
    payload.is_signal = false;
    payload.name = "particle";
    payload.particles = static_cast<std::size_t>(part);
    steps = st;
    mode = "all";
  }

  std::uint32_t rng = 0x85EBCA6Bu ^ (static_cast<std::uint32_t>(argc) * 0x9E3779B9u +
                                      0xC2B2AE35u);
  std::string repro = "bench_link --payload " + payload.name;
  repro += " --particles " + std::to_string(payload.particles);
  repro += " --steps " + std::to_string(steps) + " --mode " + mode;
  repro += " --rate " + std::to_string(static_cast<std::uint64_t>(rate_hz));
  print_header(repro);

  // Small paced default would take 20 s at 20k steps; clamp the paced leg
  // to 2 s worth so `all` stays interactive (explicit --mode paced keeps
  // the full count).
  const std::uint64_t paced_steps =
      (mode == "all" && steps > 2 * static_cast<std::uint64_t>(rate_hz))
          ? 2 * static_cast<std::uint64_t>(rate_hz)
          : steps;

  int rc = 0;
  const auto want = [&](std::string_view m) {
    return mode == "all" || mode == m;
  };
  if (want("solo")) {
    rc |= run_solo(payload, steps, rng);
  }
  if (want("snap")) {
    rc |= run_snap(payload, steps, rng);
  }
  if (want("flood")) {
    rc |= run_flood_or_paced(payload, steps, rate_hz, false, rng, "flood");
  }
  if (want("paced")) {
    rc |= run_flood_or_paced(payload, paced_steps, rate_hz, true, rng, "paced");
  }
  if (want("stalled")) {
    rc |= run_stalled(payload, steps, rng);
  }
  if (want("fanout")) {
    rc |= run_fanout(payload, steps, rng);
  }
  // Second payload in `all` mode: one signal + representative particles.
  if (mode == "all" && !legacy) {
    Payload other;
    std::uint64_t other_steps = steps;
    if (payload.is_signal) {
      other.is_signal = false;
      other.name = "particle";
      other.particles = 64;
    } else {
      other.is_signal = true;
      other.name = "signal";
      other.particles = 1;
      other_steps = steps > 5000 ? 5000 : steps;
    }
    const std::uint64_t other_paced =
        other_steps > 2 * static_cast<std::uint64_t>(rate_hz)
            ? 2 * static_cast<std::uint64_t>(rate_hz)
            : other_steps;
    rc |= run_flood_or_paced(other, other_steps, rate_hz, false, rng, "flood");
    rc |= run_flood_or_paced(other, other_paced, rate_hz, true, rng, "paced");
    rc |= run_stalled(other, other_steps, rng);
  }
  return rc;
}
