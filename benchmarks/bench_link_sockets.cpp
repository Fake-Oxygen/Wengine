// Loopback goodput/latency benchmark for the SIG-7 TCP/UDP publishers
// (PHYSI-29). Every scenario uses an ACTIVE reader that parses and
// validates each frame (sequence IDs, complete framing, values against a
// deterministic per-step pattern, final counts). Reader threads are
// always stopped/joined BEFORE their descriptors close — the old binary
// counted TCP bytes without decoding and closed the UDP receiver before
// joining its drain thread.
//
// Scenarios per transport, per payload (one signal, representative
// particle counts): flood (tight loop), paced 1 kHz (sleep-paced driver),
// stalled reader (publish first, drain after — backpressure/loss path),
// plus transport-independent solo (solver-only) and snap (snapshot-only)
// and a TCP+UDP fan-out to localize cost. Delivery vocabulary follows
// docs/delivery.md: attempted vs. published vs. dropped_send vs.
// received/missed/max_gap vs. incomplete tail. Delivery age is
// recv-send per frame_seq with p50/p99; CPU is getrusage user+sys.
// No timing thresholds are asserted.
//
// Repro:
//   bench_link_sockets [tcp|udp] [frames] [items]
//   bench_link_sockets [--transport tcp|udp|all] [--frames N]
//     [--items N] [--payload signal|particle] [--mode all|solo|snap|
//     flood|paced|stalled|fanout] [--rate HZ]

#include "wengine/core/runner.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/solvers/projectile.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/world/signals.hpp"
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
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

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

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

