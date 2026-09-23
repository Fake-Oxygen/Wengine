#pragma once

// Central simulation state: clock (sim_time/step_index) + entity registry +
// per-domain SoA component pools (particles, signal sources; same shape for
// more). State owns consistency: spawn_entity() touches the registry only,
// despawn_entity() detaches every pool before releasing the slot.
//
// Snapshot semantics: deep copy. take_snapshot() copies the clock plus the
// live particle arrays (dense, slot order), so sinks never borrow state
// lifetime — safe for decimated, file, or future async emit. The copy cost
// is bounded by live data and paid only on emit steps; a zero-copy view
// can be added later if profiling demands it (see emit/sink.hpp).

#include "wengine/world/entity.hpp"
#include "wengine/world/particles.hpp"
#include "wengine/world/signals.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wengine::world {

struct State {
  double sim_time{0.0};
  std::uint64_t step_index{0};
  EntityRegistry entities;
  Particles particles;
  Signals signals;

  // Registry-only spawn; attach components (particles.attach(...)) opt-in.
  Entity spawn_entity() { return entities.spawn(); }

  // False when `e` is stale/foreign. Detaches pools, then releases the slot.
  bool despawn_entity(Entity e) {
    if (!entities.alive(e)) {
      return false;
    }
    particles.detach(e.index);
    signals.detach(e.index);
    return entities.despawn(e);
  }

  void clear() noexcept {
    sim_time = 0.0;
    step_index = 0;
    entities.clear();
    particles.clear();
    signals.clear();
  }

  bool operator==(const State &) const noexcept = default;
};

struct Snapshot {
  double sim_time{0.0};
  std::uint64_t step_index{0};
  // Dense copy of live particles in slot order (deterministic).
  std::vector<double> pos_x;
  std::vector<double> pos_y;
  std::vector<double> vel_x;
  std::vector<double> vel_y;
  // Dense copy of live signal sources in slot order (deterministic):
  // stable serial, SI value, and QuantityId tag per source.
  std::vector<std::uint64_t> sig_serial;
  std::vector<double> sig_value;
  std::vector<std::uint8_t> sig_quantity;

  [[nodiscard]] std::size_t particle_count() const noexcept { return pos_x.size(); }
  [[nodiscard]] std::size_t signal_count() const noexcept { return sig_value.size(); }

  bool operator==(const Snapshot &) const noexcept = default;
};

[[nodiscard]] inline Snapshot take_snapshot(const State &s) {
  Snapshot snap;
  snap.sim_time = s.sim_time;
  snap.step_index = s.step_index;
  snap.pos_x.reserve(s.particles.live_count);
  snap.pos_y.reserve(s.particles.live_count);
  snap.vel_x.reserve(s.particles.live_count);
  snap.vel_y.reserve(s.particles.live_count);
  for (std::uint32_t i = 0; i < s.particles.capacity(); ++i) {
    if (s.particles.has(i)) {
      snap.pos_x.push_back(s.particles.pos_x[i]);
      snap.pos_y.push_back(s.particles.pos_y[i]);
      snap.vel_x.push_back(s.particles.vel_x[i]);
      snap.vel_y.push_back(s.particles.vel_y[i]);
    }
  }
  snap.sig_serial.reserve(s.signals.live_count);
  snap.sig_value.reserve(s.signals.live_count);
  snap.sig_quantity.reserve(s.signals.live_count);
  for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
    if (s.signals.has(i)) {
      snap.sig_serial.push_back(s.signals.serial[i]);
      snap.sig_value.push_back(s.signals.value[i]);
      snap.sig_quantity.push_back(s.signals.quantity[i]);
    }
  }
  return snap;
}

// Subscription filter (SIG-5 routing): copy of `s` carrying only the
// listed source serials, in snapshot (slot) order. Unknown or despawned
// serials are skipped — subscribers validate loudly at subscribe time via
// their own counters (see emit/routing.hpp), while the per-snapshot path
// stays infallible and allocation-bounded like take_snapshot. Particles
// pass through whole (filtering is signal-scoped).
[[nodiscard]] inline Snapshot select_signals(const Snapshot &s,
                                             std::span<const std::uint64_t> serials) {
  Snapshot out;
  out.sim_time = s.sim_time;
  out.step_index = s.step_index;
  out.pos_x = s.pos_x;
  out.pos_y = s.pos_y;
  out.vel_x = s.vel_x;
  out.vel_y = s.vel_y;
  for (std::size_t k = 0; k < s.signal_count(); ++k) {
    for (std::uint64_t want : serials) {
      if (s.sig_serial[k] == want) {
        out.sig_serial.push_back(s.sig_serial[k]);
        out.sig_value.push_back(s.sig_value[k]);
        out.sig_quantity.push_back(s.sig_quantity[k]);
        break;
      }
    }
  }
  return out;
}

} // namespace wengine::world
