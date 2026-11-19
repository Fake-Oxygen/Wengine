#pragma once

/**
 * @file
 * ROS2 state-out publisher: the emit::ISink that adapts Runner snapshots
 * onto ROS topics. Edge-only bridge (D-ROS-1): built and owned by the
 * caller (bridge process, CLI/GUI edge), attached like any sink via
 * Runner::add_sink, never part of RunConfig/Runner/Snapshot - mirrors
 * ShmPublisher/TcpPublisher at the app edge. ROS-free like the link
 * headers: this header owns the transport-neutral half (serial-addressed
 * naming, QoS intent, sim_time stamps, the over-bound gate, ISink
 * fan-out); rmw calls live in the rclcpp TU behind the RosTransport
 * seam, so unit tests link wengine_core alone with a fake transport.
 * Consumers: ros2/rclcpp_transport.hpp, the CLI `sim run`/`sig run`
 * batch paths, and test_ros2_publisher. Publish path performs no
 * syscalls or blocking I/O, never reenters the driving Runner, and
 * never throws out of on_snapshot; failures are sticky core::Status and
 * over-bound frames drop loudly, never truncate (see docs/delivery.md).
 */

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
/**
 * @brief Maximum signal rows per snapshot (LinkFrame parity: 64).
 */
inline constexpr std::size_t kMaxRosSignals = link::kMaxChannels;
/**
 * @brief Maximum particle items per snapshot (LinkFrame parity: 1024).
 */
inline constexpr std::size_t kMaxRosParticles = link::kMaxItems;
// Backstop on lazily created per-serial publishers (per-serial mode only;
// grouped mode advertises a fixed 2 topics). Serials are dense from 0 per
// State, so the high-water mark bounds the retained publisher set.
/**
 * @brief Backstop on lazily created per-serial publishers (per-serial
 *        mode only; grouped mode advertises a fixed 2 topics).
 * @note Serials are dense from 0 per State, so the high-water mark
 *       bounds the retained publisher set.
 */
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
/**
 * @brief Whether a QoS class maps to reliable (vs best-effort) rmw
 *        delivery.
 * @param[in] q QoS class from BridgeConfig.
 * @return True for kSampleStream (reliable), false for kLatestState
 *         (best-effort).
 */
[[nodiscard]] inline constexpr bool qos_is_reliable(QosClass q) noexcept {
  return q == QosClass::kSampleStream;
}

/**
 * @brief Keep-last history depth for a QoS class.
 * @param[in] q QoS class from BridgeConfig.
 * @return 10 for kSampleStream, 1 for kLatestState.
 */
[[nodiscard]] inline constexpr std::size_t qos_depth(QosClass q) noexcept {
  return q == QosClass::kSampleStream ? 10 : 1;
}

/**
 * @brief rmw reliability name for a QoS class.
 * @param[in] q QoS class from BridgeConfig.
 * @return `reliable` or `best_effort` (for health/summary lines).
 */
[[nodiscard]] inline constexpr std::string_view qos_reliability_name(QosClass q) noexcept {
  return qos_is_reliable(q) ? "reliable" : "best_effort";
}

/**
 * @brief rmw durability name for a QoS class.
 * @param[in] q QoS class (ignored by the pinned policy).
 * @return `volatile` for both classes - late joiners miss earlier ticks
 *         by pin (no transient-local replay).
 */
[[nodiscard]] inline constexpr std::string_view qos_durability_name(QosClass q) noexcept {
  (void)q;
  return "volatile";
}

/**
 * @brief rmw history kind name for a QoS class.
 * @param[in] q QoS class (ignored by the pinned policy).
 * @return `keep_last` for both classes.
 */
[[nodiscard]] inline constexpr std::string_view qos_history_name(QosClass q) noexcept {
  (void)q;
  return "keep_last";
}

// Absolute topic names (leading '/'): the bridge node spins in the root
// namespace so `ros2 topic list` shows these verbatim.
/**
 * @brief Absolute grouped-signals topic (`/<ns>/<signal_stem>`).
 * @param[in] c Bridge config (namespace + signal stem).
 * @return Topic name with a leading `/`.
 * @note The bridge node spins in the root namespace so
 *       `ros2 topic list` shows these names verbatim.
 */
