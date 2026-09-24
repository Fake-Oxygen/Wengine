// TCP/UDP socket publishers (SIG-7): endpoint parsing, hello/version
// gates, compact UDP codec, live loopback round-trips against the engine
// truth, backpressure drops without stalling, and rate-extreme soaks.
// Peers are raw POSIX sockets (readers are Zephyr-side); the shared
// framing helpers under test are the reader contract.

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
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
  const char *bad[] = {"",          "no-colon",  ":8080",  "host:",   "host:0",
                       "host:65536", "host:abc", "host:80x", "host:123456"};
  for (const char *text : bad) {
    assert(!link::parse_endpoint(text).ok());
  }
}

void test_hello_gate() {
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
  link::TcpHello bad_version = good;
  bad_version.version = 999;
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

link::Frame sample_frame() {
  link::Frame f;
  f.step = 7;
  f.time = 0.07;
  link::FrameItem it;
  it.x = 1.5;
  it.vx = -2.5;
  f.items.push_back(it);
  f.channels.push_back({"sig.count", 2.0});
  f.channels.push_back({"sig.0.U", 3.3});
  return f;
}

void test_udp_codec() {
  const link::Frame in = sample_frame();
  std::array<unsigned char, link::kMaxUdpPayload> buf{};
  std::size_t len = 0;
  assert(link::encode_udp(in, "signal", 42, buf.data(), buf.size(), len).ok());
  // Compact: header + 1 item + 2 channels, far below a full LinkFrame.
  assert(len == 80 + 64 + 2 * 40);
  link::Frame back;
  std::uint64_t seq = 0;
  assert(link::decode_udp(buf.data(), len, "signal", back, seq).ok());
  assert(seq == 42 && back == in);

  // Empty frame round-trips (ticks with no rows stay observable).
  const link::Frame empty;
  assert(link::encode_udp(empty, "d", 1, buf.data(), buf.size(), len).ok());
  assert(len == 80);
  link::Frame back_empty;
  assert(link::decode_udp(buf.data(), len, "", back_empty, seq).ok());
  assert(seq == 1 && back_empty == empty);

  // Over-bound inputs refused loudly, outputs untouched.
  link::Frame many = in;
  for (std::size_t k = many.channels.size(); k <= link::kMaxChannels; ++k) {
    many.channels.push_back({"c", 1.0});
  }
  const std::size_t keep_len = len;
  assert(!link::encode_udp(many, "d", 1, buf.data(), buf.size(), len).ok());
  assert(len == keep_len);
  link::Frame long_name = in;
  long_name.channels[0].name.assign(32, 'x');
  assert(!link::encode_udp(long_name, "d", 1, buf.data(), buf.size(), len).ok());
  assert(!link::encode_udp(in, "", 1, buf.data(), buf.size(), len).ok());
  assert(!link::encode_udp(in, "d", 1, nullptr, buf.size(), len).ok());
  assert(!link::encode_udp(in, "d", 1, buf.data(), 79, len).ok()); // tiny cap

  // Payload-size boundary: 1022 items fit, 1023 do not (80 + N*64 ≤ 65507).
  link::Frame boundary;
  for (std::size_t k = 0; k < 1022; ++k) {
    boundary.items.push_back(link::FrameItem{});
  }
  assert(link::encode_udp(boundary, "d", 1, buf.data(), buf.size(), len).ok());
  assert(len == 80 + 1022 * 64);
  boundary.items.push_back(link::FrameItem{});
  assert(!link::encode_udp(boundary, "d", 1, buf.data(), buf.size(), len).ok());

  // Decode refusals: short buffer, bad magic/version, domain problems,
  // count/length mismatch, over-limit counts, unterminated names.
  link::Frame out;
  assert(link::encode_udp(in, "signal", 9, buf.data(), buf.size(), len).ok());
  assert(!link::decode_udp(buf.data(), 79, "signal", out, seq).ok());
  assert(!link::decode_udp(nullptr, len, "signal", out, seq).ok());
  std::array<unsigned char, link::kMaxUdpPayload> bad{};
  std::memcpy(bad.data(), buf.data(), len);
  bad[0] ^= 0xFFu;
  assert(!link::decode_udp(bad.data(), len, "signal", out, seq).ok());
  std::memcpy(bad.data(), buf.data(), len);
  bad[4] ^= 0xFFu;
  assert(!link::decode_udp(bad.data(), len, "signal", out, seq).ok());
  assert(!link::decode_udp(buf.data(), len, "other", out, seq).ok());
  assert(!link::decode_udp(buf.data(), len - 1, "signal", out, seq).ok());
  assert(!link::decode_udp(buf.data(), len + 1, "signal", out, seq).ok());
  std::memcpy(bad.data(), buf.data(), len);
  for (std::size_t i = 8; i < 8 + link::kDomainSize; ++i) {
    bad[i] = 'x'; // unterminated domain
  }
  assert(!link::decode_udp(bad.data(), len, "", out, seq).ok());
  std::memcpy(bad.data(), buf.data(), len);
  bad[64] += 1; // item count no longer matches the length
  assert(!link::decode_udp(bad.data(), len, "signal", out, seq).ok());
  std::memcpy(bad.data(), buf.data(), len);
  // Channel name loses its NUL only when all 32 bytes are non-NUL.
  for (std::size_t i = 0; i < link::kChannelNameSize; ++i) {
    bad[80 + 64 + i] = 'y';
  }
  assert(!link::decode_udp(bad.data(), len, "signal", out, seq).ok());
}

void test_tcp_round_trip() {
  // Live loopback: hello gate + every emitted frame bit-exact to the
  // engine truth recorded beside the publisher.
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
    const std::uint32_t frame_len = link::socket_detail::get_u32_le(prefix);
    assert(frame_len == sizeof(link::LinkFrame));
    link::LinkFrame wire{};
    assert(recv_all(peer, reinterpret_cast<unsigned char *>(&wire), frame_len));
    link::Frame got;
    assert(link::decode_frame(wire, got).ok());
    const link::Frame want = link::frame_from_snapshot(rec.snaps[k]);
    assert(got == want);
    assert(wire.frame_seq == k + 1 && wire.step_index == k + 1);
  }
  ::close(peer);
  ::close(listen_fd);
}

