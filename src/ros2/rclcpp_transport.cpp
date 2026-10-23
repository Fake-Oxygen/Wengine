// rclcpp transport for the ROS2 state-out publisher (rmw TU).
//
// Built only with BUILD_ROS2=ON (see CMakeLists.txt). Owns the rmw calls
// the core TU deliberately avoids: JointState grouped/per-serial topics,
// the exact wengine_msgs/SignalBlock record (grouped mode), plus
// PointCloud2 particles, QoS per D-ROS-5, sim_time header stamps.
// Publish calls are synchronous copies into DDS (non-blocking on
// readers); reuse of the member staging messages after publish returns
// is safe, and no Snapshot reference crosses this TU.

#include "wengine/ros2/rclcpp_transport.hpp"

#include "wengine/signal/types.hpp"

#include <cstring>
#include <utility>

namespace wengine::ros2 {

namespace {

void append_point_value(std::vector<std::uint8_t> &data, double v) {
  const std::size_t at = data.size();
  data.resize(at + sizeof(double));
  std::memcpy(data.data() + at, &v, sizeof(double));
}

} // namespace

RclcppTransport::RclcppTransport(rclcpp::Node &node, const BridgeConfig &cfg) : node_(&node), config_(cfg), qos_(ros_qos_for(cfg.qos)) {
  if (!is_valid_config(config_)) {
    return; // inert: every publish reports backpressure-free dead below
  }
  try {
    if (config_.grouped_signals) {
      grouped_pub_ = node_->create_publisher<sensor_msgs::msg::JointState>(grouped_signals_topic(config_).substr(1), qos_);
      block_pub_ = node_->create_publisher<wengine_msgs::msg::SignalBlock>(signals_block_topic(config_).substr(1), qos_);
    }
    particles_pub_ = node_->create_publisher<sensor_msgs::msg::PointCloud2>(particles_topic(config_).substr(1), qos_);
  } catch (...) {
    grouped_pub_.reset();
    block_pub_.reset();
    particles_pub_.reset();
    serial_pubs_.clear();
  }
}

void RclcppTransport::fill_stamp(std_msgs::msg::Header &header, double sim_time) noexcept {
  const RosStamp stamp = sim_time_to_stamp(sim_time);
  header.stamp.sec = stamp.sec;
  header.stamp.nanosec = stamp.nanosec;
}

void RclcppTransport::fill_particles_msg(const link::Frame &particles) noexcept {
  particles_msg_.header.frame_id = "map";
  fill_stamp(particles_msg_.header, particles.time);
  particles_msg_.height = 1;
  const std::uint64_t n = static_cast<std::uint64_t>(particles.items.size());
  particles_msg_.width = n > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
                             ? std::numeric_limits<std::uint32_t>::max()
                             : static_cast<std::uint32_t>(n);
  particles_msg_.is_bigendian = false;
  particles_msg_.is_dense = true;
  particles_msg_.fields.resize(4);
  const char *names[4] = {"x", "y", "vx", "vy"};
  for (std::uint32_t k = 0; k < 4; ++k) {
    particles_msg_.fields[k].name = names[k];
    particles_msg_.fields[k].offset = k * 8;
    particles_msg_.fields[k].datatype = sensor_msgs::msg::PointField::FLOAT64;
    particles_msg_.fields[k].count = 1;
  }
  particles_msg_.point_step = 32;
  particles_msg_.row_step = particles_msg_.point_step * particles_msg_.width;
  particles_msg_.data.clear();
  particles_msg_.data.reserve(static_cast<std::size_t>(particles_msg_.row_step));
  for (const link::FrameItem &it : particles.items) {
    append_point_value(particles_msg_.data, it.x);
    append_point_value(particles_msg_.data, it.y);
    append_point_value(particles_msg_.data, it.vx);
    append_point_value(particles_msg_.data, it.vy);
  }
}

TransportOutcome RclcppTransport::ensure_serial_pub(std::uint64_t serial, JointStatePub &out) noexcept {
  const auto it = serial_pubs_.find(serial);
  if (it != serial_pubs_.end()) {
    out = it->second;
    return out != nullptr ? TransportOutcome::kAccepted : TransportOutcome::kDead;
  }
  try {
    // Retained: despawn never removes one, so a respawned (new) serial
    // gets a fresh topic and a stale topic never carries another serial.
    // The publisher-side high-water gate bounds this set.
    JointStatePub pub = node_->create_publisher<sensor_msgs::msg::JointState>(serial_topic(config_, serial).substr(1), qos_);
    if (!pub) {
      return TransportOutcome::kDead;
    }
    serial_pubs_.emplace(serial, pub);
    out = pub;
    return TransportOutcome::kAccepted;
  } catch (...) {
    return TransportOutcome::kDead;
  }
}

TransportOutcome RclcppTransport::publish_grouped(const SignalBlock &block, const link::Frame &particles) noexcept {
  if (!grouped_pub_ || !block_pub_ || !particles_pub_) {
    return TransportOutcome::kDead;
  }
  try {
    grouped_msg_.header.frame_id = "map";
    fill_stamp(grouped_msg_.header, block.sim_time);
    grouped_msg_.name.clear();
    grouped_msg_.position.clear();
    grouped_msg_.velocity.clear();
    grouped_msg_.effort.clear();
    grouped_msg_.name.reserve(block.serial.size());
    grouped_msg_.position.reserve(block.value.size());
    for (std::size_t k = 0; k < block.serial.size(); ++k) {
      const auto qty = static_cast<signal::QuantityId>(block.quantity[k]);
      std::string name = "sig." + std::to_string(block.serial[k]) + "." + std::string(signal::quantity_suffix(qty));
      grouped_msg_.name.push_back(std::move(name));
      grouped_msg_.position.push_back(block.value[k]);
    }
    grouped_pub_->publish(grouped_msg_);
    // Exact record alongside the RViz view: same payload with serial and
    // quantity code arrays (see msg/SignalBlock.msg). No header stamp —
    // sim_time carries the engine clock, never wall time.
    block_msg_.step = block.step;
    block_msg_.sim_time = block.sim_time;
    block_msg_.serial.assign(block.serial.begin(), block.serial.end());
    block_msg_.value.assign(block.value.begin(), block.value.end());
    block_msg_.quantity.assign(block.quantity.begin(), block.quantity.end());
    block_pub_->publish(block_msg_);
    fill_particles_msg(particles);
    particles_pub_->publish(particles_msg_);
    return TransportOutcome::kAccepted;
  } catch (...) {
    return TransportOutcome::kDead;
  }
}

TransportOutcome RclcppTransport::publish_one(std::uint64_t serial, double value, std::uint8_t quantity, std::uint64_t step, double sim_time) noexcept {
  (void)step;
  if (!particles_pub_) {
    return TransportOutcome::kDead;
  }
  JointStatePub pub;
  if (ensure_serial_pub(serial, pub) != TransportOutcome::kAccepted) {
    return TransportOutcome::kDead;
  }
  try {
    const auto qty = static_cast<signal::QuantityId>(quantity);
    one_msg_.header.frame_id = "map";
    fill_stamp(one_msg_.header, sim_time);
    one_msg_.name.resize(1);
    one_msg_.position.resize(1);
    one_msg_.velocity.clear();
    one_msg_.effort.clear();
    one_msg_.name[0] = "sig." + std::to_string(serial) + "." + std::string(signal::quantity_suffix(qty));
    one_msg_.position[0] = value;
    pub->publish(one_msg_);
    return TransportOutcome::kAccepted;
  } catch (...) {
    return TransportOutcome::kDead;
  }
}

TransportOutcome RclcppTransport::publish_particles(const link::Frame &particles) noexcept {
  if (!particles_pub_) {
    return TransportOutcome::kDead;
  }
  try {
    fill_particles_msg(particles);
    particles_pub_->publish(particles_msg_);
    return TransportOutcome::kAccepted;
  } catch (...) {
    return TransportOutcome::kDead;
  }
}

} // namespace wengine::ros2
