#pragma once

/**
 * @file
 * Shared `--emit-ros` / `--ros-*` option vocabulary for `sim run` and
 * `sig run` (ROS2-3): node/namespace/stem shapes, QoS text, and the
 * BridgeConfig builder. Parsed values and validation only - no rclcpp,
 * no I/O, no syscalls - so both commands validate identically (the CLI
 * owns taking/parsing via ArgCursor plus the emit_options.hpp
 * stride/select helpers). Sinks stay caller-owned at the app edge, never
 * in RunConfig, the same rule as every other emit transport: enabling
 * `--emit-ros` constructs a RosPublisher plus RclcppTransport in the
 * command and attaches it through the usual StrideSink/SignalSubsetSink
 * edge wrappers. Without BUILD_ROS2 the commands parse and validate
 * everything, then fail loudly (rebuild hint, exit 1) instead of
 * silently dropping the flag. Consumers: the `sim`/`sig` batch commands
 * and their completion entries. Compiles and unit-tests with no ROS
 * install; only ensure_ros_initialized needs WENGINE_HAS_RCLCPP.
 * Delivery classes follow docs/delivery.md and docs/ros2.md: complete
 * recording never rides a topic.
 */

// Shared `--emit-ros` / `--ros-*` option vocabulary for `sim run` and
// `sig run` (ROS2-3).
//
// ROS-free by construction (like every other ros2/ header): parsed values
// and validation only, no rclcpp, no I/O. The CLI owns taking/parsing
// (ArgCursor + cli/emit_options.hpp stride/select helpers); this header
// owns the ROS identity half — node/namespace/stem shapes, QoS text, and
// the BridgeConfig builder — so both commands validate identically.
// Sinks stay caller-owned at the edge (never in RunConfig, same rule as
// every other emit transport): enabling `--emit-ros` constructs a
// RosPublisher + RclcppTransport in the command and attaches it through
// the usual StrideSink/SignalSubsetSink edge wrappers. Without
// BUILD_ROS2 the commands parse/validate everything and then fail loudly
// (rebuild hint, exit 1) instead of silently dropping the flag.
//
// Options (see SimSpec/SigSpec kUsage):
//   --emit-ros                enable ROS publishing (flag)
//   --emit-ros-stride N       per-output stride (cli::parse_emit_stride)
//   --emit-ros-select L       signal serial allowlist (cli::parse_emit_select)
//   --ros-node NAME           node name (default "wengine")
//   --ros-ns NS               topic namespace, relative (default "wengine")
//   --ros-signal-stem STEM    grouped/per-serial topic stem (default "signals")
//   --ros-particle-stem STEM  particle topic stem (default "particles")
//   --ros-qos latest|reliable delivery class (default latest)
//   --ros-grouped / --ros-per-serial  topic layout (default grouped,
//                             mutually exclusive)

#include "wengine/core/status.hpp"
#include "wengine/ros2/config.hpp"

#include <string>
#include <string_view>

#ifdef WENGINE_HAS_RCLCPP
#include "rclcpp/rclcpp.hpp"
#endif

