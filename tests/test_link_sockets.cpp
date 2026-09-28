// TCP/UDP socket publishers (SIG-7): endpoint parsing, hello/version
// gates, compact UDP codec, live loopback round-trips against the engine
// truth, backpressure drops without stalling, and rate-extreme soaks.
// Peers are raw POSIX sockets (readers are Zephyr-side); the shared
// framing helpers under test are the reader contract.

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/compact_codec.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

constexpr int kIoTimeoutSec = 10;

void set_recv_timeout(int fd) {
  timeval tv{};
  tv.tv_sec = kIoTimeoutSec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Loopback TCP listener on an ephemeral port; fills port_out.
int make_listener(std::uint16_t &port_out) {
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
  port_out = ntohs(addr.sin_port);
  return fd;
}

// Loopback UDP receiver on an ephemeral port; fills port_out.
int make_udp_receiver(std::uint16_t &port_out) {
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
  port_out = ntohs(addr.sin_port);
  set_recv_timeout(fd);
  return fd;
}

std::string endpoint_for(std::uint16_t port) {
  return "127.0.0.1:" + std::to_string(port);
}

// Blocking read of exactly len bytes (SO_RCVTIMEO bounds stalls).
bool recv_all(int fd, unsigned char *buf, std::size_t len) {
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

struct Recorder : public emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

core::RunConfig projectile_cfg(double dt, std::uint64_t steps, std::uint64_t emit_every) {
  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = emit_every;
  cfg.steps = steps;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  return cfg;
}

core::RunConfig signal_cfg(double dt, std::uint64_t steps) {
  core::RunConfig cfg;
  cfg.loop.dt = dt;
  cfg.loop.emit_every = 1;
  cfg.steps = steps;
  cfg.solver = "signal";
  cfg.particles = 0;
  return cfg;
}

void test_parse_endpoint() {
  {
    const auto r = link::parse_endpoint("127.0.0.1:8080");
    assert(r.ok() && r.value.host == "127.0.0.1" && r.value.port == 8080);
  }
  {
    const auto r = link::parse_endpoint("example.host:1");
    assert(r.ok() && r.value.host == "example.host" && r.value.port == 1);
  }
  {
    const auto r = link::parse_endpoint("[::1]:65535");
    assert(r.ok() && r.value.host == "::1" && r.value.port == 65535);
  }
  {
    const auto r = link::parse_endpoint("[2001:db8::1]:9000");
    assert(r.ok() && r.value.host == "2001:db8::1" && r.value.port == 9000);
  }
  const char *bad[] = {"",          "no-colon",  ":8080",  "host:",   "host:0",
                       "host:65536", "host:abc", "host:80x", "host:123456",
                       "[::1]",     "[::1]:",   "[]:9000"};
  for (const char *text : bad) {
    assert(!link::parse_endpoint(text).ok());
  }
}

void test_hello_gate() {
  // Exact magic/schema/domain; garbage, foreign magics and future schemas
  // refuse loudly at attach, never mid-stream.
  static_assert(sizeof(link::TcpHello) == 40);
  link::TcpHello good{};
  good.magic = link::kTcpMagic;
  good.version = link::kTcpVersion;
  const char *tag = "projectile";
  for (std::size_t i = 0; tag[i] != '\0'; ++i) {
    good.domain[i] = tag[i];
  }
  assert(link::check_tcp_hello(good, "projectile").ok());

  link::TcpHello bad_magic = good;
  bad_magic.magic = 0;
  assert(!link::check_tcp_hello(bad_magic, "projectile").ok());
  link::TcpHello foreign_magic = good;
  foreign_magic.magic = 0x31544C57u; // retired "WLT1": refused, never parsed
  assert(!link::check_tcp_hello(foreign_magic, "projectile").ok());
  link::TcpHello bad_version = good;
  bad_version.version = 999; // future schema: refused, never guessed
  assert(!link::check_tcp_hello(bad_version, "projectile").ok());
  link::TcpHello bad_domain = good;
  bad_domain.domain[0] = 'x';
  assert(!link::check_tcp_hello(bad_domain, "projectile").ok());
  assert(!link::check_tcp_hello(good, "signal").ok()); // exact domain match
  assert(!link::check_tcp_hello(good, "").ok());
  link::TcpHello unterminated = good;
  for (std::size_t i = 0; i < link::kDomainSize; ++i) {
    unterminated.domain[i] = 'x'; // no NUL anywhere: corrupt
  }
  assert(!link::check_tcp_hello(unterminated, "projectile").ok());
}

void test_tcp_round_trip() {
  // Live loopback: hello gate + every emitted compact record bit-exact to
  // the engine truth recorded beside the publisher. Variable lengths carry
  // exactly the used entries; run_id is stable across the run.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpPublisher pub(endpoint_for(port), "projectile");
  assert(pub.ok());
  const std::uint64_t run_id = pub.run_id();
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0);
  set_recv_timeout(peer);

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&pub, &rec};
  assert(solvers::run_headless(projectile_cfg(0.01, 5, 1), sinks, final_state).ok());
  assert(pub.ok());
  assert(pub.stats().published == 5);
  assert(pub.stats().dropped_send == 0 && pub.stats().dropped_over_bound == 0);

  link::TcpHello hello{};
  assert(recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello)));
  assert(link::check_tcp_hello(hello, "projectile").ok());

  for (std::size_t k = 0; k < 5; ++k) {
    unsigned char prefix[4]{};
    assert(recv_all(peer, prefix, sizeof(prefix)));
    const std::uint32_t rec_len = link::socket_detail::get_u32_le(prefix);
    const link::Frame want = link::frame_from_snapshot(rec.snaps[k]);
    assert(rec_len == link::compact::encoded_size(want)); // used entries only
    std::vector<unsigned char> rec_bytes(rec_len);
    assert(recv_all(peer, rec_bytes.data(), rec_len));
    link::Frame got;
    std::uint64_t got_run = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(rec_bytes.data(), rec_len, "projectile", got,
                                        got_run, seq, kind)
               .ok());
    assert(got == want);
    assert(got_run == run_id && seq == k + 1);
    assert(kind == link::compact::RecordKind::kFrame);
    assert(got.step == k + 1);
  }
  ::close(peer);
  ::close(listen_fd);
}

