// ROS2 publisher tests: transport-neutral ISink half, no rclcpp.
// Fake RosTransport captures owned copies; the rclcpp TU (BUILD_ROS2=ON)
// is covered by live HIL (ros2_bridge + ros2 topic echo), not here.
#include "wengine/emit/routing.hpp"
#include "wengine/link/output_report.hpp"
#include "wengine/ros2/publisher.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using wengine::link::Frame;
using wengine::ros2::BridgeConfig;
using wengine::ros2::RosPublisher;
using wengine::ros2::RosTransport;
using wengine::ros2::SignalBlock;
using wengine::ros2::TransportOutcome;

struct FakeTransport : public RosTransport {
  struct GroupedCall {
    SignalBlock block;
    Frame particles;
  };
  struct OneCall {
    std::uint64_t serial = 0;
    double value = 0.0;
    std::uint8_t quantity = 0;
    std::uint64_t step = 0;
    double sim_time = 0.0;
  };
  std::vector<GroupedCall> grouped;
  std::vector<OneCall> ones;
  std::vector<Frame> particle_calls;
  // Scripted outcomes (front-popped per transport call, then Accepted).
  std::vector<TransportOutcome> script;

  TransportOutcome next() {
    if (script.empty()) {
      return TransportOutcome::kAccepted;
    }
    const TransportOutcome o = script.front();
    script.erase(script.begin());
    return o;
  }

  TransportOutcome publish_grouped(const SignalBlock &block,
                                   const Frame &particles) noexcept override {
    const TransportOutcome o = next();
    if (o != TransportOutcome::kAccepted) {
      return o;
    }
    grouped.push_back({block, particles});
    return o;
  }

  TransportOutcome publish_one(std::uint64_t serial, double value, std::uint8_t quantity,
                               std::uint64_t step, double sim_time) noexcept override {
    const TransportOutcome o = next();
    if (o != TransportOutcome::kAccepted) {
      return o;
    }
    ones.push_back({serial, value, quantity, step, sim_time});
    return o;
  }

  TransportOutcome publish_particles(const Frame &particles) noexcept override {
    const TransportOutcome o = next();
    if (o != TransportOutcome::kAccepted) {
      return o;
    }
    particle_calls.push_back(particles);
    return o;
  }
};

wengine::world::State make_state_two_sources(wengine::world::Entity &e0,
                                             wengine::world::Entity &e1) {
  wengine::world::State st;
  e0 = st.spawn_entity();
  e1 = st.spawn_entity();
  wengine::signal::SignalParams p0;
  p0.quantity = wengine::signal::QuantityId::kVoltage;
  p0.freq_hz = 1.0;
  assert(wengine::world::configure_source(e0, st.signals, p0, st.sim_time).ok());
  wengine::signal::SignalParams p1 = p0;
  p1.quantity = wengine::signal::QuantityId::kCurrent;
  p1.freq_hz = 2.0;
  assert(wengine::world::configure_source(e1, st.signals, p1, st.sim_time).ok());
  st.particles.attach(e0.index);
  st.particles.pos_x[e0.index] = 1.0;
  st.particles.pos_y[e0.index] = 2.0;
  st.particles.vel_x[e0.index] = 0.5;
  st.particles.vel_y[e0.index] = -0.5;
  return st;
}

void test_topics() {
  const BridgeConfig c;
  assert(wengine::ros2::grouped_signals_topic(c) == "/wengine/signals");
  assert(wengine::ros2::serial_topic(c, 3) == "/wengine/signals/sig_3");
  assert(wengine::ros2::particles_topic(c) == "/wengine/particles");
}

void test_qos_and_stamp() {
  using wengine::ros2::qos_depth;
  using wengine::ros2::qos_is_reliable;
  using wengine::ros2::QosClass;
  assert(!qos_is_reliable(QosClass::kLatestState) && qos_depth(QosClass::kLatestState) == 1);
  assert(qos_is_reliable(QosClass::kSampleStream) && qos_depth(QosClass::kSampleStream) == 10);
  const wengine::ros2::RosStamp z = wengine::ros2::sim_time_to_stamp(0.0);
  assert(z.sec == 0 && z.nanosec == 0);
  const wengine::ros2::RosStamp q = wengine::ros2::sim_time_to_stamp(1.25);
  assert(q.sec == 1 && q.nanosec == 250000000u);
  const wengine::ros2::RosStamp neg = wengine::ros2::sim_time_to_stamp(-1.0);
  assert(neg.sec == 0 && neg.nanosec == 0);
}

