// PHYSI-39: live output health separates each cause of dropped data.
//
// Covers the acceptance gaps beyond the publisher-level suites:
// - per-cause PublisherStats separation (attempted / over-bound /
//   backpressure / pending-blocked / pending / abandoned-at-close /
//   disabled-after-error) with exact accounting;
// - shared link::output_report formatting + labeling (local acceptance
//   only; remote receipt needs a receiver LagStats);
// - fault injection: capacity overflow (over-bound) and peer disconnect;
// - telemetry failures leave numerical results unchanged (State +
//   recorded snapshots bit-identical with and without a poisoned sink).

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/socket_publisher.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace wengine;

namespace {

struct Recorder : public emit::ISink {
  std::vector<world::Snapshot> snaps;
  void on_snapshot(const world::Snapshot &s) override { snaps.push_back(s); }
};

link::Frame too_many_channels(const world::Snapshot &) {
  link::Frame f;
  for (std::size_t k = 0; k <= link::kMaxChannels; ++k) {
    f.channels.push_back({"c", 1.0});
  }
  return f;
}

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

void test_report_labels_every_cause() {
  link::PublisherStats s;
  s.attempted = 10;
  s.published = 6;
  s.dropped_over_bound = 1;
  s.dropped_send = 1;
  s.dropped_pending_blocked = 1;
  s.dropped_queue_full = 0;
  s.disabled_after_error = 0;
  s.bytes_accepted = 1234;
  assert(link::output_accounting_ok(s, 1));
  assert(!link::output_accounting_ok(s, 0));
  const std::string line = link::format_publisher_health(
      s, core::Status::success(), 0, 1, 99, 1);
  for (const char *k : {"attempted=10", "published_local=6", "over_bound=1",
                        "queue_full=0", "backpressure=1", "pending_blocked=1",
                        "pending=1/99B", "abandoned=1", "disabled=0",
                        "bytes=1234", "status=ok", "errno=0",
                        "local acceptance only"}) {
    assert(line.find(k) != std::string::npos);
  }
  assert(line.find("ACCOUNTING_MISMATCH") == std::string::npos);
  link::PublisherStats bad = s;
  ++bad.published;
  const std::string bad_line = link::format_publisher_health(
      bad, core::Status::success(), 0, 1, 99, 1);
  assert(bad_line.find("ACCOUNTING_MISMATCH") != std::string::npos);
}

void test_shm_per_cause_and_telemetry_isolation() {
  // Healthy prefix: attempted == published, exact accounting, bytes counted.
  const std::string seg = "/wengine-test-output-health-" + std::to_string(::getpid());
  link::ShmPublisher pub(seg, "projectile");
  assert(pub.ok());
  world::Snapshot snap;
  for (int k = 0; k < 3; ++k) {
    snap.step_index = static_cast<std::uint64_t>(k + 1);
    pub.on_snapshot(snap);
  }
  assert(pub.ok());
  assert(pub.stats().attempted == 3);
  assert(pub.stats().published == 3);
  assert(pub.stats().disabled_after_error == 0);
  assert(pub.stats().bytes_accepted == 3 * sizeof(link::Slot));
  assert(link::output_accounting_ok(pub.stats(), 0));

  // Telemetry failure leaves numerics unchanged: a poisoned SHM publisher
  // (over-bound adapter) beside a recorder vs recorder alone.
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = 5;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  link::ShmPublisher poisoned(seg + "-p", "projectile", &too_many_channels);
  assert(poisoned.ok());
  Recorder with_telemetry;
  Recorder without_telemetry;
  world::State s1;
  world::State s2;
  {
    std::array<emit::ISink *, 2> sinks{&poisoned, &with_telemetry};
    assert(solvers::run_headless(cfg, sinks, s1).ok());
  }
  assert(!poisoned.ok());  // capacity overflow is sticky and loud
  assert(poisoned.stats().dropped_over_bound == 1);
  assert(poisoned.stats().disabled_after_error == 4);  // 1 trigger + 3 post-sticky
  assert(poisoned.stats().attempted == 5);
  assert(link::output_accounting_ok(poisoned.stats(), 0));
  {
    std::array<emit::ISink *, 1> sinks{&without_telemetry};
    assert(solvers::run_headless(cfg, sinks, s2).ok());
  }
  assert(s1.step_index == s2.step_index && s1.sim_time == s2.sim_time);
  assert(s1.particles.live_count == s2.particles.live_count);
  assert(s1.particles.pos_x[0] == s2.particles.pos_x[0]);
  assert(with_telemetry.snaps.size() == without_telemetry.snaps.size());
  for (std::size_t k = 0; k < with_telemetry.snaps.size(); ++k) {
    assert(with_telemetry.snaps[k].step_index == without_telemetry.snaps[k].step_index);
  }
  const std::string line = link::format_publisher_health(
      poisoned.stats(), poisoned.status(), poisoned.last_errno(), 0, 0, 0);
  assert(line.find("over_bound=1") != std::string::npos);
  assert(line.find("disabled=4") != std::string::npos);
  assert(line.find("status=kOutOfRange") != std::string::npos);
}

void test_tcp_disconnect_counts_disabled_and_keeps_numerics() {
  std::uint16_t port = 0;
  const int listen_fd = make_listener(port);
  assert(listen_fd >= 0);
  link::TcpPublisher pub("127.0.0.1:" + std::to_string(port), "projectile");
  assert(pub.ok());
  int peer = ::accept(listen_fd, nullptr, nullptr);
  assert(peer >= 0);
  ::close(peer);  // reader disconnects immediately: fault injection
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
  assert(dead && !pub.ok());
  assert(pub.status().code == core::StatusCode::kInternal);
  assert(pub.stats().attempted > 0);
  assert(pub.stats().disabled_after_error > 0);
  const std::uint64_t pending = pub.has_pending_frame() ? 1 : 0;
  assert(link::output_accounting_ok(pub.stats(), pending));
  const std::string line = link::format_publisher_health(
      pub.stats(), pub.status(), pub.last_errno(), pending,
      pub.pending_bytes(), pending);
  assert(line.find("disabled=") != std::string::npos);
  assert(line.find("status=kInternal") != std::string::npos);
  assert(line.find("local acceptance only") != std::string::npos);
  ::close(listen_fd);

  // Disconnect telemetry never alters the sim: poisoned TCP + recorder vs
  // recorder alone produce identical State.
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.loop.emit_every = 1;
  cfg.steps = 4;
  cfg.solver = "projectile";
  cfg.gravity = 9.81;
  cfg.particles = 1;
  std::uint16_t port2 = 0;
  const int listen2 = make_listener(port2);
  assert(listen2 >= 0);
  link::TcpPublisher dead_pub("127.0.0.1:" + std::to_string(port2), "projectile",
                              &too_many_channels);
  assert(dead_pub.ok());
  Recorder a;
  Recorder b;
  world::State s1;
  world::State s2;
  {
    std::array<emit::ISink *, 2> sinks{&dead_pub, &a};
    assert(solvers::run_headless(cfg, sinks, s1).ok());
  }
  assert(!dead_pub.ok() && dead_pub.stats().dropped_over_bound == 1);
  {
    std::array<emit::ISink *, 1> sinks{&b};
    assert(solvers::run_headless(cfg, sinks, s2).ok());
  }
  assert(s1.particles.pos_x[0] == s2.particles.pos_x[0]);
  assert(a.snaps.size() == b.snaps.size());
  ::close(listen2);
}

void test_udp_over_bound_counts_disabled() {
  link::UdpPublisher pub("127.0.0.1:9", "d", &too_many_channels);
  assert(pub.ok());
  world::Snapshot snap;
  pub.on_snapshot(snap);
  assert(!pub.ok() && pub.status().code == core::StatusCode::kOutOfRange);
  assert(pub.stats().attempted == 1);
  assert(pub.stats().dropped_over_bound == 1);
  pub.on_snapshot(snap);
  assert(pub.stats().attempted == 2);
  assert(pub.stats().disabled_after_error == 1);
  assert(link::output_accounting_ok(pub.stats(), 0));
}

}  // namespace

int main() {
  test_report_labels_every_cause();
  test_shm_per_cause_and_telemetry_isolation();
  test_tcp_disconnect_counts_disabled_and_keeps_numerics();
  test_udp_over_bound_counts_disabled();
  std::cout << "test_output_health: OK\n";
  return 0;
}