void test_tcp_backpressure_drops_without_stalling() {
  // Accepted but never drained: the kernel buffers fill, the publisher
  // drops loudly (counted, not sticky) and the loop never blocks. The
  // received byte stream walks as variable-length records landing exactly
  // on its end — every sent byte arrives, framing intact.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  constexpr std::uint64_t kAttempts = 20000; // ~2 MB compact: past any buffer
  int peer = -1;
  std::uint64_t published = 0;
  std::uint64_t dropped_send = 0;
  bool pending_frame = false;
  {
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0); // held open, never read

    world::Snapshot snap; // empty state: smallest valid encode (~100 B)
    for (std::uint64_t k = 1; k <= kAttempts; ++k) {
      snap.step_index = k;
      snap.sim_time = 0.01 * static_cast<double>(k);
      pub.on_snapshot(snap);
    }
    assert(pub.ok()); // transient drops never poison
    published = pub.stats().published;
    dropped_send = pub.stats().dropped_send;
    pending_frame = pub.has_pending_frame();
    assert(published + dropped_send + pub.stats().dropped_pending_blocked +
               static_cast<std::uint64_t>(pending_frame) ==
           kAttempts);
    assert(pub.stats().attempted == kAttempts);
    assert(pub.stats().disabled_after_error == 0);
    assert(link::output_accounting_ok(pub.stats(), pending_frame ? 1 : 0));
    assert(pub.stats().dropped_send + pub.stats().dropped_pending_blocked > 0);
    assert(pub.stats().dropped_over_bound == 0);
  }
  set_recv_timeout(peer);
  link::TcpHello hello{};
  assert(recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello)));
  assert(link::check_tcp_hello(hello, "projectile").ok());
  std::vector<unsigned char> bytes;
  std::array<unsigned char, 65536> chunk{};
  while (true) {
    const ssize_t n = ::recv(peer, chunk.data(), chunk.size(), 0);
    if (n <= 0) {
      break; // publisher close after all complete kernel-buffered bytes
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + n);
  }
  link::LagStats lag;
  std::uint64_t prev_seq = 0;
  std::uint64_t received = 0;
  std::size_t cursor = 0;
  while (cursor + 4 <= bytes.size()) {
    const std::uint32_t rec_len =
        link::socket_detail::get_u32_le(bytes.data() + cursor);
    assert(rec_len >= link::compact::kHeaderSize);
    assert(rec_len <= link::compact::kMaxRecordBytes);
    if (cursor + 4 + rec_len > bytes.size()) {
      break; // trailing partial record: only possible while one is pending
    }
    link::Frame got;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(bytes.data() + cursor + 4, rec_len,
                                        "projectile", got, run_id, seq, kind)
               .ok());
    assert(seq > prev_seq);
    prev_seq = seq;
    (void)link::lag_observe(lag, seq);
    ++received;
    cursor += 4 + rec_len;
  }
  assert((cursor < bytes.size()) == pending_frame);
  assert(received == published); // TCP delivers every sent byte
  assert(lag.received == received);
  ::close(peer);
  ::close(listen_fd);
}

link::Frame too_many_channels(const world::Snapshot &) {
  link::Frame f;
  for (std::size_t k = 0; k <= link::kMaxChannels; ++k) {
    f.channels.push_back({"c", 1.0});
  }
  return f;
}

// Fixed probe frame for the budget tests: each test sets its channels, then
// publish through this adapter so the encoded size is exactly known.
link::Frame g_budget_frame;

link::Frame budget_frame_adapter(const world::Snapshot &s) {
  link::Frame f = g_budget_frame;
  f.step = s.step_index;
  f.time = s.sim_time;
  return f;
}

