// ROS2 state-out publisher: transport-neutral ISink half (no rclcpp here).
//
// This TU is ROS-free by construction (same rule as src/link/*.cpp owning
// POSIX syscalls while link headers stay platform-free): it adapts each
// Snapshot into owned staging (block_from_snapshot for the grouped
// payload, frame_from_signal_snapshot for the per-signal channel check,
// frame_from_snapshot for particles), runs the shared over-bound gate,
// then fans out through the caller-provided RosTransport. The rmw calls
// live in rclcpp_transport.cpp (BUILD_ROS2=ON only). Compiling this TU in
// the default build keeps that build ROS-free by construction.

#include "wengine/ros2/publisher.hpp"

#include <utility>

namespace wengine::ros2 {

namespace {

[[nodiscard]] std::uint64_t estimated_grouped_bytes(const SignalBlock &block, const link::Frame &particles) noexcept {
  // Local-acceptance estimate (names + SI doubles + clocks), the
  // bytes_accepted leg of the shared accounting. Remote receipt is
  // unknown without a receiver LagStats (see delivery.md).
  std::uint64_t bytes = 16; // step + sim_time
  bytes += static_cast<std::uint64_t>(block.serial.size()) * 17; // serial + value + quantity
  for (const auto &ch : particles.channels) {
    bytes += static_cast<std::uint64_t>(ch.name.size()) + 8;
  }
  bytes += static_cast<std::uint64_t>(particles.items.size()) * 48; // x/y/vx/vy + framing
  return bytes;
}

[[nodiscard]] std::uint64_t estimated_one_bytes(const link::Frame &particles) noexcept {
  std::uint64_t bytes = 32; // channel name + value + clocks
  bytes += static_cast<std::uint64_t>(particles.items.size()) * 48;
  return bytes;
}

} // namespace

RosPublisher::RosPublisher(const BridgeConfig &cfg, RosTransport &transport) : config_(cfg), transport_(&transport) {
  if (!is_valid_config(config_)) {
    status_ = {core::StatusCode::kInvalidArgument};
  }
}

void RosPublisher::on_snapshot(const world::Snapshot &snapshot) {
  ++stats_.attempted;
  if (!status_.ok()) {
    ++stats_.disabled_after_error;
    return;
  }
  // Owned copies before anything else: the Runner reuses its emission
  // buffer, so the Snapshot reference is valid only for this call (see
  // emit/sink.hpp). Staging vectors reuse capacity across steady-shape
  // emissions, like take_snapshot/fill_snapshot.
  try {
    staging_block_ = block_from_snapshot(snapshot);
    staging_signals_ = link::frame_from_signal_snapshot(snapshot);
    staging_particles_ = link::frame_from_snapshot(snapshot);
  } catch (...) {
    // Best-effort observer: sticky failure, never break the sim loop.
    status_ = {core::StatusCode::kInternal};
    ++stats_.disabled_after_error;
    return;
  }
  // Shared over-bound gate (LinkFrame parity, loud never truncated).
  if (staging_block_.serial.size() > kMaxRosSignals ||
      staging_particles_.items.size() > kMaxRosParticles) {
    ++stats_.dropped_over_bound;
    status_ = {core::StatusCode::kOutOfRange};
    return;
  }
  for (const link::FrameChannel &ch : staging_signals_.channels) {
    if (ch.name.size() >= link::kChannelNameSize) {
      ++stats_.dropped_over_bound;
      status_ = {core::StatusCode::kOutOfRange};
      return;
    }
  }
  // Per-serial publisher backstop (per-serial mode only): serials are
  // dense from 0 per State and never reused, so the high-water mark
  // bounds the retained per-signal publisher set. Grouped mode
  // advertises a fixed 2 topics and needs no such bound.
  if (!config_.grouped_signals) {
    for (std::uint64_t serial : staging_block_.serial) {
      if (!have_serial_ || serial > max_serial_seen_) {
        have_serial_ = true;
        max_serial_seen_ = serial;
      }
      if (serial >= static_cast<std::uint64_t>(kMaxRosSerialTopics)) {
        ++stats_.dropped_over_bound;
        status_ = {core::StatusCode::kOutOfRange};
        return;
      }
    }
  }
  // Fan-out through the transport (synchronous; it copies before
  // returning and never retains our staging refs). A throwing transport
  // is a dead peer: sticky, never an exception out of a sink.
  TransportOutcome outcome = TransportOutcome::kAccepted;
  try {
    if (config_.grouped_signals) {
      outcome = transport_->publish_grouped(staging_block_, staging_particles_);
    } else {
      const std::size_t n = staging_block_.serial.size();
      for (std::size_t k = 0; k < n; ++k) {
        outcome = transport_->publish_one(staging_block_.serial[k], staging_block_.value[k],
                                          staging_block_.quantity[k], staging_block_.step,
                                          staging_block_.sim_time);
        if (outcome != TransportOutcome::kAccepted) {
          break;
        }
      }
      if (outcome == TransportOutcome::kAccepted) {
        outcome = transport_->publish_particles(staging_particles_);
      }
    }
  } catch (...) {
    outcome = TransportOutcome::kDead;
  }
  if (outcome == TransportOutcome::kAccepted) {
    ++stats_.published;
    if (config_.grouped_signals) {
      stats_.bytes_accepted += estimated_grouped_bytes(staging_block_, staging_particles_);
    } else {
      stats_.bytes_accepted += static_cast<std::uint64_t>(staging_block_.serial.size()) * estimated_one_bytes(staging_particles_);
    }
    return;
  }
  if (outcome == TransportOutcome::kBackpressure) {
    ++stats_.dropped_send; // transient: counted, never sticky
    return;
  }
  status_ = {core::StatusCode::kInternal}; // dead peer: sticky, like SHM teardown
  ++stats_.disabled_after_error;
}

} // namespace wengine::ros2
