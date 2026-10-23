#pragma once

// ROS2 state-out publisher: emit::ISink adapting Snapshots for ROS topics.
//
// An edge-only bridge (D-ROS-1, see docs/ros2.md): built and owned by the
// caller (bridge process, CLI/GUI edge), attached like any sink via
// Runner::add_sink, and never part of RunConfig/Runner/Snapshot. Mirrors
// ShmPublisher/TcpPublisher at the app edge: the publish path performs no
// syscalls or blocking I/O, takes an owned copy before returning (the
// Runner reuses one Snapshot buffer across emissions), never reenters the
// driving Runner, and never throws out of on_snapshot. Failures are sticky
// core::Status; over-bound frames are dropped loudly and counted, never
// truncated.
//
// This header is ROS-free (same layering as link/* headers staying
// platform-free while src/link/*.cpp own the syscalls): it owns the
// transport-neutral half — topic naming (serial-addressed, never slot
// indices), QoS intent per D-ROS-5, sim_time stamp conversion, the shared
// over-bound gate, and the ISink fan-out. The rmw calls live in the rclcpp
// TU (src/ros2/rclcpp_transport.cpp, built only with BUILD_ROS2=ON) behind
// the RosTransport seam below, so unit tests link wengine_core alone with
// a fake transport.
//
// Delivery mapping (see docs/delivery.md + D-ROS-4/5):
//   * latest-state viewing (QosClass::kLatestState, default): best-effort,
//     keep-last depth 1. Writer drops oldest; readers take latest.
//   * ordered sample streaming (QosClass::kSampleStream): reliable bounded
//     (keep-last depth 10). Backpressure is counted, never a zero-loss
//     promise to arbitrarily slow receivers.
//   * complete recording stays in the CSV/block recorder (driver-paced),
//     never a topic promise.
// Four clocks stay separate: sim dt, emit stride (StrideSink/BridgeConfig
// stride), ROS wall packet rate, UI refresh — waveform frequency is none
// of those. Message stamps carry Snapshot sim_time; ROS wall time never
// drives physics.
//
// Topics (see ros2/mapping.hpp for the naming rules):
//   * grouped signals (BridgeConfig::grouped_signals): one JointState on
//     `<ns>/<signal_topic_stem>` carrying every live source in slot order
//     (name `sig.<serial>.U`, position SI value; quantity rides in the
//     suffix from the single quantity table).
//   * per-signal (grouped_signals == false): one single-entry JointState
//     per live source on `<ns>/<signal_topic_stem>/sig_<serial>`.
//     Publishers are created lazily and retained: slots recycle, serials
//     don't, so a despawn/respawn never reuses a live subscription.
//   * particles: one PointCloud2 (x/y/vx/vy FLOAT64) on
//     `<ns>/<particle_topic_stem>` via frame_from_snapshot.
// Every message header stamp is sim_time (see sim_time_to_stamp).
//
// Over-bound gate (shared with every other transport, see
// link/protocol.hpp S7): signal rows beyond kMaxRosSignals, particle
// items beyond kMaxRosParticles, or a channel name reaching
// kChannelNameSize refuse the whole snapshot loudly (sticky kOutOfRange,
// dropped_over_bound, frame skipped). The per-serial backstop
// kMaxRosSerialTopics caps lazily created per-signal publishers
// (serials are dense from 0 per State, so the high-water mark bounds
// the live publisher set); beyond it the snapshot drops the same loud
// way. Counts follow link::PublisherStats so output_accounting_ok holds.

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/ros2/config.hpp"
#include "wengine/ros2/mapping.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::ros2 {

// Payload bounds shared with every transport (LinkFrame parity: 64
// channels, 1024 items). Loud refusal, never silent truncation.
inline constexpr std::size_t kMaxRosSignals = link::kMaxChannels;
inline constexpr std::size_t kMaxRosParticles = link::kMaxItems;
// Backstop on lazily created per-serial publishers (per-serial mode only;
// grouped mode advertises a fixed 2 topics). Serials are dense from 0 per
// State, so the high-water mark bounds the retained publisher set.
inline constexpr std::size_t kMaxRosSerialTopics = 1024;

// QoS intent for the edge TU (resolved to rmw profiles in the rclcpp TU
// only, never here): latest-state viewing is best-effort, sample
// streaming is reliable. Depths are the keep-last history sizes.
//
// Pinned QoS matrix (ROS2-4, see docs/ros2.md + docs/delivery.md):
//   * latest (kLatestState):  reliability best_effort, history keep_last,
//     depth 1, durability volatile. Writer drops oldest; readers take
//     latest (class A: live viewing).
//   * reliable (kSampleStream): reliability reliable, history keep_last,
//     depth 10, durability volatile, bounded. Backpressure is counted,
//     never a zero-loss promise to arbitrarily slow readers (class B:
//     ordered bounded streaming).
// Durability is volatile on both classes by pin: late joiners miss earlier
// ticks (no transient-local replay); complete capture stays in the
// CSV/block recorder, never a topic promise (class C).
[[nodiscard]] inline constexpr bool qos_is_reliable(QosClass q) noexcept {
  return q == QosClass::kSampleStream;
}