void test_over_bound_poison() {
  // Over-bound frames are refused identically on both transports: loud
  // kOutOfRange, counted, sticky (mirrors SHM).
  {
    std::uint16_t port = 0;
    const int listen_fd = make_listener(port);
    assert(listen_fd >= 0);
    link::TcpPublisher pub(endpoint_for(port), "d", &too_many_channels);
    assert(pub.ok());
    world::Snapshot snap;
    pub.on_snapshot(snap);
    assert(!pub.ok() && pub.status().code == core::StatusCode::kOutOfRange);
    assert(pub.stats().published == 0 && pub.stats().dropped_over_bound == 1);
    ::close(listen_fd);
  }
  {
    link::UdpPublisher pub("127.0.0.1:9", "d", &too_many_channels);
    assert(pub.ok());
    world::Snapshot snap;
    pub.on_snapshot(snap);
    assert(!pub.ok() && pub.status().code == core::StatusCode::kOutOfRange);
    assert(pub.stats().published == 0 && pub.stats().dropped_over_bound == 1);
  }
}

void test_udp_round_trip() {
  std::uint16_t port = 0;
  const int receiver = make_udp_receiver(port);
  assert(receiver >= 0);
  link::UdpPublisher pub(endpoint_for(port), "signal",
                         &link::frame_from_signal_snapshot);
  assert(pub.ok());

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&pub, &rec};
  assert(solvers::run_headless(signal_cfg(2.0, 5), sinks, final_state).ok());
  assert(pub.ok());
  assert(pub.stats().published == 5 && pub.stats().dropped_send == 0);

  const std::uint64_t run_id = pub.run_id();
  link::LagStats lag;
  std::array<unsigned char, link::kMaxUdpPayload> buf{};
  for (std::size_t k = 0; k < 5; ++k) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(receiver, buf.data(), buf.size(), 0,
                                 reinterpret_cast<sockaddr *>(&from), &from_len);
    assert(n > 0);
    link::Frame got;
    std::uint64_t got_run = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(buf.data(), static_cast<std::size_t>(n),
                                        "signal", got, got_run, seq, kind)
               .ok());
    assert(seq == k + 1 && got_run == run_id);
    assert(kind == link::compact::RecordKind::kFrame);
    assert(got == link::frame_from_signal_snapshot(rec.snaps[k]));
    (void)lag_observe(lag, seq);
  }
  assert(lag.received == 5 && lag.missed == 0);
  ::close(receiver);
}

void test_subhz_tcp() {
  // Sub-Hz rate (dt = 2 s): content-exact, no rate assumptions anywhere.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpPublisher pub(endpoint_for(port), "projectile");
  assert(pub.ok());
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0);
  set_recv_timeout(peer);

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&pub, &rec};
  assert(solvers::run_headless(projectile_cfg(2.0, 3, 1), sinks, final_state).ok());
  assert(pub.stats().published == 3);

  link::TcpHello hello{};
  assert(recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello)));
  assert(link::check_tcp_hello(hello, "projectile").ok());
  for (std::size_t k = 0; k < 3; ++k) {
    unsigned char prefix[4]{};
    assert(recv_all(peer, prefix, sizeof(prefix)));
    const std::uint32_t rec_len = link::socket_detail::get_u32_le(prefix);
    std::vector<unsigned char> rec_bytes(rec_len);
    assert(recv_all(peer, rec_bytes.data(), rec_len));
    link::Frame got;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(rec_bytes.data(), rec_len, "projectile", got,
                                        run_id, seq, kind)
               .ok());
    assert(got == link::frame_from_snapshot(rec.snaps[k]));
    assert(got.step == k + 1 && seq == k + 1);
  }
  ::close(peer);
  ::close(listen_fd);
}

// 100 kHz-class soak: 2000 steps at dt = 10 us with a concurrently
// draining reader. Kernel buffers cannot hold the flood, so publisher-side
// drops are EXPECTED here — the test proves the honest contract instead:
// the loop never blocks, published + dropped == attempts exactly, every
// byte the kernel accepted arrives, and every received frame decodes
// bit-exact to its recorded snapshot (matched by step_index).
// Termination is deterministic, not timeout-based: the publisher is
// destroyed after the run, its FIN orders after all sent bytes, and the
// reader exits on EOF. (A 30 s recv timeout only converts a genuine stall
// into a clean failure instead of a hang.)
struct TcpDrain {
  int peer = -1;
  std::vector<unsigned char> bytes; // raw stream after the hello
  bool hello_ok = false;
  void run() {
    link::TcpHello hello{};
    if (!recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello))) {
      return;
    }
    if (!link::check_tcp_hello(hello, "signal").ok()) {
      return;
    }
    hello_ok = true;
    unsigned char chunk[65536]{};
    while (true) {
      const ssize_t n = ::recv(peer, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        return; // EOF (publisher closed) or stall timeout
      }
      bytes.insert(bytes.end(), chunk, chunk + n);
    }
  }
};

