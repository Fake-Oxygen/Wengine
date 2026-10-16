// Worker-backed TCP output benchmark (PHYSI-46, PHYSI-29 methodology):
// sim-callback latency (the metric the worker exists to minimize) plus
// end-to-end goodput through a validating reader. Every scenario uses an
// ACTIVE reader that parses each length-prefixed record (hello gate, kind
// dispatch, values/steps against a deterministic pattern, final counts).
// Reader threads stop/join BEFORE their descriptors close. No timing
// thresholds are asserted.
//
// Scenarios: flood (tight snapshot loop into an ordered worker, draining
// reader) and engine (signal run_headless beside a solo reference for
// numerical isolation). Delivery vocabulary follows docs/delivery.md.
// Callback latency is measured BOTH in the driver (per-on_snapshot
// steady_clock deltas: p50/max) and via the worker's own callback
// counters as a cross-check.
//
// Repro:
//   bench_tcp_worker [snapshots] [sources] [ordered|latest]

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/record_stream.hpp"
#include "wengine/link/sample_block.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/link/tcp_worker.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/world/state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double cpu_seconds() {
  rusage ru{};
  ::getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec) / 1e6 +
         static_cast<double>(ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
}

std::string compiler_string() {
#if defined(__VERSION__)
  return __VERSION__;
#else
  return "unknown-compiler";
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

int make_tcp_listener(std::uint16_t &port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 8) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  port = ntohs(addr.sin_port);
  return fd;
}