[[nodiscard]] inline constexpr std::size_t qos_depth(QosClass q) noexcept {
  return q == QosClass::kSampleStream ? 10 : 1;
}

[[nodiscard]] inline constexpr std::string_view qos_reliability_name(QosClass q) noexcept {
  return qos_is_reliable(q) ? "reliable" : "best_effort";
}

[[nodiscard]] inline constexpr std::string_view qos_durability_name(QosClass q) noexcept {
  (void)q;
  return "volatile";
}

[[nodiscard]] inline constexpr std::string_view qos_history_name(QosClass q) noexcept {
  (void)q;
  return "keep_last";
}

// Absolute topic names (leading '/'): the bridge node spins in the root
// namespace so `ros2 topic list` shows these verbatim.
[[nodiscard]] inline std::string grouped_signals_topic(const BridgeConfig &c) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += std::string(c.signal_topic_stem);
  return out;
}

[[nodiscard]] inline std::string serial_topic(const BridgeConfig &c, std::uint64_t serial) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += topic_for_serial(c.signal_topic_stem, serial);
  return out;
}

[[nodiscard]] inline std::string particles_topic(const BridgeConfig &c) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += std::string(c.particle_topic_stem);
  return out;
}

// sim_time (seconds, engine clock) -> header stamp. Non-finite or
// negative inputs (never produced by the Runner) stamp zero; seconds
// clamp to int32 range, nanos clamp below one second.
struct RosStamp {
  std::int32_t sec = 0;
  std::uint32_t nanosec = 0;
};

[[nodiscard]] inline RosStamp sim_time_to_stamp(double sim_time) noexcept {
  if (!std::isfinite(sim_time) || sim_time < 0.0) {
    return {};
  }
  const double whole = std::floor(sim_time);
  double frac = sim_time - whole;
  if (frac < 0.0) {
    frac = 0.0;
  }
  if (frac >= 1.0) {
    frac = 0.0;
  }
  constexpr double kMaxSec = 2147483647.0;
  const std::int32_t sec = whole >= kMaxSec ? std::numeric_limits<std::int32_t>::max() : static_cast<std::int32_t>(whole);
  std::uint32_t nanosec = static_cast<std::uint32_t>(frac * 1000000000.0);
  if (nanosec >= 1000000000u) {
    nanosec = 999999999u;
  }
  return {sec, nanosec};
}

// Transport outcome for one publish call (mirrors the socket vocabulary:
// transient backpressure is counted, a dead peer is sticky).
enum class TransportOutcome : std::uint8_t { kAccepted, kBackpressure, kDead };

// rmw seam. Implementations (rclcpp TU, test fakes) copy what they need
// out of the referenced payloads before returning: the publisher reuses
// its staging buffers across emissions, so no reference handed here is
// valid after the call. Never throws; a throwing transport is treated as
// a dead peer (sticky kInternal at the publisher).
class RosTransport {
public:
  virtual ~RosTransport() = default;
  virtual TransportOutcome publish_grouped(const SignalBlock &block, const link::Frame &particles) noexcept = 0;
  virtual TransportOutcome publish_one(std::uint64_t serial, double value, std::uint8_t quantity, std::uint64_t step, double sim_time) noexcept = 0;
  virtual TransportOutcome publish_particles(const link::Frame &particles) noexcept = 0;
};

// State-out ISink for ROS topics (caller-owned, like every sink).
// Single-writer model: call on_snapshot from one runner thread only.
// Not copyable (owns staging); movable (transport reference rebinds by
// copy, like a non-owning observer).
class RosPublisher : public emit::ISink {
public:
  explicit RosPublisher(const BridgeConfig &cfg, RosTransport &transport);
  ~RosPublisher() override = default;

  RosPublisher(const RosPublisher &) = delete;
  RosPublisher &operator=(const RosPublisher &) = delete;
  RosPublisher(RosPublisher &&) noexcept = default;
  RosPublisher &operator=(RosPublisher &&) = delete;

  void on_snapshot(const world::Snapshot &snapshot) override;

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  [[nodiscard]] const link::PublisherStats &stats() const noexcept { return stats_; }
  // Highest signal serial observed so far (per-serial backstop cursor).
  [[nodiscard]] std::uint64_t max_serial_seen() const noexcept { return max_serial_seen_; }
  [[nodiscard]] const BridgeConfig &config() const noexcept { return config_; }

private:
  BridgeConfig config_;
  RosTransport *transport_; // caller-owned (outlives this publisher)
  link::PublisherStats stats_{};
  core::Status status_{};
  SignalBlock staging_block_; // owned copies: valid only during the call
  link::Frame staging_signals_; // frame_from_signal_snapshot staging
  link::Frame staging_particles_; // frame_from_snapshot staging
  std::uint64_t max_serial_seen_ = 0;
  bool have_serial_ = false;
};

} // namespace wengine::ros2