void test_grouped_layout() {
  wengine::world::Entity e0{0, 0};
  wengine::world::Entity e1{0, 0};
  wengine::world::State st = make_state_two_sources(e0, e1);
  st.sim_time = 0.5;
  st.step_index = 7;
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  assert(pub.ok());
  pub.on_snapshot(snap);
  assert(pub.ok());
  assert(pub.stats().attempted == 1 && pub.stats().published == 1);
  assert(fake.grouped.size() == 1);
  const SignalBlock &b = fake.grouped[0].block;
  assert(b.step == 7 && b.sim_time == 0.5);
  assert(b.serial.size() == 2 && b.value.size() == 2 && b.quantity.size() == 2);
  assert(b.serial[0] == snap.sig_serial[0] && b.serial[1] == snap.sig_serial[1]);
  assert(b.value[0] == snap.sig_value[0] && b.value[1] == snap.sig_value[1]);
  // Particles ride along via frame_from_snapshot.
  assert(fake.grouped[0].particles.items.size() == 1);
  assert(fake.grouped[0].particles.items[0].x == 1.0);
  assert(wengine::link::output_accounting_ok(pub.stats(), 0));
}

void test_owned_copy() {
  wengine::world::Entity e0{0, 0};
  wengine::world::Entity e1{0, 0};
  wengine::world::State st = make_state_two_sources(e0, e1);
  wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  pub.on_snapshot(snap);
  // Mutate the source snapshot after return: the capture must not follow.
  snap.sig_value[0] = -12345.0;
  snap.sig_serial.clear();
  assert(fake.grouped.size() == 1);
  assert(fake.grouped[0].block.serial.size() == 2);
  assert(fake.grouped[0].block.value[0] != -12345.0);
}

void test_per_serial_survives_respawn() {
  wengine::world::Entity e0{0, 0};
  wengine::world::Entity e1{0, 0};
  wengine::world::State st = make_state_two_sources(e0, e1);
  FakeTransport fake;
  BridgeConfig cfg;
  cfg.grouped_signals = false;
  RosPublisher pub(cfg, fake);
  wengine::world::Snapshot s0 = wengine::world::take_snapshot(st);
  pub.on_snapshot(s0);
  assert(fake.ones.size() == 2 && fake.particle_calls.size() == 1);
  assert(fake.ones[0].serial == 0 && fake.ones[1].serial == 1);
  assert(fake.ones[0].step == s0.step_index);
  // Despawn serial 0, spawn a replacement: new serial, new topic; the old
  // topic goes silent and never carries another serial.
  assert(st.despawn_entity(e0));
  const wengine::world::Entity e2 = st.spawn_entity();
  wengine::signal::SignalParams p;
  assert(wengine::world::configure_source(e2, st.signals, p, st.sim_time).ok());
  wengine::world::Snapshot s1 = wengine::world::take_snapshot(st);
  // Slot 0 recycled by the respawn: slot order is [new serial, 1].
  assert(s1.sig_serial.size() == 2);
  assert(s1.sig_serial[0] == 2 && s1.sig_serial[1] == 1);
  fake.ones.clear();
  fake.particle_calls.clear();
  pub.on_snapshot(s1);
  assert(pub.ok());
  assert(fake.ones.size() == 2);
  assert(fake.ones[0].serial == s1.sig_serial[0] && fake.ones[1].serial == s1.sig_serial[1]);
  assert(fake.ones[0].value == s1.sig_value[0] && fake.ones[1].value == s1.sig_value[1]);
  assert(wengine::ros2::serial_topic(cfg, 1) != wengine::ros2::serial_topic(cfg, 2));
}

void test_over_bound_signals_sticky() {
  wengine::world::State st;
  wengine::signal::SignalParams p;
  for (std::uint64_t k = 0; k < wengine::ros2::kMaxRosSignals + 1; ++k) {
    const wengine::world::Entity e = st.spawn_entity();
    assert(wengine::world::configure_source(e, st.signals, p, st.sim_time).ok());
  }
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  pub.on_snapshot(snap);
  assert(!pub.ok());
  assert(pub.status().code == wengine::core::StatusCode::kOutOfRange);
  assert(pub.stats().dropped_over_bound == 1);
  assert(fake.grouped.empty());
  // Sticky: later snapshots count disabled, never touch the transport.
  pub.on_snapshot(snap);
  assert(pub.stats().disabled_after_error == 1);
  assert(fake.grouped.empty());
  assert(wengine::link::output_accounting_ok(pub.stats(), 0));
}

