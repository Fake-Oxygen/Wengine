// ROS2 mapping tests: pure vocabulary, no rclcpp (links wengine_core alone).
#include "wengine/link/frame.hpp"
#include "wengine/ros2/cli_options.hpp"
#include "wengine/ros2/config.hpp"
#include "wengine/ros2/control.hpp"
#include "wengine/ros2/mapping.hpp"
#include "wengine/signal/control.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <cassert>
#include <cstdio>

namespace {
using wengine::link::Frame;
using wengine::ros2::block_from_snapshot;
using wengine::ros2::is_valid_config;
using wengine::ros2::publisher_count;
using wengine::ros2::sanitize_channel;
using wengine::ros2::topic_for_channel;
using wengine::ros2::topic_for_serial;

void test_topics() {
  assert(topic_for_serial("wengine/signals", 3) == "wengine/signals/sig_3");
  assert(topic_for_channel("wengine", "sig.3.U") == "wengine/sig/s3/U");
  assert(sanitize_channel("sig.3.U") == "sig/s3/U");
  assert(sanitize_channel("p0.x") == "p0/x");
}

void test_block_topic() {
  wengine::ros2::BridgeConfig c;
  assert(wengine::ros2::signals_block_topic(c) == "/wengine/signals/block");
  c.ns = "bench";
  c.signal_topic_stem = "sig";
  assert(wengine::ros2::signals_block_topic(c) == "/bench/sig/block");
}

void test_config() {
  wengine::ros2::BridgeConfig c;
  assert(is_valid_config(c));
  assert(publisher_count(0, true) == 2);
  assert(publisher_count(4, true) == 2);
  assert(publisher_count(4, false) == 5);
  wengine::ros2::BridgeConfig bad;
  bad.stride = 0;
  assert(!is_valid_config(bad));
  bad = wengine::ros2::BridgeConfig{};
  bad.node_name = "";
  assert(!is_valid_config(bad));
}

void test_block_layout() {
  wengine::world::State st;
  wengine::signal::SignalParams p0;
  p0.quantity = wengine::signal::QuantityId::kVoltage;
  p0.freq_hz = 1.0;
  wengine::signal::SignalParams p1 = p0;
  p1.freq_hz = 2.0;
  const wengine::world::Entity e0 = st.spawn_entity();
  assert(wengine::world::configure_source(e0, st.signals, p0, st.sim_time).ok());
  const wengine::world::Entity e1 = st.spawn_entity();
  assert(wengine::world::configure_source(e1, st.signals, p1, st.sim_time).ok());
  const wengine::world::Snapshot s = wengine::world::take_snapshot(st);
  assert(s.signal_count() == 2);
  const auto b = block_from_snapshot(s);
  assert(b.serial.size() == 2 && b.value.size() == 2 && b.quantity.size() == 2);
  // Slot order in, slot order out; serials stable (0,1 on fresh State).
  assert(b.serial[0] == s.sig_serial[0] && b.serial[1] == s.sig_serial[1]);
  assert(b.value[0] == s.sig_value[0] && b.value[1] == s.sig_value[1]);
  assert(b.step == s.step_index && b.sim_time == s.sim_time);
}

void test_service_verbs() {
  using V = wengine::signal::control::Verb;
  assert(wengine::ros2::control::verb_for("sig_add") == V::kAdd);
  assert(wengine::ros2::control::verb_for("sig_set") == V::kSet);
  assert(wengine::ros2::control::verb_for("sig_start") == V::kStart);
  assert(wengine::ros2::control::verb_for("sig_stop") == V::kStop);
  assert(wengine::ros2::control::verb_for("sig_drop") == V::kDrop);
  assert(wengine::ros2::control::verb_for("sig_list") == V::kList);
  assert(wengine::ros2::control::verb_for("nope") == V::kList);
}

void test_quantity_suffix_single_table() {
  // Display suffix comes from the one quantity table, no local switch.
  assert(wengine::ros2::unit_suffix(wengine::signal::QuantityId::kVoltage) == "U");
}

void test_ros_cli_options() {
  namespace cli = wengine::ros2::cli_options;
  using wengine::ros2::BridgeConfig;
  using wengine::ros2::QosClass;
  // QoS text (D-ROS-5): latest vs reliable, nothing else.
  assert(cli::parse_ros_qos("latest").value == QosClass::kLatestState);
  assert(cli::parse_ros_qos("reliable").value == QosClass::kSampleStream);
  assert(!cli::parse_ros_qos("best-effort").ok());
  assert(!cli::parse_ros_qos("").ok());
  assert(cli::qos_name(QosClass::kLatestState) == "latest");
  assert(cli::qos_name(QosClass::kSampleStream) == "reliable");
  // Node names are single rmw segments (no digits first, no slashes).
  assert(cli::is_valid_ros_node("wengine"));
  assert(cli::is_valid_ros_node("_node1"));
  assert(!cli::is_valid_ros_node(""));
  assert(!cli::is_valid_ros_node("3node"));
  assert(!cli::is_valid_ros_node("my-node"));
  assert(!cli::is_valid_ros_node("a/b"));
  // Namespaces/stems are relative segment paths (no leading slash).
  assert(cli::is_valid_ros_path("wengine"));
  assert(cli::is_valid_ros_path("a/b"));
  assert(!cli::is_valid_ros_path(""));
  assert(!cli::is_valid_ros_path("/wengine"));
  assert(!cli::is_valid_ros_path("wengine/"));
  assert(!cli::is_valid_ros_path("a//b"));
  assert(!cli::is_valid_ros_path("3sig"));
  // Builder fills defaults for empty views.
  BridgeConfig cfg;
  cli::make_bridge_config(cfg, "", "", "", "", QosClass::kLatestState, true);
  assert(cfg.node_name == "wengine" && cfg.ns == "wengine");
  assert(cfg.signal_topic_stem == "signals" && cfg.particle_topic_stem == "particles");
  assert(cfg.qos == QosClass::kLatestState && cfg.grouped_signals);
  assert(wengine::ros2::is_valid_config(cfg));
  cli::make_bridge_config(cfg, "talker", "bench", "sig", "pts",
                           QosClass::kSampleStream, false);
  assert(cfg.node_name == "talker" && cfg.ns == "bench");
  assert(cfg.signal_topic_stem == "sig" && cfg.particle_topic_stem == "pts");
  assert(cfg.qos == QosClass::kSampleStream && !cfg.grouped_signals);
  assert(wengine::ros2::is_valid_config(cfg));
}
} // namespace

int main() {
  test_topics();
  test_block_topic();
  test_config();
  test_block_layout();
  test_service_verbs();
  test_quantity_suffix_single_table();
  test_ros_cli_options();
  std::printf("test_ros2_mapping: OK\n");
  return 0;
}