[[nodiscard]] inline std::string grouped_signals_topic(const BridgeConfig &c) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += std::string(c.signal_topic_stem);
  return out;
}

/**
 * @brief Absolute per-serial topic (`/<ns>/<signal_stem>/sig_<serial>`).
 * @param[in] c Bridge config (namespace + signal stem).
 * @param[in] serial Stable source serial (never reused per State).
 * @return Topic name with a leading `/`.
 */
[[nodiscard]] inline std::string serial_topic(const BridgeConfig &c, std::uint64_t serial) {
  std::string out("/");
  out += std::string(c.ns);
  out += '/';
  out += topic_for_serial(c.signal_topic_stem, serial);
  return out;
}

/**
 * @brief Absolute particle topic (`/<ns>/<particle_stem>`).
 * @param[in] c Bridge config (namespace + particle stem).
 * @return Topic name with a leading `/`.
 */
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
/**
 * @brief Message header stamp split into seconds + nanoseconds.
 * @note Mirrors the rmw builtin_interfaces time fields; the engine
 *       clock (seconds) is the only source (D-ROS-7), never wall time.
 */
struct RosStamp {
  /**
   * @brief Whole seconds of the engine sim_time.
   */
  std::int32_t sec = 0;
  /**
   * @brief Sub-second nanoseconds [0, 999999999].
   */
  std::uint32_t nanosec = 0;
};

/**
 * @brief Convert engine sim_time to a message header stamp.
 * @param[in] sim_time Engine simulation time in seconds (SI).
 * @return sec/nanosec split; non-finite or negative inputs (never
 *         produced by the Runner) stamp zero; seconds clamp to int32
 *         range, nanoseconds clamp below one second.
 */
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
/**
 * @brief Result of one rmw publish call, mirroring the socket
 *        vocabulary: kAccepted (local DDS acceptance only - remote
 *        receipt is unknown without a receiver report), kBackpressure
 *        (transient queue-full: counted, never sticky - a slow reader
 *        loses beyond its history bound by design), kDead (transport
 *        unusable, e.g. a throwing impl: treated as a dead peer -
 *        sticky kInternal at the publisher).
 */
enum class TransportOutcome : std::uint8_t { kAccepted, kBackpressure, kDead };

// rmw seam. Implementations (rclcpp TU, test fakes) copy what they need
// out of the referenced payloads before returning: the publisher reuses
// its staging buffers across emissions, so no reference handed here is
// valid after the call. Never throws; a throwing transport is treated as
// a dead peer (sticky kInternal at the publisher).
/**
 * @brief rmw seam: the transport-neutral publish interface the ROS-free
 *        publisher drives.
 * @note Implementations (rclcpp TU, test fakes) copy what they need out
 *       of the referenced payloads before returning: the publisher
 *       reuses its staging buffers across emissions, so no reference
 *       handed here is valid after the call. Never throws; a throwing
 *       transport is treated as a dead peer (sticky kInternal at the
 *       publisher). The implementation is owned by the caller and must
 *       outlive the RosPublisher attached to it.
 */
class RosTransport {
public:
  /**
   * @brief Destructor (default; the concrete TU tears down its rmw
   *        resources).
   */
  virtual ~RosTransport() = default;
  /**
   * @brief Publish the grouped pair (exact SignalBlock + particle frame).
   * @param[in] block Owned copy of the grouped signal record (dense
   *                  slot order, serial/value/quantity arrays).
   * @param[in] particles Owned Frame copy for the particle topic.
   * @return TransportOutcome for this call.
   */
  virtual TransportOutcome publish_grouped(const SignalBlock &block, const link::Frame &particles) noexcept = 0;
  /**
   * @brief Publish one single-entry per-serial message.
   * @param[in] serial Stable source serial (topic key, never a slot).
   * @param[in] value SI value of the source this tick.
   * @param[in] quantity Quantity code mirroring signal::QuantityId.
   * @param[in] step Engine step index of the emitted tick.
   * @param[in] sim_time Engine simulation time in seconds (SI) - the
   *                     message stamp, never wall time.
   * @return TransportOutcome for this call.
   */
  virtual TransportOutcome publish_one(std::uint64_t serial, double value, std::uint8_t quantity, std::uint64_t step, double sim_time) noexcept = 0;
  /**
   * @brief Publish the particle cloud frame.
   * @param[in] particles Owned Frame copy (x/y/vx/vy items).
   * @return TransportOutcome for this call.
   */
  virtual TransportOutcome publish_particles(const link::Frame &particles) noexcept = 0;
};