bool recv_exact(int fd, unsigned char *buf, std::size_t len) {
  std::size_t got = 0;
  while (got < len) {
    const ssize_t n = ::recv(fd, buf + got, len - got, 0);
    if (n <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(n);
  }
  return true;
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

int make_udp_receiver(std::uint16_t &port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }
  timeval timeout{};
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  port = ntohs(addr.sin_port);
  return fd;
}

void set_recv_timeout(int fd, long secs) {
  timeval tv{};
  tv.tv_sec = secs;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

struct ReaderResult {
  wengine::link::LagStats lag{};
  std::uint64_t last_seq = 0;
  std::uint64_t decoded = 0;
  std::uint64_t bad = 0;
  std::uint64_t value_mismatch = 0;
  std::uint64_t bytes = 0;
  double checksum = 0.0;
  std::vector<double> ages_us;
};

// Deterministic per-step pattern: step i carries pos_x[0] (particle) or
// sig_value[0] (signal) as a pure function of i, so the reader validates
// VALUES, not just sequence IDs. Base vectors come from a runtime xorshift
// so the compiler cannot fold the stream.
struct Pattern {
  bool is_signal = false;
  std::size_t items = 64;
  std::vector<double> base_pos_x;
  std::vector<double> base_pos_y;
  std::vector<double> base_vel_x;
  std::vector<double> base_vel_y;
  double base_sig = 3.3;
};

Pattern make_pattern(bool is_signal, std::size_t items, std::uint32_t seed) {
  Pattern p;
  p.is_signal = is_signal;
  p.items = is_signal ? 0 : items;
  std::uint32_t rng = seed;
  if (!is_signal) {
    p.base_pos_x.resize(items);
    p.base_pos_y.resize(items);
    p.base_vel_x.resize(items);
    p.base_vel_y.resize(items);
    for (std::size_t k = 0; k < items; ++k) {
      p.base_pos_x[k] = static_cast<double>(xorshift(rng)) / 4294967296.0;
      p.base_pos_y[k] = static_cast<double>(xorshift(rng)) / 4294967296.0;
      p.base_vel_x[k] = static_cast<double>(xorshift(rng)) / 4294967296.0;
      p.base_vel_y[k] = static_cast<double>(xorshift(rng)) / 4294967296.0;
    }
  } else {
    p.base_sig = 3.3 + static_cast<double>(xorshift(rng) % 1000) / 1e6;
  }
  return p;
}

void fill_snapshot(wengine::world::Snapshot &s, const Pattern &p,
                   std::uint64_t step) {
  s.step_index = step;
  s.sim_time = static_cast<double>(step) * 0.001;
  if (p.is_signal) {
    s.pos_x.clear();
    s.pos_y.clear();
    s.vel_x.clear();
    s.vel_y.clear();
    s.sig_serial = {0};
    s.sig_value = {p.base_sig + static_cast<double>(step) * 1e-6};
    s.sig_quantity = {static_cast<std::uint8_t>(
        wengine::signal::QuantityId::kVoltage)};
  } else {
    s.sig_serial.clear();
    s.sig_value.clear();
    s.sig_quantity.clear();
    s.pos_x = p.base_pos_x;
    s.pos_y = p.base_pos_y;
    s.vel_x = p.base_vel_x;
    s.vel_y = p.base_vel_y;
    if (!s.pos_x.empty()) {
      s.pos_x[0] = static_cast<double>(step) * 1e-6;
    }
  }
}

bool check_frame_values(const wengine::link::Frame &f, const Pattern &p,
                        std::uint64_t seq) {
  if (f.step != seq) {
    return false;
  }
  if (p.is_signal) {
    if (f.items.size() != 0 || f.channels.size() != 2) {
      return false;
    }
    const double want = p.base_sig + static_cast<double>(seq) * 1e-6;
    return f.channels[1].value == want;
  }
  if (f.items.size() != p.items) {
    return false;
  }
  if (p.items == 0) {
    return true;
  }
  return f.items[0].x == static_cast<double>(seq) * 1e-6;
}

wengine::link::FrameAdapter adapter_for(const Pattern &p) {
  return p.is_signal ? &wengine::link::frame_from_signal_snapshot
                     : &wengine::link::frame_from_snapshot;
}

// TCP active reader: accepts one peer, gates the hello, then parses every
// length-prefixed frame (complete framing), decodes, and validates values.
void tcp_reader_loop(int listen_fd, const std::string &domain,
                     const Pattern &pattern,
                     std::vector<std::atomic<std::int64_t>> &send_ns,
                     ReaderResult &out) {
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  if (peer < 0) {
    return;
  }
  set_recv_timeout(peer, 10);
  wengine::link::TcpHello hello{};
  if (!recv_exact(peer, reinterpret_cast<unsigned char *>(&hello),
                  sizeof(hello)) ||
      !wengine::link::check_tcp_hello(hello, domain).ok()) {
    ::close(peer);
    return;
  }
  std::uint64_t prev_seq = 0;
  bool first = true;
  while (true) {
    unsigned char prefix[4]{};
    if (!recv_exact(peer, prefix, sizeof(prefix))) {
      break;  // EOF after publisher close, or stall timeout
    }
    const std::uint32_t len =
        wengine::link::socket_detail::get_u32_le(prefix);
    if (len < wengine::link::compact::kHeaderSize ||
        len > wengine::link::compact::kMaxRecordBytes) {
      ++out.bad;
      break;
    }
    std::vector<unsigned char> rec(len);
    if (!recv_exact(peer, rec.data(), len)) {
      ++out.bad;
      break;
    }
    const std::int64_t recv = now_ns();
    out.bytes += 4 + len;
    wengine::link::Frame frame;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    wengine::link::compact::RecordKind kind{};
    if (!wengine::link::compact::decode_record(rec.data(), len, domain, frame,
                                               run_id, seq, kind)
             .ok()) {
      ++out.bad;
      continue;
    }
    (void)run_id;
    if (!first && seq <= prev_seq) {
      ++out.bad;
      continue;
    }
    first = false;
    prev_seq = seq;
    (void)wengine::link::lag_observe(out.lag, seq);
    out.last_seq = seq;
    if (!check_frame_values(frame, pattern, seq)) {
      ++out.value_mismatch;
    }
    ++out.decoded;
    for (const auto &it : frame.items) {
      out.checksum += it.x + it.vx;
    }
    for (const auto &c : frame.channels) {
      out.checksum += c.value;
    }
    if (seq < send_ns.size()) {
      const std::int64_t sent =
          send_ns[static_cast<std::size_t>(seq)].load(
              std::memory_order_relaxed);
      if (sent != 0) {
        out.ages_us.push_back(static_cast<double>(recv - sent) / 1000.0);
      }
    }
  }
  ::close(peer);
}

// UDP active reader: decodes every datagram, validates domain/framing/
// values, tracks LagStats. Stops on the atomic flag (main shuts the socket
// down to unblock a parked recvfrom, then JOINS before closing).
void udp_reader_loop(int fd, const std::string &domain, const Pattern &pattern,
                     std::vector<std::atomic<std::int64_t>> &send_ns,
                     std::atomic<bool> &stop, ReaderResult &out) {
  std::array<unsigned char, wengine::link::kMaxUdpPayload> buf{};
  while (!stop.load(std::memory_order_relaxed)) {
    const ssize_t n =
        ::recvfrom(fd, buf.data(), buf.size(), 0, nullptr, nullptr);
    if (n <= 0) {
      continue;  // timeout tick: re-check stop
    }
    const std::int64_t recv = now_ns();
    out.bytes += static_cast<std::uint64_t>(n);
    wengine::link::Frame frame;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    wengine::link::compact::RecordKind kind{};
    if (!wengine::link::compact::decode_record(buf.data(),
                                               static_cast<std::size_t>(n),
                                               domain, frame, run_id, seq, kind)
             .ok()) {
      ++out.bad;
      continue;
    }
    (void)run_id;
    (void)wengine::link::lag_observe(out.lag, seq);
    out.last_seq = seq > out.last_seq ? seq : out.last_seq;
    if (!check_frame_values(frame, pattern, seq)) {
      ++out.value_mismatch;
    }
    ++out.decoded;
    for (const auto &c : frame.channels) {
      out.checksum += c.value;
    }
    if (seq < send_ns.size()) {
      const std::int64_t sent =
          send_ns[static_cast<std::size_t>(seq)].load(
              std::memory_order_relaxed);
      if (sent != 0) {
        out.ages_us.push_back(static_cast<double>(recv - sent) / 1000.0);
      }
    }
  }
}

void print_header(std::string_view repro) {
  std::cout << "bench_link_sockets: compiler=\"" << compiler_string() << "\""
#ifdef NDEBUG
            << " ndebug=1"
#else
            << " ndebug=0"
#endif
            << " tcp_wire_max=" << wengine::link::kTcpWireBytes
            << " socket_schema=" << wengine::link::kTcpVersion << "\n"
            << "bench_link_sockets: repro: " << repro << "\n";
}

void print_row(std::string_view transport, std::string_view scenario,
               const Pattern &p, std::uint64_t frames, double wall_s,
               double cpu_s, const wengine::link::PublisherStats &stats,
               bool pending, const ReaderResult &r) {
  std::vector<double> ages = r.ages_us;
  std::sort(ages.begin(), ages.end());
  const double p50 = percentile_sorted(ages, 0.50);
  const double p99 = percentile_sorted(ages, 0.99);
  const double attempted =
      wall_s > 0.0 ? static_cast<double>(frames) / wall_s : 0.0;
  const double goodput =
      wall_s > 0.0 ? static_cast<double>(r.lag.received) / wall_s : 0.0;
  const std::uint64_t tail =
      stats.published > r.last_seq ? stats.published - r.last_seq : 0;
  std::cout << "bench_link_sockets " << transport << " " << scenario
            << ": payload=" << (p.is_signal ? "signal" : "particle")
            << " frames=" << frames << " items=" << p.items
            << " attempted=" << attempted << "/s goodput=" << goodput << "/s"
            << " published_local=" << stats.published
            << " dropped_send=" << stats.dropped_send
            << " dropped_over_bound=" << stats.dropped_over_bound
            << " pending_blocked=" << stats.dropped_pending_blocked
            << " disabled=" << stats.disabled_after_error
            << " pending=" << (pending ? 1 : 0)
            << " received=" << r.lag.received << " missed=" << r.lag.missed
            << " max_gap=" << r.lag.max_gap << " tail=" << tail
            << " decoded=" << r.decoded << " bad=" << r.bad
            << " value_mismatch=" << r.value_mismatch
            << " bytes=" << r.bytes << " p50_age_us=" << p50
            << " p99_age_us=" << p99 << " cpu_s=" << cpu_s
            << " cpu_pct="
            << (wall_s > 0.0 ? 100.0 * cpu_s / wall_s : 0.0)
            << " checksum=" << r.checksum << "\n";
}

struct CountSink : public wengine::emit::ISink {
  std::uint64_t n = 0;
  void on_snapshot(const wengine::world::Snapshot &) override { ++n; }
};

int run_solo(bool is_signal, std::size_t items, std::uint64_t steps) {
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver proj;
  wengine::solvers::SignalSolver sig;
  std::uint32_t rng = 0x51ED270Bu;
  if (is_signal) {
    runner.add_solver(sig);
    wengine::signal::SignalParams params;
    params.kind = wengine::signal::SignalKind::kSine;
    params.freq_hz = 10.0 + static_cast<double>(rng % 1000);
    wengine::world::Entity e = runner.state().spawn_entity();
    if (!wengine::world::configure_source(e, runner.state().signals, params,
                                          0.0)
             .ok()) {
      std::cout << "bench_link_sockets solo: setup failed\n";
      return 1;
    }
  } else {
    runner.add_solver(proj);
    for (std::size_t k = 0; k < items; ++k) {
      wengine::world::Entity e = runner.state().spawn_entity();
      runner.state().particles.attach(e.index);
    }
  }
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  if (!runner.run(steps).ok()) {
    std::cout << "bench_link_sockets solo: run failed\n";
    return 1;
  }
  const double wall =
      std::chrono::duration<double>(Clock::now() - t0).count();
  std::cout << "bench_link_sockets solo: payload="
            << (is_signal ? "signal" : "particle") << " steps=" << steps
            << " solver_rate=" << static_cast<double>(steps) / wall << "/s"
            << " cpu_s=" << cpu_seconds() - c0 << "\n";
  return 0;
}

int run_snap(bool is_signal, std::size_t items, std::uint64_t steps) {
  wengine::core::Runner runner{{.dt = 0.01, .emit_every = 1}};
  wengine::solvers::ProjectileSolver proj;
  wengine::solvers::SignalSolver sig;
  std::uint32_t rng = 0x51ED270Bu;
  if (is_signal) {
    runner.add_solver(sig);
    wengine::signal::SignalParams params;
    params.kind = wengine::signal::SignalKind::kSine;
    params.freq_hz = 10.0 + static_cast<double>(rng % 1000);
    wengine::world::Entity e = runner.state().spawn_entity();
    if (!wengine::world::configure_source(e, runner.state().signals, params,
                                          0.0)
             .ok()) {
      std::cout << "bench_link_sockets snap: setup failed\n";
      return 1;
    }
  } else {
    runner.add_solver(proj);
    for (std::size_t k = 0; k < items; ++k) {
      wengine::world::Entity e = runner.state().spawn_entity();
      runner.state().particles.attach(e.index);
    }
  }
  CountSink counter;
  runner.add_sink(counter);
  const double c0 = cpu_seconds();
  const auto t0 = Clock::now();
  if (!runner.run(steps).ok() || counter.n != steps) {
    std::cout << "bench_link_sockets snap: run failed\n";
    return 1;
  }
  const double wall =
      std::chrono::duration<double>(Clock::now() - t0).count();
  std::cout << "bench_link_sockets snap: payload="
            << (is_signal ? "signal" : "particle") << " steps=" << steps
            << " snapshot_rate=" << static_cast<double>(steps) / wall << "/s"
            << " cpu_s=" << cpu_seconds() - c0 << "\n";
  return 0;
}

int run_tcp(const Pattern &p, std::uint64_t frames, double rate_hz, bool paced,
            bool stalled, std::string_view label) {
  std::uint16_t port = 0;
  const int listen_fd = make_tcp_listener(port);
  if (listen_fd < 0) {
    std::cout << "bench_link_sockets: cannot create loopback receiver\n";
    return 1;
  }
  const std::string endpoint = "127.0.0.1:" + std::to_string(port);
  std::vector<std::atomic<std::int64_t>> send_ns(
      static_cast<std::size_t>(frames + 1));
  for (auto &a : send_ns) {
    a.store(0, std::memory_order_relaxed);
  }
  ReaderResult result;
  wengine::link::PublisherStats stats{};
  bool pending = false;
  double wall = 0.0;
  double cpu = 0.0;
  int rc = 0;
  wengine::world::Snapshot snapshot;

  if (stalled) {
    // Stalled reader: the peer is accepted but never drained during the
    // flood, so kernel backpressure drops are exercised; drain only after
    // the run. The publisher is created first (its connect sits in the
    // listen backlog), then the peer is accepted.
    // Stalled reader: the peer is accepted but never drained during the
    // flood, so kernel backpressure drops are exercised; drain only after
    // the run. The publisher connects first (its SYN sits in the listen
    // backlog), then the peer is accepted while the publisher is alive;
    // the publisher is destroyed before the drain so FIN terminates it.
    int peer = -1;
    {
      wengine::link::TcpPublisher publisher(endpoint, "bench", adapter_for(p));
      if (!publisher.ok()) {
        std::cout << "bench_link_sockets: TCP connect failed\n";
        ::close(listen_fd);
        return 1;
      }
      peer = ::accept(listen_fd, nullptr, nullptr);
      if (peer < 0) {
        std::cout << "bench_link_sockets tcp stalled: accept failed\n";
        ::close(listen_fd);
        return 1;
      }
      const double c0 = cpu_seconds();
      const auto t0 = Clock::now();
      for (std::uint64_t i = 1; i <= frames; ++i) {
        fill_snapshot(snapshot, p, i);
        publisher.on_snapshot(snapshot);
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
      cpu = cpu_seconds() - c0;
      stats = publisher.stats();
      pending = publisher.has_pending_frame();
    }  // publisher dtor: FIN after all accepted bytes
    set_recv_timeout(peer, 10);
    wengine::link::TcpHello hello{};
    if (recv_exact(peer, reinterpret_cast<unsigned char *>(&hello),
                   sizeof(hello)) &&
        wengine::link::check_tcp_hello(hello, "bench").ok()) {
      std::array<unsigned char, 65536> chunk{};
      std::vector<unsigned char> bytes;
      while (true) {
        const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
        if (n <= 0) {
          break;
        }
        result.bytes += static_cast<std::uint64_t>(n);
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
      }
      std::uint64_t prev = 0;
      std::uint64_t received = 0;
      std::size_t cursor = 0;
      while (cursor + 4 <= bytes.size()) {
        const std::uint32_t rec_len =
            wengine::link::socket_detail::get_u32_le(bytes.data() + cursor);
        if (rec_len < wengine::link::compact::kHeaderSize ||
            rec_len > wengine::link::compact::kMaxRecordBytes) {
          ++result.bad;
          break;
        }
        if (cursor + 4 + rec_len > bytes.size()) {
          break; // trailing partial record: the pending tail, counted below
        }
        wengine::link::Frame f;
        std::uint64_t run_id = 0;
        std::uint64_t seq = 0;
        wengine::link::compact::RecordKind kind{};
        if (!wengine::link::compact::decode_record(bytes.data() + cursor + 4,
                                                   rec_len, "bench", f, run_id,
                                                   seq, kind)
                 .ok()) {
          ++result.bad;
          break;
        }
        (void)run_id;
        if (seq <= prev && received > 0) {
          ++result.bad;
        }
        prev = seq;
        (void)wengine::link::lag_observe(result.lag, seq);
        result.last_seq = seq;
        if (!check_frame_values(f, p, seq)) {
          ++result.value_mismatch;
        }
        ++result.decoded;
        ++received;
        result.checksum += f.step;
        cursor += 4 + rec_len;
      }
      if ((cursor < bytes.size()) != pending) {
        std::cout << "bench_link_sockets tcp stalled: pending-tail mismatch\n";
        rc = 1;
      }
    }
    ::close(peer);
    ::close(listen_fd);
    print_row("tcp", label, p, frames, wall, cpu, stats, pending, result);
    if (stats.published + stats.dropped_send + stats.dropped_pending_blocked +
            static_cast<std::uint64_t>(pending) !=
        frames) {
      std::cout << "bench_link_sockets tcp stalled: accounting mismatch\n";
      rc = 1;
    }
    return rc;
  }

  // Flood + paced: reader thread parses concurrently; join BEFORE closing
  // the listener (descriptor order).
  std::thread reader;
  reader = std::thread([&] {
    tcp_reader_loop(listen_fd, "bench", p, send_ns, result);
  });
  {
    wengine::link::TcpPublisher publisher(endpoint, "bench", adapter_for(p));
    if (!publisher.ok()) {
      std::cout << "bench_link_sockets: TCP connect failed\n";
      ::shutdown(listen_fd, SHUT_RDWR);
      reader.join();
      ::close(listen_fd);
      return 1;
    }
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    if (paced) {
      const auto period = std::chrono::nanoseconds(
          static_cast<long long>(1e9 / rate_hz));
      auto deadline = t0;
      for (std::uint64_t i = 1; i <= frames; ++i) {
        deadline += period;
        fill_snapshot(snapshot, p, i);
        if (i < send_ns.size()) {
          send_ns[static_cast<std::size_t>(i)].store(now_ns(),
                                                     std::memory_order_relaxed);
        }
        publisher.on_snapshot(snapshot);
        const auto now = Clock::now();
        if (now < deadline) {
          std::this_thread::sleep_until(deadline);
        }
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
    } else {
      for (std::uint64_t i = 1; i <= frames; ++i) {
        fill_snapshot(snapshot, p, i);
        if (i < send_ns.size()) {
          send_ns[static_cast<std::size_t>(i)].store(now_ns(),
                                                     std::memory_order_relaxed);
        }
        publisher.on_snapshot(snapshot);
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
    }
    cpu = cpu_seconds() - c0;
    stats = publisher.stats();
    pending = publisher.has_pending_frame();
  }  // publisher close: FIN terminates the reader
  reader.join();
  ::close(listen_fd);
  print_row("tcp", label, p, frames, wall, cpu, stats, pending, result);
  if (result.bad != 0 || result.value_mismatch != 0) {
    std::cout << "bench_link_sockets tcp " << label << ": framing/value errors\n";
    rc = 1;
  }
  if (!stalled && stats.dropped_over_bound != 0) {
    std::cout << "bench_link_sockets tcp " << label << ": over-bound drop\n";
    rc = 1;
  }
  return rc;
}

int run_udp(const Pattern &p, std::uint64_t frames, double rate_hz, bool paced,
            bool stalled, std::string_view label) {
  std::uint16_t port = 0;
  const int recv_fd = make_udp_receiver(port);
  if (recv_fd < 0) {
    std::cout << "bench_link_sockets: cannot create loopback receiver\n";
    return 1;
  }
  const std::string endpoint = "127.0.0.1:" + std::to_string(port);
  std::vector<std::atomic<std::int64_t>> send_ns(
      static_cast<std::size_t>(frames + 1));
  for (auto &a : send_ns) {
    a.store(0, std::memory_order_relaxed);
  }
  ReaderResult result;
  wengine::link::PublisherStats stats{};
  double wall = 0.0;
  double cpu = 0.0;
  wengine::world::Snapshot snapshot;

  if (stalled) {
    // Stalled reader: no drain during the flood; attach the drain after
    // and collect whatever the kernel buffer retained.
    {
      wengine::link::UdpPublisher publisher(endpoint, "bench", adapter_for(p));
      if (!publisher.ok()) {
        std::cout << "bench_link_sockets: UDP connect failed\n";
        ::close(recv_fd);
        return 1;
      }
      const double c0 = cpu_seconds();
      const auto t0 = Clock::now();
      for (std::uint64_t i = 1; i <= frames; ++i) {
        fill_snapshot(snapshot, p, i);
        publisher.on_snapshot(snapshot);
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
      cpu = cpu_seconds() - c0;
      stats = publisher.stats();
    }
    std::atomic<bool> stop{false};
    std::thread th([&] {
      udp_reader_loop(recv_fd, "bench", p, send_ns, stop, result);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop.store(true, std::memory_order_relaxed);
    ::shutdown(recv_fd, SHUT_RDWR);
    th.join();
    ::close(recv_fd);
    print_row("udp", label, p, frames, wall, cpu, stats, false, result);
    return 0;
  }

  std::atomic<bool> stop{false};
  std::thread reader([&] {
    udp_reader_loop(recv_fd, "bench", p, send_ns, stop, result);
  });
  {
    wengine::link::UdpPublisher publisher(endpoint, "bench", adapter_for(p));
    if (!publisher.ok()) {
      std::cout << "bench_link_sockets: UDP connect failed\n";
      stop.store(true, std::memory_order_relaxed);
      ::shutdown(recv_fd, SHUT_RDWR);
      reader.join();
      ::close(recv_fd);
      return 1;
    }
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    if (paced) {
      const auto period = std::chrono::nanoseconds(
          static_cast<long long>(1e9 / rate_hz));
      auto deadline = t0;
      for (std::uint64_t i = 1; i <= frames; ++i) {
        deadline += period;
        fill_snapshot(snapshot, p, i);
        if (i < send_ns.size()) {
          send_ns[static_cast<std::size_t>(i)].store(now_ns(),
                                                     std::memory_order_relaxed);
        }
        publisher.on_snapshot(snapshot);
        const auto now = Clock::now();
        if (now < deadline) {
          std::this_thread::sleep_until(deadline);
        }
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
    } else {
      for (std::uint64_t i = 1; i <= frames; ++i) {
        fill_snapshot(snapshot, p, i);
        if (i < send_ns.size()) {
          send_ns[static_cast<std::size_t>(i)].store(now_ns(),
                                                     std::memory_order_relaxed);
        }
        publisher.on_snapshot(snapshot);
      }
      wall = std::chrono::duration<double>(Clock::now() - t0).count();
    }
    cpu = cpu_seconds() - c0;
    stats = publisher.stats();
    // Brief drain so trailing datagrams in flight are observed, then an
    // orderly stop: flag, shutdown to unblock recvfrom, JOIN, then close.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    ::shutdown(recv_fd, SHUT_RDWR);
  }
  reader.join();
  ::close(recv_fd);
  print_row("udp", label, p, frames, wall, cpu, stats, false, result);
  int rc = 0;
  if (result.bad != 0 || result.value_mismatch != 0) {
    std::cout << "bench_link_sockets udp " << label << ": framing/value errors\n";
    rc = 1;
  }
  return rc;
}

int run_fanout(const Pattern &p, std::uint64_t frames) {
  // One snapshot stream into TCP and UDP publishers at once.
  std::uint16_t tcp_port = 0;
  const int listen_fd = make_tcp_listener(tcp_port);
  std::uint16_t udp_port = 0;
  const int udp_fd = make_udp_receiver(udp_port);
  if (listen_fd < 0 || udp_fd < 0) {
    if (listen_fd >= 0) {
      ::close(listen_fd);
    }
    if (udp_fd >= 0) {
      ::close(udp_fd);
    }
    std::cout << "bench_link_sockets fanout: socket setup failed\n";
    return 1;
  }
  std::vector<std::atomic<std::int64_t>> send_ns(
      static_cast<std::size_t>(frames + 1));
  for (auto &a : send_ns) {
    a.store(0, std::memory_order_relaxed);
  }
  ReaderResult tcp_res;
  ReaderResult udp_res;
  std::atomic<bool> udp_stop{false};
  std::thread tcp_th([&] {
    tcp_reader_loop(listen_fd, "bench", p, send_ns, tcp_res);
  });
  std::thread udp_th([&] {
    udp_reader_loop(udp_fd, "bench", p, send_ns, udp_stop, udp_res);
  });
  wengine::link::PublisherStats tcp_stats{};
  wengine::link::PublisherStats udp_stats{};
  double wall = 0.0;
  double cpu = 0.0;
  {
    wengine::link::TcpPublisher tcp("127.0.0.1:" + std::to_string(tcp_port),
                                    "bench", adapter_for(p));
    wengine::link::UdpPublisher udp("127.0.0.1:" + std::to_string(udp_port),
                                    "bench", adapter_for(p));
    if (!tcp.ok() || !udp.ok()) {
      std::cout << "bench_link_sockets fanout: connect failed\n";
      ::shutdown(listen_fd, SHUT_RDWR);
      udp_stop.store(true, std::memory_order_relaxed);
      ::shutdown(udp_fd, SHUT_RDWR);
      tcp_th.join();
      udp_th.join();
      ::close(listen_fd);
      ::close(udp_fd);
      return 1;
    }
    wengine::world::Snapshot snapshot;
    const double c0 = cpu_seconds();
    const auto t0 = Clock::now();
    for (std::uint64_t i = 1; i <= frames; ++i) {
      fill_snapshot(snapshot, p, i);
      if (i < send_ns.size()) {
        send_ns[static_cast<std::size_t>(i)].store(now_ns(),
                                                   std::memory_order_relaxed);
      }
      tcp.on_snapshot(snapshot);
      udp.on_snapshot(snapshot);
    }
    wall = std::chrono::duration<double>(Clock::now() - t0).count();
    cpu = cpu_seconds() - c0;
    tcp_stats = tcp.stats();
    udp_stats = udp.stats();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    udp_stop.store(true, std::memory_order_relaxed);
    ::shutdown(udp_fd, SHUT_RDWR);
  }
  tcp_th.join();
  udp_th.join();
  ::close(listen_fd);
  ::close(udp_fd);
  print_row("tcp", "fanout", p, frames, wall, cpu, tcp_stats, false, tcp_res);
  print_row("udp", "fanout", p, frames, wall, cpu, udp_stats, false, udp_res);
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
  std::cout << "usage: bench_link_sockets [tcp|udp] [frames] [items]\n"
               "       bench_link_sockets [--transport tcp|udp|all] [--frames N] "
               "[--items N] [--payload signal|particle]\n"
               "                          [--mode all|solo|snap|flood|paced|stalled|"
               "fanout] [--rate HZ]\n";
}

}  // namespace

int main(int argc, char **argv) {
  std::string transport = "all";
  std::uint64_t frames = 2000;
  std::size_t items = 64;
  bool is_signal = false;
  std::string mode = "all";
  double rate_hz = 1000.0;

  // Legacy positional form: bench_link_sockets [tcp|udp] [frames] [items].
  int i = 1;
  if (argc > 1 && argv[1][0] != '-') {
    const std::string_view m = argv[1];
    if (m == "tcp" || m == "udp") {
      transport = std::string(m);
      i = 2;
      if (argc > 2 && argv[2][0] != '-') {
        std::uint64_t f = 0;
        if (parse_u64(argv[2], f) && f != 0) {
          frames = f;
        }
        ++i;
        if (argc > 3 && argv[3][0] != '-') {
          std::uint64_t it = 0;
          if (parse_u64(argv[3], it) && it != 0 &&
              it <= wengine::link::kMaxItems) {
            items = static_cast<std::size_t>(it);
          }
          ++i;
        }
      }
      // Legacy run keeps the historic default: flood only on that transport
      // with the particle payload (new flags still refine it below).
      mode = "flood";
    }
  }
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--transport" && i + 1 < argc) {
      transport = argv[++i];
      if (transport != "tcp" && transport != "udp" && transport != "all") {
        usage();
        return 2;
      }
    } else if (a == "--frames" && i + 1 < argc) {
      if (!parse_u64(argv[++i], frames) || frames == 0) {
        usage();
        return 2;
      }
    } else if (a == "--items" && i + 1 < argc) {
      std::uint64_t v = 0;
      if (!parse_u64(argv[++i], v) || v == 0 ||
          v > wengine::link::kMaxItems) {
        usage();
        return 2;
      }
      items = static_cast<std::size_t>(v);
    } else if (a == "--payload" && i + 1 < argc) {
      const std::string_view v = argv[++i];
      if (v == "signal") {
        is_signal = true;
      } else if (v == "particle") {
        is_signal = false;
      } else {
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
    } else {
      usage();
      return 2;
    }
  }

  std::string repro = "bench_link_sockets --transport " + transport +
                      " --frames " + std::to_string(frames) + " --items " +
                      std::to_string(items) + " --payload " +
                      (is_signal ? "signal" : "particle") + " --mode " + mode +
                      " --rate " +
                      std::to_string(static_cast<std::uint64_t>(rate_hz));
  print_header(repro);

  const Pattern pattern =
      make_pattern(is_signal, items, 0x9E3779B9u ^ static_cast<std::uint32_t>(frames));
  const std::uint64_t paced_frames =
      (mode == "all" && frames > 2 * static_cast<std::uint64_t>(rate_hz))
          ? 2 * static_cast<std::uint64_t>(rate_hz)
          : frames;
  const auto want = [&](std::string_view m) {
    return mode == "all" || mode == m;
  };
  const auto want_tr = [&](std::string_view t) {
    return transport == "all" || transport == t;
  };
  int rc = 0;
  if (want("solo")) {
    rc |= run_solo(is_signal, items, frames);
  }
  if (want("snap")) {
    rc |= run_snap(is_signal, items, frames);
  }
  if (want("flood") && want_tr("tcp")) {
    rc |= run_tcp(pattern, frames, rate_hz, false, false, "flood");
  }
  if (want("flood") && want_tr("udp")) {
    rc |= run_udp(pattern, frames, rate_hz, false, false, "flood");
  }
  if (want("paced") && want_tr("tcp")) {
    rc |= run_tcp(pattern, paced_frames, rate_hz, true, false, "paced");
  }
  if (want("paced") && want_tr("udp")) {
    rc |= run_udp(pattern, paced_frames, rate_hz, true, false, "paced");
  }
  if (want("stalled") && want_tr("tcp")) {
    rc |= run_tcp(pattern, frames, rate_hz, false, true, "stalled");
  }
  if (want("stalled") && want_tr("udp")) {
    rc |= run_udp(pattern, frames, rate_hz, false, true, "stalled");
  }
  if (want("fanout")) {
    rc |= run_fanout(pattern, frames);
  }
  return rc;
}