void test_soak_100khz_tcp() {
  constexpr std::uint64_t kSteps = 2000;
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);

  TcpDrain drain;
  std::thread reader;
  int peer = -1;
  std::uint64_t published = 0;
  std::uint64_t dropped_send = 0;
  std::uint64_t dropped_bound = 0;
  std::uint64_t dropped_blocked = 0;
  std::uint64_t attempted = 0;
  bool pending_frame = false;
  Recorder rec;
  {
    link::TcpPublisher pub(endpoint_for(port), "signal",
                           &link::frame_from_signal_snapshot);
    assert(pub.ok());
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0);
    // Stall guard only (30 s): EOF from the publisher dtor terminates the
    // drain; a timeout here means the loop genuinely stalled.
    timeval tv{};
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    drain.peer = peer;
    reader = std::thread([&drain] { drain.run(); });

    world::State final_state;
    std::array<emit::ISink *, 2> sinks{&pub, &rec};
    assert(solvers::run_headless(signal_cfg(0.00001, kSteps), sinks, final_state).ok());
    assert(pub.ok()); // drops never poison
    published = pub.stats().published;
    dropped_send = pub.stats().dropped_send;
    dropped_bound = pub.stats().dropped_over_bound;
    dropped_blocked = pub.stats().dropped_pending_blocked;
    attempted = pub.stats().attempted;
    pending_frame = pub.has_pending_frame();
  } // publisher dtor closes the socket: FIN terminates the drain
  reader.join();
  ::close(peer);
  ::close(listen_fd);

  assert(attempted == kSteps);
  assert(published + dropped_send + dropped_blocked +
             static_cast<std::uint64_t>(pending_frame) ==
         kSteps); // exact accounting, no stall
  assert(dropped_bound == 0);
  assert(drain.hello_ok);

  // Every received record decodes bit-exact to its recorded snapshot; the
  // walk lands exactly on the stream end unless a record is pending.
  std::uint64_t prev_seq = 0;
  std::uint64_t received = 0;
  std::size_t cursor = 0;
  link::LagStats lag;
  while (cursor + 4 <= drain.bytes.size()) {
    const std::uint32_t rec_len =
        link::socket_detail::get_u32_le(drain.bytes.data() + cursor);
    assert(rec_len >= link::compact::kHeaderSize);
    assert(rec_len <= link::compact::kMaxRecordBytes);
    if (cursor + 4 + rec_len > drain.bytes.size()) {
      break; // trailing partial record: only possible while one is pending
    }
    link::Frame got;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(drain.bytes.data() + cursor + 4, rec_len,
                                        "signal", got, run_id, seq, kind)
               .ok());
    assert(seq > prev_seq); // strictly increasing, never reordered
    prev_seq = seq;
    (void)link::lag_observe(lag, seq);
    assert(got.step >= 1 && got.step <= kSteps);
    const link::Frame want = link::frame_from_signal_snapshot(rec.snaps[got.step - 1]);
    assert(got == want);
    ++received;
    cursor += 4 + rec_len;
  }
  assert((cursor < drain.bytes.size()) == pending_frame);
  assert(received == published); // TCP delivers every sent byte
  if (dropped_send > 0) {
    assert(lag.missed > 0); // receiver observes publisher-side drops as seq gaps
  }
}

void test_udp_soak() {
  // Compact datagrams at a 100 kHz-class simulated rate with a concurrent
  // reader, so the test checks the transport rather than receive-buffer size.
  constexpr std::uint64_t kSteps = 1000;
  std::uint16_t port = 0;
  const int receiver = make_udp_receiver(port);
  assert(receiver >= 0);
  link::UdpPublisher pub(endpoint_for(port), "signal",
                         &link::frame_from_signal_snapshot);
  assert(pub.ok());

  struct UdpDrain {
    int fd = -1;
    std::vector<std::vector<unsigned char>> packets;
    void run() {
      std::array<unsigned char, link::kMaxUdpPayload> buf{};
      for (;;) {
        const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), 0, nullptr, nullptr);
        if (n <= 0) {
          return;
        }
        packets.emplace_back(buf.begin(), buf.begin() + n);
      }
    }
  } drain{receiver};
  timeval tv{};
  tv.tv_sec = 1;
  ::setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::thread reader([&drain] { drain.run(); });

  world::State final_state;
  Recorder rec;
  std::array<emit::ISink *, 2> sinks{&pub, &rec};
  assert(solvers::run_headless(signal_cfg(0.00001, kSteps), sinks, final_state).ok());
  assert(pub.ok());
  assert(pub.stats().published == kSteps && pub.stats().dropped_send == 0);
  reader.join();

  const std::uint64_t run_id = pub.run_id();
  link::LagStats lag;
  std::uint64_t previous = 0;
  for (const auto &packet : drain.packets) {
    link::Frame got;
    std::uint64_t got_run = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(packet.data(), packet.size(), "signal", got,
                                        got_run, seq, kind)
               .ok());
    assert(seq > previous && seq <= kSteps && got_run == run_id);
    previous = seq;
    assert(got == link::frame_from_signal_snapshot(rec.snaps[seq - 1]));
    (void)lag_observe(lag, seq);
  }
  assert(lag.received > 0 && lag.missed + lag.received == previous);
  ::close(receiver);
}