// State-out ISink for ROS topics (caller-owned, like every sink).
// Single-writer model: call on_snapshot from one runner thread only.
// Not copyable (owns staging); movable (transport reference rebinds by
// copy, like a non-owning observer).
/**
 * @brief State-out ISink for ROS topics: adapts each Snapshot to
 *        grouped/per-serial messages through a RosTransport.
 * @note Caller-owned like every sink: attach via Runner::add_sink at
 *       the app edge, never store it in RunConfig. Single-writer
 *       model - call on_snapshot from one runner thread only. Takes an
 *       owned copy of the snapshot before returning (the Runner reuses
 *       one Snapshot buffer across emissions). Failures are sticky
 *       core::Status; over-bound frames drop loudly and are counted,
 *       never truncated.
 */
class RosPublisher : public emit::ISink {
public:
  /**
   * @brief Construct with a validated bridge config and a transport.
   * @param[in] cfg Validated identity/QoS/layout (is_valid_config).
   * @param[in,out] transport rmw seam (rclcpp TU or test fake);
   *                caller-owned and must outlive this publisher.
   */
  explicit RosPublisher(const BridgeConfig &cfg, RosTransport &transport);
  /**
   * @brief Destructor (default; staging buffers free themselves).
   */
  ~RosPublisher() override = default;

  /**
   * @brief Copy construction is deleted: the publisher owns staging
   *        buffers and a non-owning transport reference.
   */
  RosPublisher(const RosPublisher &) = delete;
  /**
   * @brief Copy assignment is deleted (same reason as the deleted copy).
   */
  RosPublisher &operator=(const RosPublisher &) = delete;
  /**
   * @brief Move construction is allowed: the transport reference rebinds
   *        by copy, like a non-owning observer.
   */
  RosPublisher(RosPublisher &&) noexcept = default;
  /**
   * @brief Move assignment is deleted (single owner, no rebinding in
   *        place).
   */
  RosPublisher &operator=(RosPublisher &&) = delete;

  /**
   * @brief Adapt and publish one Snapshot (the emit::ISink contract).
   * @param[in] snapshot Independent copy taken by the Runner this tick;
   *                     never the mutable live state.
   * @note Publish path performs no syscalls or blocking I/O and never
   *       reenters the driving Runner; over-bound frames (rows/items/
   *       serial high-water beyond the kMaxRos* bounds) refuse the whole
   *       snapshot loudly (sticky kOutOfRange, frame skipped, counted).
   */
  void on_snapshot(const world::Snapshot &snapshot) override;

  /**
   * @brief Whether publishing is still healthy.
   * @return False once a failure turned sticky (never resets).
   */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  /**
   * @brief Sticky failure status (kOk until something fails).
   * @return kOk while healthy; the first failure code otherwise
   *         (kOutOfRange over-bound, kInternal dead transport).
   */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  /**
   * @brief Publish statistics in the shared link counter vocabulary.
   * @return Attempted/published/over-bound/backpressure/bytes counters;
   *         output_accounting_ok closes the books in tests.
   */
  [[nodiscard]] const link::PublisherStats &stats() const noexcept { return stats_; }
  // Highest signal serial observed so far (per-serial backstop cursor).
  /**
   * @brief Highest signal serial observed so far (per-serial backstop
   *        cursor for the lazily created publisher set).
   * @return Largest serial seen across emissions; 0 before the first
   *         signal snapshot.
   */
  [[nodiscard]] std::uint64_t max_serial_seen() const noexcept { return max_serial_seen_; }
  /**
   * @brief The config this publisher was constructed with.
   * @return Const reference to the owned copy of the BridgeConfig.
   */
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