void test_tcp_backpressure_drops_without_stalling() {
  // Accepted but never drained: the kernel buffers fill, the publisher
  // drops loudly (counted, not sticky) and the loop never blocks.
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  constexpr std::uint64_t kAttempts = 3000; // ~200 MB: far past any buffer
  int peer = -1;
  std::uint64_t published = 0;
  std::uint64_t dropped_send = 0;
  bool pending_frame = false;
  {
    link::TcpPublisher pub(endpoint_for(port), "projectile");
    assert(pub.ok());
    peer = ::accept(listen_fd, nullptr, nullptr);
    assert(peer >= 0); // held open, never read

    world::Snapshot snap; // empty frame: valid ticks, smallest encode
    for (std::uint64_t k = 1; k <= kAttempts; ++k) {
      snap.step_index = k;
      snap.sim_time = 0.01 * static_cast<double>(k);
      pub.on_snapshot(snap);
    }
    assert(pub.ok()); // transient drops never poison
    published = pub.stats().published;
    dropped_send = pub.stats().dropped_send;
    pending_frame = pub.has_pending_frame();
    assert(published + dropped_send +
               static_cast<std::uint64_t>(pending_frame) ==
           kAttempts);
    assert(dropped_send > 0);
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
  assert((bytes.size() % link::kTcpFrameBytes != 0) == pending_frame);
  const std::uint64_t received = bytes.size() / link::kTcpFrameBytes;
  assert(received == published);
  link::LagStats lag;
  std::uint64_t prev_seq = 0;
  for (std::uint64_t k = 0; k < received; ++k) {
    const unsigned char *p = bytes.data() + k * link::kTcpFrameBytes;
    assert(link::socket_detail::get_u32_le(p) == sizeof(link::LinkFrame));
    link::LinkFrame wire{};
    std::memcpy(&wire, p + 4, sizeof(wire));
    assert(wire.frame_seq > prev_seq);
    prev_seq = wire.frame_seq;
    (void)link::lag_observe(lag, wire.frame_seq);
  }
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

  link::LagStats lag;
  std::array<unsigned char, link::kMaxUdpPayload> buf{};
  for (std::size_t k = 0; k < 5; ++k) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const ssize_t n = ::recvfrom(receiver, buf.data(), buf.size(), 0,
                                 reinterpret_cast<sockaddr *>(&from), &from_len);
    assert(n > 0);
    link::Frame got;
    std::uint64_t seq = 0;
    assert(link::decode_udp(buf.data(), static_cast<std::size_t>(n), "signal", got,
                            seq)
               .ok());
    assert(seq == k + 1);
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
    link::LinkFrame wire{};
    assert(recv_all(peer, reinterpret_cast<unsigned char *>(&wire), sizeof(wire)));
    link::Frame got;
    assert(link::decode_frame(wire, got).ok());
    assert(got == link::frame_from_snapshot(rec.snaps[k]));
    assert(wire.step_index == k + 1);
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
    unsigned char hello[sizeof(link::TcpHello)]{};
    if (!recv_all(peer, hello, sizeof(hello))) {
      return;
    }
    if (!link::check_tcp_hello(*reinterpret_cast<link::TcpHello *>(hello),
                               "signal")
             .ok()) {
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
    pending_frame = pub.has_pending_frame();
  } // publisher dtor closes the socket: FIN terminates the drain
  reader.join();
  ::close(peer);
  ::close(listen_fd);

  assert(published + dropped_send +
             static_cast<std::uint64_t>(pending_frame) ==
         kSteps); // exact accounting, no stall
  assert(dropped_bound == 0);
  assert(drain.hello_ok);
  assert((drain.bytes.size() % link::kTcpFrameBytes != 0) == pending_frame);
  const std::uint64_t received = drain.bytes.size() / link::kTcpFrameBytes;
  assert(received == published); // TCP delivers every sent byte

  // Every received frame decodes bit-exact to its recorded snapshot.
  std::uint64_t prev_seq = 0;
  link::LagStats lag;
  for (std::uint64_t k = 0; k < received; ++k) {
    const unsigned char *p = drain.bytes.data() + k * link::kTcpFrameBytes;
    assert(link::socket_detail::get_u32_le(p) == sizeof(link::LinkFrame));
    link::LinkFrame wire{};
    std::memcpy(&wire, p + 4, sizeof(wire));
    assert(wire.frame_seq > prev_seq); // strictly increasing, never reordered
    prev_seq = wire.frame_seq;
    (void)link::lag_observe(lag, wire.frame_seq);
    assert(wire.step_index >= 1 && wire.step_index <= kSteps);
    link::Frame got;
    assert(link::decode_frame(wire, got).ok());
    const link::Frame want =
        link::frame_from_signal_snapshot(rec.snaps[wire.step_index - 1]);
    assert(got == want);
  }
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

  link::LagStats lag;
  std::uint64_t previous = 0;
  for (const auto &packet : drain.packets) {
    link::Frame got;
    std::uint64_t seq = 0;
    assert(link::decode_udp(packet.data(), packet.size(), "signal", got, seq)
               .ok());
    assert(seq > previous && seq <= kSteps);
    previous = seq;
    assert(got == link::frame_from_signal_snapshot(rec.snaps[seq - 1]));
    (void)lag_observe(lag, seq);
  }
  assert(lag.received > 0 && lag.missed + lag.received == previous);
  ::close(receiver);
}

} // namespace

int main() {
  test_parse_endpoint();
  test_hello_gate();
  test_udp_codec();
  test_tcp_round_trip();
  test_tcp_backpressure_drops_without_stalling();
  test_over_bound_poison();
  test_udp_round_trip();
  test_subhz_tcp();
  test_soak_100khz_tcp();
  test_udp_soak();
  std::cout << "test_link_sockets: OK\n";
  return 0;
}