// PHYSI-32: pending TCP bytes progress while the producer is idle (no dummy
// snapshot), then complete once the reader resumes; shutdown finish() is
// bounded and explicit; timeouts and dead peers never claim success.

void test_service_pending_idle_no_dummy_snapshot() {
  // Stalled peer: flood until a partial frame is held, prove an idle
  // service_pending() neither counts a drop nor advances the stream, and
  // the pending tail stays explicitly reportable (abandoned at close).
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  int peer = -1;
  {
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0); // held open, never read: backpressure is forced

    world::Snapshot snap;
    bool got_pending = false;
    for (std::uint64_t k = 1; k <= 5000; ++k) {
      snap.step_index = k;
      snap.sim_time = 0.01 * static_cast<double>(k);
      pub.on_snapshot(snap);
      if (pub.has_pending_frame()) {
        got_pending = true;
        break;
      }
    }
    assert(got_pending);
    assert(pub.ok());
    assert(pub.pending_bytes() > 0);
    const std::uint64_t published_before = pub.stats().published;
    const std::uint64_t dropped_before = pub.stats().dropped_send;

    // Idle service with no new snapshot: kernel still full, so the tail
    // stays pending, no drop is counted, nothing is published.
    assert(!pub.service_pending());
    assert(pub.has_pending_frame());
    assert(pub.pending_bytes() > 0);
    assert(pub.ok());
    assert(pub.stats().published == published_before);
    assert(pub.stats().dropped_send == dropped_before);
    // finish(0) is one non-blocking attempt: same verdict, still explicit.
    assert(!pub.finish(0).ok());
    assert(pub.has_pending_frame());
    assert(pub.ok());
  } // dtor abandons the tail without waiting (bounded shutdown by design)
  ::close(peer);
  ::close(listen_fd);
}

void test_finish_completes_after_reader_resumes() {
  // Flood stalled until pending, then let a concurrent reader drain while
  // finish() waits: the final accepted record must complete framed and
  // decodable, with no truncation and received == published.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);

  struct Drain {
    int peer = -1;
    std::vector<unsigned char> bytes;
    bool hello_ok = false;
    void run() {
      link::TcpHello hello{};
      if (!recv_all(peer, reinterpret_cast<unsigned char *>(&hello), sizeof(hello))) {
        return;
      }
      if (!link::check_tcp_hello(hello, "projectile").ok()) {
        return;
      }
      hello_ok = true;
      unsigned char chunk[65536]{};
      while (true) {
        const ssize_t n = ::recv(peer, chunk, sizeof(chunk), 0);
        if (n <= 0) {
          return; // EOF after the publisher closes
        }
        bytes.insert(bytes.end(), chunk, chunk + n);
      }
    }
  };

  Drain drain;
  std::thread reader;
  int peer = -1;
  std::uint64_t published = 0;
  {
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0);
    timeval tv{};
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    world::Snapshot snap;
    bool got_pending = false;
    for (std::uint64_t k = 1; k <= 5000; ++k) {
      snap.step_index = k;
      snap.sim_time = 0.01 * static_cast<double>(k);
      pub.on_snapshot(snap);
      if (pub.has_pending_frame()) {
        got_pending = true;
        break;
      }
    }
    assert(got_pending);
    assert(pub.pending_bytes() > 0);

    drain.peer = peer;
    reader = std::thread([&drain] { drain.run(); });
    assert(pub.finish(10000).ok()); // reader resumes: tail completes
    assert(!pub.has_pending_frame());
    assert(pub.pending_bytes() == 0);
    assert(pub.ok());
    assert(pub.service_pending()); // idle-clean stays clean
    published = pub.stats().published;
    assert(published > 0);
  } // FIN terminates the drain
  reader.join();
  ::close(peer);
  ::close(listen_fd);

  assert(drain.hello_ok);
  // Variable-length walk landing exactly on the stream end: no truncation,
  // no garbage, every record decodable with increasing seqs.
  std::uint64_t prev_seq = 0;
  std::uint64_t received = 0;
  std::size_t cursor = 0;
  while (cursor < drain.bytes.size()) {
    assert(cursor + 4 <= drain.bytes.size());
    const std::uint32_t rec_len =
        link::socket_detail::get_u32_le(drain.bytes.data() + cursor);
    assert(rec_len >= link::compact::kHeaderSize);
    assert(rec_len <= link::compact::kMaxRecordBytes);
    assert(cursor + 4 + rec_len <= drain.bytes.size());
    link::Frame decoded;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(drain.bytes.data() + cursor + 4, rec_len,
                                        "projectile", decoded, run_id, seq, kind)
               .ok());
    assert(seq > prev_seq);
    prev_seq = seq;
    ++received;
    cursor += 4 + rec_len;
  }
  assert(received == published);
}