namespace wengine::ros2::cli_options {

// Canonical option spellings (single source so sim/sig/completion agree).
/**
 * @brief Flag spelling enabling ROS publishing.
 */
inline constexpr std::string_view kEmitRos = "--emit-ros";
/**
 * @brief Option spelling for the per-output ROS stride.
 */
inline constexpr std::string_view kEmitRosStride = "--emit-ros-stride";
/**
 * @brief Option spelling for the ROS serial allowlist.
 */
inline constexpr std::string_view kEmitRosSelect = "--emit-ros-select";
/**
 * @brief Option spelling for the ROS node name.
 */
inline constexpr std::string_view kRosNode = "--ros-node";
/**
 * @brief Option spelling for the ROS topic namespace.
 */
inline constexpr std::string_view kRosNs = "--ros-ns";
/**
 * @brief Option spelling for the signal topic stem.
 */
inline constexpr std::string_view kRosSignalStem = "--ros-signal-stem";
/**
 * @brief Option spelling for the particle topic stem.
 */
inline constexpr std::string_view kRosParticleStem = "--ros-particle-stem";
/**
 * @brief Option spelling for the QoS delivery class.
 */
inline constexpr std::string_view kRosQos = "--ros-qos";
/**
 * @brief Flag spelling for the grouped topic layout.
 */
inline constexpr std::string_view kRosGrouped = "--ros-grouped";
/**
 * @brief Flag spelling for the per-serial topic layout.
 */
inline constexpr std::string_view kRosPerSerial = "--ros-per-serial";

// QoS text (D-ROS-5): latest-state viewing vs ordered sample streaming.
/**
 * @brief QoS word for latest-state viewing (delivery class A).
 */
inline constexpr std::string_view kQosLatest = "latest";
/**
 * @brief QoS word for ordered bounded sample streaming (delivery class B).
 */
inline constexpr std::string_view kQosReliable = "reliable";

/**
 * @brief Parse `--ros-qos` text into a QoS delivery class.
 * @param[in] s QoS word (`latest` or `reliable`).
 * @return Success with the class on a known word; failure with
 *         kInvalidArgument on anything else (usage error, exit 2).
 * @note ROS-free intent only; the rmw profile is built by ros_qos_for
 *       in the rclcpp TU, never here.
 */
[[nodiscard]] inline core::Result<QosClass> parse_ros_qos(std::string_view s) noexcept {
  if (s == kQosLatest) {
    return core::Result<QosClass>::success(QosClass::kLatestState);
  }
  if (s == kQosReliable) {
    return core::Result<QosClass>::success(QosClass::kSampleStream);
  }
  return core::Result<QosClass>::failure("unknown ROS QoS (want latest|reliable)");
}

/**
 * @brief Canonical QoS word for a delivery class (round-trips parse_ros_qos).
 * @param[in] q Delivery class to name.
 * @return `latest` for kLatestState, `reliable` for kSampleStream.
 */
[[nodiscard]] inline constexpr std::string_view qos_name(QosClass q) noexcept {
  return q == QosClass::kSampleStream ? kQosReliable : kQosLatest;
}

// One ROS path segment: [A-Za-z_][A-Za-z0-9_]* (rmw refuses digit-leading
// segments, so serials take the `sig_<serial>` flat form — see mapping.hpp).
/**
 * @brief Check one ROS path segment shape (the rmw segment rule).
 * @param[in] s Segment text, without slashes.
 * @return True for [A-Za-z_][A-Za-z0-9_]*; digit-leading segments fail
 *         because rmw refuses them (serials use the flat `sig_` prefixed
 *         form instead, see mapping.hpp).
 */
[[nodiscard]] inline constexpr bool is_valid_ros_segment(std::string_view s) noexcept {
  if (s.empty()) {
    return false;
  }
  const char first = s.front();
  const bool first_ok = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_';
  if (!first_ok) {
    return false;
  }
  for (char c : s) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Relative ROS path (namespace or topic stem): one or more valid segments
// joined by single '/' (no leading/trailing/double slashes — the bridge
// prepends exactly one '/' when forming absolute topics).
/**
 * @brief Check a relative ROS path (namespace or topic stem).
 * @param[in] s Path text: valid segments joined by single `/`, with no
 *            leading, trailing, or doubled slashes.
 * @return True when every segment passes is_valid_ros_segment.
 * @note The bridge prepends exactly one `/` when forming absolute
 *       topics; callers pass the relative form only.
 */
[[nodiscard]] inline constexpr bool is_valid_ros_path(std::string_view s) noexcept {
  if (s.empty() || s.front() == '/' || s.back() == '/') {
    return false;
  }
  std::size_t begin = 0;
  while (true) {
    const std::size_t slash = s.find('/', begin);
    const std::string_view seg = (slash == std::string_view::npos) ? s.substr(begin) : s.substr(begin, slash - begin);
    if (!is_valid_ros_segment(seg)) {
      return false;
    }
    if (slash == std::string_view::npos) {
      return true;
    }
    begin = slash + 1;
  }
}

/**
 * @brief Check a ROS node name shape (exactly one valid segment).
 * @param[in] s Node name text.
 * @return True when s is one valid ROS segment.
 */
[[nodiscard]] inline constexpr bool is_valid_ros_node(std::string_view s) noexcept {
  return is_valid_ros_segment(s);
}

// Fill a BridgeConfig from validated CLI text (defaults for empty views).
// Callers validate shapes first (is_valid_ros_*) so this never fails.
/**
 * @brief Fill a BridgeConfig from validated CLI text.
 * @param[out] cfg Config overwritten field by field.
 * @param[in] node Node name, empty for the `wengine` default.
 * @param[in] ns Namespace, empty for the `wengine` default.
 * @param[in] signal_stem Signal topic stem, empty for `signals`.
 * @param[in] particle_stem Particle topic stem, empty for `particles`.
 * @param[in] qos Delivery class from parse_ros_qos.
 * @param[in] grouped True for the grouped layout, false for per-serial.
 * @note Callers validate shapes first (is_valid_ros_*), so this never
 *       fails and returns nothing. Views alias caller-owned storage.
 */
inline void make_bridge_config(BridgeConfig &cfg, std::string_view node, std::string_view ns, std::string_view signal_stem,
                               std::string_view particle_stem, QosClass qos, bool grouped) noexcept {
  cfg.node_name = node.empty() ? "wengine" : node;
  cfg.ns = ns.empty() ? "wengine" : ns;
  cfg.signal_topic_stem = signal_stem.empty() ? "signals" : signal_stem;
  cfg.particle_topic_stem = particle_stem.empty() ? "particles" : particle_stem;
  cfg.qos = qos;
  cfg.grouped_signals = grouped;
}

#ifdef WENGINE_HAS_RCLCPP
// Initialize rclcpp once per process for the CLI batch path (the commands
// are a library: tests and the TUI call run() repeatedly in one process,
// so shutdown is never called here — the context lives until process
// exit). Signal handling stays off (shutdown_on_signal = false): paced and
// --forever runs own SIGINT via their step-boundary handlers, and a batch
// publish path must never reprogram them. Re-entry (already initialized
// by this or another component) falls back to ok(): proceed when the
// context is usable, fail loudly only when it is not.
/**
 * @brief Initialize rclcpp once per process for the CLI batch path.
 * @return True when the rmw context is usable (freshly initialized or
 *         already up); false only when initialization truly failed.
 * @note Needs WENGINE_HAS_RCLCPP (BUILD_ROS2=ON). Signal handling stays
 *       off: paced and `--forever` runs own SIGINT via their
 *       step-boundary handlers, and a batch publish path must never
 *       reprogram them. Shutdown is never called here - the context lives
 *       until process exit, since commands are a library whose run() may
 *       execute repeatedly in one process. Never throws.
 */
[[nodiscard]] inline bool ensure_ros_initialized() noexcept {
  try {
    rclcpp::InitOptions opts;
    opts.shutdown_on_signal = false;
    rclcpp::init(0, nullptr, opts);
    return true;
  } catch (...) {
  }
  try {
    return rclcpp::ok();
  } catch (...) {
    return false;
  }
}
#endif

}  // namespace wengine::ros2::cli_options