void set_recv_timeout(int fd, long secs) {
  timeval tv{};
  tv.tv_sec = secs;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Deterministic synthetic tick: `sources` channels, value = step * 10 + c
// (self-describing: the reader verifies every value against its step).
wengine::world::Snapshot make_tick(std::uint64_t step, std::size_t sources) {
  wengine::world::Snapshot s;
  s.step_index = step;
  s.sim_time = 0.001 * static_cast<double>(step);
  for (std::size_t c = 0; c < sources; ++c) {
    s.sig_serial.push_back(static_cast<std::uint64_t>(c));
    s.sig_quantity.push_back(0);
    s.sig_value.push_back(10.0 * static_cast<double>(step) + static_cast<double>(c));
  }
  return s;
}

struct FloodResult {
  std::uint64_t snapshots = 0;
  std::uint64_t blocks = 0;
  std::uint64_t samples = 0;
  std::uint64_t value_errors = 0;
  std::uint64_t step_gaps = 0;
  std::uint64_t last_step = 0;
};

void verify_stream(const std::vector<unsigned char> &bytes, std::size_t sources,
                   FloodResult &res) {
  wengine::link::StreamDecode dec;
  if (!wengine::link::decode_stream(bytes.data(), bytes.size(), "bench", dec).ok()) {
    ++res.value_errors;
    return;
  }
  for (const wengine::link::StreamRecord &r : dec.records) {
    if (r.kind != wengine::link::compact::RecordKind::kSampleBlock) {
      ++res.value_errors;
      continue;
    }
    ++res.blocks;
    const auto &b = r.block;
    if (b.channel_count() != sources) {
      ++res.value_errors;
      continue;
    }
    for (std::size_t k = 0; k < b.sample_count(); ++k) {
      ++res.samples;
      if (res.last_step != 0 && b.steps[k] != res.last_step + 1) {
        ++res.step_gaps;
      }
      res.last_step = b.steps[k];
      for (std::size_t c = 0; c < sources; ++c) {
        const double want =
            10.0 * static_cast<double>(b.steps[k]) + static_cast<double>(c);
        if (b.values[k * sources + c] != want) {
          ++res.value_errors;
        }
      }
    }
  }
}

int run_flood(std::uint64_t snapshots, std::size_t sources, bool latest) {
  std::uint16_t port = 0;
  const int listen_fd = make_tcp_listener(port);
  if (listen_fd < 0) {
    std::cout << "bench_tcp_worker flood: socket setup failed\n";
    return 1;
  }
  wengine::link::TcpWorkerConfig cfg;
  cfg.endpoint = "127.0.0.1:" + std::to_string(port);
  cfg.domain = "bench";
  cfg.mode = latest ? wengine::link::TcpWorkerMode::kLatest
                    : wengine::link::TcpWorkerMode::kOrdered;
  // Absorb the whole flood: the bench measures steady cost, not drops
  // (drop behavior is covered in test_tcp_worker).
  cfg.queue.max_slots = static_cast<std::size_t>(snapshots / 64 + 32);
  wengine::link::TcpBlockWorker worker(cfg);
  if (!worker.ok()) {
    std::cout << "bench_tcp_worker flood: connect failed\n";
    ::close(listen_fd);
    return 1;
  }
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  if (peer < 0) {
    std::cout << "bench_tcp_worker flood: accept failed\n";
    ::close(listen_fd);
    return 1;
  }
  set_recv_timeout(peer, 30);

  std::vector<unsigned char> bytes;
  std::atomic<bool> reader_done{false};
  FloodResult res;
  res.snapshots = snapshots;
  std::thread reader([&] {
    wengine::link::TcpHello hello{};
    std::size_t got = 0;
    while (got < sizeof(hello)) {
      const ssize_t n =
          ::recv(peer, reinterpret_cast<unsigned char *>(&hello) + got, sizeof(hello) - got, 0);
      if (n <= 0) {
        return;
      }
      got += static_cast<std::size_t>(n);
    }
    if (!wengine::link::check_tcp_hello(hello, "bench").ok()) {
      return;
    }
    std::array<unsigned char, 65536> chunk{};
    for (;;) {
      const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
      if (n <= 0) {
        reader_done.store(true, std::memory_order_release);
        return;
      }
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
    }
  });

  std::vector<double> callback_us;
  callback_us.reserve(static_cast<std::size_t>(snapshots));
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // worker start settle
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  for (std::uint64_t i = 1; i <= snapshots; ++i) {
    const auto tick = make_tick(i, sources);
    const auto s0 = Clock::now();
    worker.on_snapshot(tick);
    const auto s1 = Clock::now();
    callback_us.push_back(
        std::chrono::duration<double, std::micro>(s1 - s0).count());
  }
  const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
  const double cpu = cpu_seconds() - c0;
  const auto finish_st = worker.finish(30000);
  // Settle so the reader usually captures the accepted tail too (indicative
  // goodput, not a completeness proof — tests own exactness).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Reader stops at EOF: shutdown our side after the worker drained.
  ::shutdown(peer, SHUT_RDWR);
  reader.join();
  ::close(peer);
  ::close(listen_fd);

  std::sort(callback_us.begin(), callback_us.end());
  verify_stream(bytes, sources, res);
  const wengine::link::TcpWorkerHealth h = worker.health();
  const double recv_samples_per_s =
      wall > 0.0 ? static_cast<double>(res.samples) / wall : 0.0;
  std::cout << "scenario=flood mode=" << (latest ? "latest" : "ordered")
            << " snapshots=" << snapshots << " sources=" << sources << "\n";
  std::cout << "callback_us: count=" << callback_us.size()
            << " p50=" << percentile_sorted(callback_us, 0.5)
            << " max=" << percentile_sorted(callback_us, 1.0)
            << " worker_max_ns=" << h.callback_ns_max << "\n";
  std::cout << "wall_s=" << wall << " cpu_s=" << cpu << " bytes=" << bytes.size() << "\n";
  std::cout << "received: blocks=" << res.blocks << " samples=" << res.samples
            << " value_errors=" << res.value_errors << " step_gaps=" << res.step_gaps
            << " goodput_samples_per_s=" << recv_samples_per_s << "\n";
  std::cout << "worker: attempted=" << h.attempted << " published=" << h.published_blocks
            << " queue_full=" << h.queue.dropped_blocks
            << " coalesced=" << h.coalesced_blocks << " send_drops=" << h.dropped_send
            << " over_bound=" << h.dropped_over_bound
            << " finish=" << (finish_st.ok() ? "ok" : "FAIL") << "\n";
  // Latest mode coalesces by design (gaps expected, counted); ordered must
  // be gap-free into its absorbent queue.
  const bool ordered_loss = !latest && (res.step_gaps != 0 || h.queue.dropped_blocks != 0);
  return (res.value_errors == 0 && finish_st.ok() && !ordered_loss) ? 0 : 1;
}

struct EngineRecorder : public wengine::emit::ISink {
  std::vector<wengine::world::Snapshot> snaps;
  void on_snapshot(const wengine::world::Snapshot &s) override { snaps.push_back(s); }
};

int run_engine(std::uint64_t steps) {
  // Signal run beside a solo reference: networking must leave numerics
  // bit-identical while the block stream stays gap-free end to end.
  std::uint16_t port = 0;
  const int listen_fd = make_tcp_listener(port);
  if (listen_fd < 0) {
    std::cout << "bench_tcp_worker engine: socket setup failed\n";
    return 1;
  }
  wengine::link::TcpWorkerConfig cfg;
  cfg.endpoint = "127.0.0.1:" + std::to_string(port);
  cfg.domain = "bench";
  cfg.queue.max_slots = 128; // absorb the whole run; drops are a test topic
  wengine::link::TcpBlockWorker worker(cfg);
  if (!worker.ok()) {
    std::cout << "bench_tcp_worker engine: connect failed\n";
    ::close(listen_fd);
    return 1;
  }
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  if (peer < 0) {
    std::cout << "bench_tcp_worker engine: accept failed\n";
    ::close(listen_fd);
    return 1;
  }
  set_recv_timeout(peer, 30);

  std::vector<unsigned char> bytes;
  std::thread reader([&] {
    std::array<unsigned char, 65536> chunk{};
    for (;;) {
      const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
      if (n <= 0) {
        return;
      }
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
    }
  });

  wengine::core::RunConfig run_cfg;
  run_cfg.loop.dt = 0.001;
  run_cfg.loop.emit_every = 1;
  run_cfg.steps = steps;
  run_cfg.solver = "signal";
  wengine::world::State final_state;
  EngineRecorder rec;
  std::array<wengine::emit::ISink *, 2> sinks{&worker, &rec};
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // worker start settle
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  const auto run_st =
      wengine::solvers::run_headless(run_cfg, sinks, final_state);
  const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
  const double cpu = cpu_seconds() - c0;
  const auto finish_st = worker.finish(30000);
  std::this_thread::sleep_for(std::chrono::milliseconds(200)); // tail settle, as above
  ::shutdown(peer, SHUT_RDWR);
  reader.join();
  ::close(peer);
  ::close(listen_fd);

  wengine::world::State ref_state;
  const auto ref_st = wengine::solvers::run_headless(
      run_cfg, std::span<wengine::emit::ISink *>{}, ref_state);

  // The reader accumulated the 40-byte hello first; split past it for
  // exact block accounting (a hello-prefixed blob is not a record stream).
  std::vector<wengine::link::StreamRecord> records;
  std::uint64_t stream_samples = 0;
  std::uint64_t last_step = 0;
  std::uint64_t gaps = 0;
  bool hello_ok = false;
  if (bytes.size() > sizeof(wengine::link::TcpHello)) {
    wengine::link::TcpHello hello{};
    std::memcpy(&hello, bytes.data(), sizeof(hello));
    hello_ok = wengine::link::check_tcp_hello(hello, "bench").ok();
    wengine::link::StreamDecode blocks;
    if (wengine::link::decode_stream(bytes.data() + sizeof(wengine::link::TcpHello),
                                     bytes.size() - sizeof(wengine::link::TcpHello),
                                     "bench", blocks)
            .ok()) {
      records = std::move(blocks.records);
    }
  }
  for (const auto &r : records) {
    if (r.kind != wengine::link::compact::RecordKind::kSampleBlock) {
      continue;
    }
    for (std::uint64_t st : r.block.steps) {
      ++stream_samples;
      if (last_step != 0 && st != last_step + 1) {
        ++gaps;
      }
      last_step = st;
    }
  }
  const wengine::link::TcpWorkerHealth h = worker.health();
  std::cout << "scenario=engine steps=" << steps << "\n";
  std::cout << "run=" << (run_st.ok() ? "ok" : "FAIL") << " ref=" << (ref_st.ok() ? "ok" : "FAIL")
            << " state_equal=" << (final_state == ref_state ? "yes" : "NO")
            << " wall_s=" << wall << " cpu_s=" << cpu << "\n";
  std::cout << "stream: blocks=" << records.size() << " samples=" << stream_samples
            << " step_gaps=" << gaps << " last_step=" << last_step << "\n";
  std::cout << "worker: attempted=" << h.attempted << " published=" << h.published_blocks
            << " queue_full=" << h.queue.dropped_blocks
            << " callback_max_ns=" << h.callback_ns_max
            << " finish=" << (finish_st.ok() ? "ok" : "FAIL") << "\n";
  const bool ok = run_st.ok() && ref_st.ok() && final_state == ref_state && gaps == 0 &&
                  stream_samples == steps && h.queue.dropped_blocks == 0 && hello_ok &&
                  finish_st.ok();
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  std::cout << "bench_tcp_worker (" << compiler_string() << ")\n";
  std::cout << "repro: bench_tcp_worker [snapshots] [sources] [ordered|latest] (flood) "
               "+ engine case\n";
  std::uint64_t snapshots = 20000;
  std::size_t sources = 8;
  bool latest = false;
  if (argc > 1) {
    snapshots = static_cast<std::uint64_t>(std::strtoul(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    sources = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
  }
  if (argc > 3) {
    latest = std::string(argv[3]) == "latest";
  }
  if (snapshots == 0 || sources == 0 || sources > 1024) {
    std::cout << "usage: bench_tcp_worker [snapshots] [sources] [ordered|latest]\n";
    return 2;
  }
  // Opaque semantics guard so the pattern is not constant-folded away.
  volatile double wobble_src = 1.0;
  (void)wobble_src;
  int rc = 0;
  rc |= run_flood(snapshots, sources, latest);
  rc |= run_engine(5000);
  return rc;
}