void test_finish_timeout_reports_pending() {
  // Never-drained peer: a bounded finish must fail loudly with the tail
  // preserved (never a false success), without poisoning the publisher,
  // and reject a negative deadline as usage error.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpPublisher pub(endpoint_for(port), "projectile");
  assert(pub.ok());
  const int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0); // never read

  world::Snapshot snap;
  bool got_pending = false;
  for (std::uint64_t k = 1; k <= 5000; ++k) {
    snap.step_index = k;
    snap.sim_time = 0.01 * static_cast<double>(k);
    pub.on_snapshot(snap);
    if (pub.has_pending_frame()) {
      got_pending = true;
      break;
    }
  }
  assert(got_pending);
  assert(!pub.finish(50).ok()); // deadline expires: explicit failure
  assert(pub.has_pending_frame());
  assert(pub.pending_bytes() > 0);
  assert(pub.ok()); // timeout is retryable, never sticky
  assert(!pub.finish(-1).ok());
  assert(pub.finish(-1).code == core::StatusCode::kInvalidArgument);
  ::close(peer);
  ::close(listen_fd);
}

void test_finish_disconnect_reports_failure() {
  // Peer goes away: further writes must turn sticky and finish() must
  // never claim the stream completed.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpPublisher pub(endpoint_for(port), "projectile");
  assert(pub.ok());
  int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0);
  ::close(peer); // reader disconnects immediately
  peer = -1;

  world::Snapshot snap;
  bool dead = false;
  for (std::uint64_t k = 1; k <= 5000; ++k) {
    snap.step_index = k;
    snap.sim_time = 0.01 * static_cast<double>(k);
    pub.on_snapshot(snap);
    if (!pub.ok()) {
      dead = true;
      break;
    }
    if (!pub.service_pending() && !pub.ok()) {
      dead = true;
      break;
    }
  }
  assert(dead);
  assert(!pub.ok());
  assert(pub.status().code == core::StatusCode::kInternal);
  assert(!pub.service_pending());
  assert(!pub.finish(100).ok()); // sticky failure, never success
  ::close(listen_fd);
}

} // namespace

link::Frame empty_frame_adapter(const world::Snapshot &) { return link::Frame{}; }

void test_tcp_empty_and_max_records() {
  // Wire extremes over loopback: an empty Frame is exactly a header
  // (84-byte record); a 1024-particle snapshot is the maximum-shape record.
  // Both decode exactly, proving variable framing at both ends.
  {
    std::uint16_t port = 0;
    const int listen_fd = make_listener(port);
    assert(listen_fd >= 0);
    link::TcpPublisher pub(endpoint_for(port), "projectile", &empty_frame_adapter);
    assert(pub.ok());
    const int peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0);
    set_recv_timeout(peer);
    world::Snapshot snap;
    snap.step_index = 3;
    pub.on_snapshot(snap);
    assert(pub.ok() && pub.stats().published == 1);
    unsigned char hello[40]{};
    assert(recv_all(peer, hello, sizeof(hello)));
    assert(link::check_tcp_hello(*reinterpret_cast<link::TcpHello *>(hello),
                                    "projectile")
               .ok());
    unsigned char prefix[4]{};
    assert(recv_all(peer, prefix, sizeof(prefix)));
    assert(link::socket_detail::get_u32_le(prefix) == link::compact::kHeaderSize);
    std::array<unsigned char, link::compact::kHeaderSize> rec{};
    assert(recv_all(peer, rec.data(), rec.size()));
    link::Frame got;
    got.step = 0xBEEF;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(rec.data(), rec.size(), "projectile", got,
                                        run_id, seq, kind)
               .ok());
    assert(got == link::Frame{});
    assert(seq == 1);
    ::close(peer);
    ::close(listen_fd);
  }
  {
    std::uint16_t port = 0;
    const int listen_fd = make_listener(port);
    assert(listen_fd >= 0);
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    const int peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0);
    set_recv_timeout(peer);
    world::Snapshot snap;
    snap.step_index = 7;
    snap.sim_time = 0.07;
    snap.pos_x.resize(1024);
    snap.pos_y.resize(1024);
    snap.vel_x.resize(1024);
    snap.vel_y.resize(1024);
    for (std::size_t k = 0; k < 1024; ++k) {
      snap.pos_x[k] = 0.125 * static_cast<double>(k);
      snap.pos_y[k] = -0.25 * static_cast<double>(k);
      snap.vel_x[k] = 1.0;
      snap.vel_y[k] = 2.0;
    }
    pub.on_snapshot(snap);
    assert(pub.ok() && pub.stats().published == 1);
    unsigned char hello[40]{};
    assert(recv_all(peer, hello, sizeof(hello)));
    unsigned char prefix[4]{};
    assert(recv_all(peer, prefix, sizeof(prefix)));
    const std::uint32_t rec_len = link::socket_detail::get_u32_le(prefix);
    const link::Frame want = link::frame_from_snapshot(snap);
    assert(rec_len == link::compact::encoded_size(want));
    assert(rec_len <= link::compact::kMaxRecordBytes);
    std::vector<unsigned char> rec_bytes(rec_len);
    assert(recv_all(peer, rec_bytes.data(), rec_len));
    link::Frame got;
    std::uint64_t run_id = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(rec_bytes.data(), rec_len, "projectile", got,
                                        run_id, seq, kind)
               .ok());
    assert(got == want);
    assert(run_id == pub.run_id() && seq == 1);
    ::close(peer);
    ::close(listen_fd);
  }
}