void test_over_bound_particles() {
  wengine::world::State st;
  for (std::uint64_t k = 0; k < wengine::ros2::kMaxRosParticles + 1; ++k) {
    const wengine::world::Entity e = st.spawn_entity();
    st.particles.attach(e.index);
  }
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  assert(snap.particle_count() == wengine::ros2::kMaxRosParticles + 1);
  pub.on_snapshot(snap);
  assert(!pub.ok());
  assert(pub.status().code == wengine::core::StatusCode::kOutOfRange);
  assert(pub.stats().dropped_over_bound == 1);
}

void test_backpressure_transient_dead_sticky() {
  wengine::world::Entity e0{0, 0};
  wengine::world::Entity e1{0, 0};
  wengine::world::State st = make_state_two_sources(e0, e1);
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  fake.script.push_back(TransportOutcome::kBackpressure);
  pub.on_snapshot(snap);
  assert(pub.ok()); // transient: counted, never sticky
  assert(pub.stats().dropped_send == 1 && pub.stats().published == 0);
  pub.on_snapshot(snap);
  assert(pub.stats().published == 1);
  fake.script.push_back(TransportOutcome::kDead);
  pub.on_snapshot(snap);
  assert(!pub.ok());
  assert(pub.status().code == wengine::core::StatusCode::kInternal);
  const std::uint64_t published = pub.stats().published;
  pub.on_snapshot(snap);
  assert(pub.stats().published == published);
  assert(pub.stats().disabled_after_error == 2); // dead call + post-sticky call
}

void test_invalid_config_inert() {
  FakeTransport fake;
  BridgeConfig cfg;
  cfg.node_name = "";
  RosPublisher pub(cfg, fake);
  assert(!pub.ok());
  wengine::world::State st;
  const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
  pub.on_snapshot(snap);
  assert(fake.grouped.empty());
  assert(pub.stats().disabled_after_error == 1);
}

void test_stride_and_subset_wiring() {
  wengine::world::Entity e0{0, 0};
  wengine::world::Entity e1{0, 0};
  wengine::world::State st = make_state_two_sources(e0, e1);
  FakeTransport fake;
  BridgeConfig cfg;
  RosPublisher pub(cfg, fake);
  // Chain mirrors sig.cpp: publisher <- Subset <- Stride.
  wengine::emit::SignalSubsetSink subset(pub);
  subset.set_subscription({0});
  wengine::emit::StrideSink stride(subset, 2);
  for (std::uint64_t step = 0; step < 4; ++step) {
    st.sim_time = static_cast<double>(step) * 0.01;
    st.step_index = step;
    const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
    stride.on_snapshot(snap);
  }
  // Stride 2 keeps steps 0,2; subset keeps serial 0 only.
  assert(stride.matched() == 2 && stride.dropped() == 2);
  assert(subset.matched() == 2);
  assert(pub.stats().published == 2);
  assert(fake.grouped.size() == 2);
  for (const auto &call : fake.grouped) {
    assert(call.block.serial.size() == 1 && call.block.serial[0] == 0);
  }
}

void test_serial_high_water_backstop() {
  FakeTransport fake;
  BridgeConfig cfg;
  cfg.grouped_signals = false;
  RosPublisher pub(cfg, fake);
  wengine::world::State st;
  // Churn one live source: serials march 0..N while only one is live.
  wengine::signal::SignalParams p;
  wengine::world::Entity live{0, 0};
  bool have_live = false;
  for (std::uint64_t k = 0; k <= wengine::ros2::kMaxRosSerialTopics; ++k) {
    if (have_live) {
      assert(st.despawn_entity(live));
    }
    live = st.spawn_entity();
    assert(wengine::world::configure_source(live, st.signals, p, st.sim_time).ok());
    have_live = true;
    const wengine::world::Snapshot snap = wengine::world::take_snapshot(st);
    pub.on_snapshot(snap);
    if (!pub.ok()) {
      break;
    }
  }
  assert(!pub.ok());
  assert(pub.status().code == wengine::core::StatusCode::kOutOfRange);
  assert(pub.stats().dropped_over_bound == 1);
}

} // namespace

int main() {
  test_topics();
  test_qos_and_stamp();
  test_grouped_layout();
  test_owned_copy();
  test_per_serial_survives_respawn();
  test_over_bound_signals_sticky();
  test_over_bound_particles();
  test_backpressure_transient_dead_sticky();
  test_invalid_config_inert();
  test_stride_and_subset_wiring();
  test_serial_high_water_backstop();
  std::printf("test_ros2_publisher: OK\n");
  return 0;
}