void test_tcp_stall_pending_finish() {
  // Stalled reader + variable-length records: exactly one partial record
  // held, accounting identity intact, finish() completes it framed once the
  // reader resumes, and every received record parses with increasing seqs
  // landing exactly on the byte-stream end (no truncation, no garbage).
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);

  struct Drain {
    int peer = -1;
    std::vector<unsigned char> bytes;
    bool hello_ok = false;
    void run() {
      link::TcpHello hello_msg{};
      if (!recv_all(peer, reinterpret_cast<unsigned char *>(&hello_msg),
                    sizeof(hello_msg))) {
        return;
      }
      if (!link::check_tcp_hello(hello_msg, "projectile").ok()) {
        return;
      }
      hello_ok = true;
      unsigned char chunk[65536]{};
      while (true) {
        const ssize_t n = ::recv(peer, chunk, sizeof(chunk), 0);
        if (n <= 0) {
          return; // EOF after the publisher closes
        }
        bytes.insert(bytes.end(), chunk, chunk + n);
      }
    }
  };

  // ~12 KB records x 500 attempts (~6 MB) far past the kernel buffer.
  world::Snapshot snap;
  snap.pos_x.resize(200);
  snap.pos_y.resize(200);
  snap.vel_x.resize(200);
  snap.vel_y.resize(200);
  for (std::size_t k = 0; k < 200; ++k) {
    snap.pos_x[k] = static_cast<double>(k);
  }

  Drain drain;
  std::thread reader;
  int peer = -1;
  std::uint64_t published = 0;
  std::uint64_t run_id = 0;
  {
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    run_id = pub.run_id();
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0); // held open, never read: backpressure is forced
    timeval tv{};
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool got_pending = false;
    std::uint64_t attempts = 0;
    for (std::uint64_t k = 1; k <= 500; ++k) {
      snap.step_index = k;
      snap.sim_time = 0.01 * static_cast<double>(k);
      pub.on_snapshot(snap);
      attempts = k;
      if (pub.has_pending_frame()) {
        got_pending = true;
        break;
      }
    }
    assert(got_pending);
    assert(pub.ok()); // transient drops never poison
    assert(pub.pending_bytes() > 0);
    assert(pub.stats().attempted == attempts);
    assert(pub.stats().published + pub.stats().dropped_send +
               pub.stats().dropped_pending_blocked + 1 ==
           attempts);
    assert(link::output_accounting_ok(pub.stats(), 1));

    drain.peer = peer;
    reader = std::thread([&drain] { drain.run(); });
    assert(pub.finish(30000).ok());
    assert(!pub.has_pending_frame() && pub.ok());
    published = pub.stats().published;
  } // dtor closes: drain thread sees EOF and stops
  reader.join();
  assert(drain.hello_ok);
  std::size_t cursor = 0;
  std::uint64_t prev_seq = 0;
  std::uint64_t received = 0;
  while (cursor < drain.bytes.size()) {
    assert(cursor + 4 <= drain.bytes.size());
    const std::uint32_t rec_len =
        link::socket_detail::get_u32_le(drain.bytes.data() + cursor);
    assert(rec_len >= link::compact::kHeaderSize);
    assert(rec_len <= link::compact::kMaxRecordBytes);
    assert(cursor + 4 + rec_len <= drain.bytes.size());
    link::Frame got;
    std::uint64_t got_run = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(drain.bytes.data() + cursor + 4, rec_len,
                                        "projectile", got, got_run, seq, kind)
               .ok());
    assert(got_run == run_id && seq > prev_seq);
    prev_seq = seq;
    ++received;
    cursor += 4 + rec_len;
  }
  assert(received == published);
  ::close(peer);
  ::close(listen_fd);
}

void test_udp_payload_budget() {
  // Explicit MTU-sized payload budget (PHYSI-41): the default is the
  // conservative 1,200-byte profile, construction rejects out-of-range
  // budgets, exact-fit records publish byte-exact, and one-byte-over
  // records refuse loudly with the needed size recorded.
  static_assert(link::kDefaultUdpPayloadBudget == 1200);
  static_assert(link::kDefaultUdpPayloadBudget <= link::kMaxUdpPayload);
  {
    link::UdpPublisher pub("127.0.0.1:9", "d");
    assert(pub.ok());
    assert(pub.payload_budget() == link::kDefaultUdpPayloadBudget);
    assert(pub.last_over_bound_bytes() == 0);
  }
  {
    link::UdpPublisher zero("127.0.0.1:9", "d", &link::frame_from_snapshot, 0);
    assert(!zero.ok() && zero.status().code == core::StatusCode::kInvalidArgument);
  }
  {
    link::UdpPublisher huge("127.0.0.1:9", "d", &link::frame_from_snapshot,
                            link::kMaxUdpPayload + 1);
    assert(!huge.ok() && huge.status().code == core::StatusCode::kInvalidArgument);
  }
  {
    // Codec over-bound frames have no finite encoded size: need stays 0.
    link::UdpPublisher pub("127.0.0.1:9", "d", &too_many_channels);
    assert(pub.ok());
    world::Snapshot snap;
    pub.on_snapshot(snap);
    assert(!pub.ok() && pub.status().code == core::StatusCode::kOutOfRange);
    assert(pub.stats().published == 0 && pub.stats().dropped_over_bound == 1);
    assert(pub.last_over_bound_bytes() == 0);
  }
  // Nine signal-shaped channels: sig.count + eight sources.
  g_budget_frame = link::Frame{};
  g_budget_frame.channels.push_back({"sig.count", 8.0});
  for (int k = 0; k < 8; ++k) {
    g_budget_frame.channels.push_back({"sig." + std::to_string(k) + ".U", 1.0});
  }
  const std::size_t need = link::compact::encoded_size(g_budget_frame);
  assert(need > 0 && need <= link::kMaxUdpPayload);
  {
    // Exact fit: the datagram IS the record, so bytes == need <= budget.
    std::uint16_t port = 0;
    const int receiver = make_udp_receiver(port);
    assert(receiver >= 0);
    link::UdpPublisher pub(endpoint_for(port), "d", &budget_frame_adapter, need);
    assert(pub.ok() && pub.payload_budget() == need);
    world::Snapshot snap;
    snap.step_index = 4;
    snap.sim_time = 0.04;
    pub.on_snapshot(snap);
    assert(pub.ok());
    assert(pub.stats().published == 1 && pub.stats().dropped_over_bound == 0);
    assert(pub.last_over_bound_bytes() == 0);
    std::array<unsigned char, link::kMaxUdpPayload> buf{};
    const ssize_t n = ::recvfrom(receiver, buf.data(), buf.size(), 0, nullptr, nullptr);
    assert(n > 0 && static_cast<std::size_t>(n) == need);
    assert(static_cast<std::size_t>(n) <= pub.payload_budget());
    link::Frame got;
    std::uint64_t got_run = 0;
    std::uint64_t seq = 0;
    link::compact::RecordKind kind{};
    assert(link::compact::decode_record(buf.data(), static_cast<std::size_t>(n), "d",
                                        got, got_run, seq, kind)
               .ok());
    assert(seq == 1 && got_run == pub.run_id());
    assert(kind == link::compact::RecordKind::kFrame);
    assert(got == budget_frame_adapter(snap));
    ::close(receiver);
  }
  {
    // One byte over: indivisible, refused loudly with need recorded.
    link::UdpPublisher pub("127.0.0.1:9", "d", &budget_frame_adapter, need - 1);
    assert(pub.ok() && pub.payload_budget() == need - 1);
    world::Snapshot snap;
    pub.on_snapshot(snap);
    assert(!pub.ok() && pub.status().code == core::StatusCode::kOutOfRange);
    assert(pub.stats().published == 0 && pub.stats().dropped_over_bound == 1);
    assert(pub.last_over_bound_bytes() == need);
  }
  g_budget_frame = link::Frame{};
}

void test_udp_budget_representative_sizes() {
  // Representative frames fit the default MTU-safe budget: a full 63-source
  // signal batch (sig.count + 63 sources = 64 channels) and a
  // single-particle frame.
  {
    link::Frame full;
    full.channels.push_back({"sig.count", 63.0});
    for (int k = 0; k < 63; ++k) {
      full.channels.push_back({"sig." + std::to_string(k) + ".U", 0.0});
    }
    const std::size_t need = link::compact::encoded_size(full);
    assert(need > 0 && need <= link::kDefaultUdpPayloadBudget);
  }
  {
    world::Snapshot snap;
    snap.pos_x = {1.0};
    snap.pos_y = {2.0};
    snap.vel_x = {3.0};
    snap.vel_y = {4.0};
    const link::Frame f = link::frame_from_snapshot(snap);
    const std::size_t need = link::compact::encoded_size(f);
    assert(need > 0 && need <= link::kDefaultUdpPayloadBudget);
  }
}

int main() {
  test_parse_endpoint();
  test_hello_gate();
  test_tcp_round_trip();
  test_tcp_backpressure_drops_without_stalling();
  test_over_bound_poison();
  test_udp_round_trip();
  test_subhz_tcp();
  test_soak_100khz_tcp();
  test_udp_soak();
  test_service_pending_idle_no_dummy_snapshot();
  test_finish_completes_after_reader_resumes();
  test_finish_timeout_reports_pending();
  test_finish_disconnect_reports_failure();
  test_tcp_empty_and_max_records();
  test_tcp_stall_pending_finish();
  test_udp_payload_budget();
  test_udp_budget_representative_sizes();
  std::cout << "test_link_sockets: OK\n";
  return 0;
}
