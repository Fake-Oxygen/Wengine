#pragma once

// Shared simulation view-model: ONE lossy projection for points-plus-scalars
// displays and the fixed-capacity SHM/socket Frame path (see
// docs/domain-outputs.md). It does not represent mesh topology, rigid-body
// orientation/constraints, or bulk tensor fields, and must not define the
// engine's scientific data limits: each domain owns typed numeric output
// blocks first and reuses Frame only when the projection is faithful.
//
// LAYERING (one source of truth, two consumers):
//   * GUI views/panels/history take `const link::Frame &` (never Snapshot
//     internals, never wire structs). New solvers add one adapter function
//     here + one frame-source registry entry; views stay untouched.
//   * link/protocol.hpp encodes Frame into fixed-capacity LinkFrame bytes
//     for transport. Wire-only concerns (frame_seq, capacity bounds) live
//     in LinkFrame, never here — transport evolution does not ripple into
//     views, and view needs never grow this struct for transport reasons.
//   * Engine Snapshot stays domain-shaped; each domain owns its adapter
//     (frame_from_snapshot() is the particle one, the only one today).
//     Snapshot must NOT grow for GUI/link needs (project rule).

#include "wengine/signal/types.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace wengine::link {

struct FrameItem {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  bool has_velocity = true;
  double size = 1.0; // glyph scale hint (rigid bodies render bigger)
  std::uint32_t color = 0; // 0 = palette by index (future: heat/color data)
  // Stable particle identity (entity slot index + generation, see
  // world/entity.hpp). Set by frame_from_snapshot when the source Snapshot
  // carries identity; legacy Snapshots/frames leave has_identity false
  // (UNAVAILABLE) — dense position never substitutes for a stable ID.
  // GUI rendering ignores these fields (glyphs by dense order); trajectory
  // use of the IDs is a separate follow-up.
  std::uint32_t entity_index = 0;
  std::uint32_t entity_generation = 0;
  bool has_identity = false;

  bool operator==(const FrameItem &) const noexcept = default;
};

struct FrameChannel {
  std::string name;
  double value = 0.0;

  bool operator==(const FrameChannel &) const noexcept = default;
};

struct Frame {
  std::uint64_t step = 0;
  double time = 0.0;
  std::vector<FrameItem> items;
  std::vector<FrameChannel> channels; // stable order per source

  bool operator==(const Frame &) const noexcept = default;
};

// Domain adapter: Snapshot (or future domain state) -> generic Frame.
// One per solver; publishers and the GUI registry take it as a parameter
// so new domains plug in without touching either consumer.
using FrameAdapter = Frame (*)(const world::Snapshot &);

// Particle adapter: the only domain adapter today. Preserves dense slot
// order plus stable identity; channels describe the primary item (slot 0,
// the CLI p0 convention) plus the live count. Future adapters (node
// voltages, field probes, ...) expose their scalars the same way.
// Allocates in the same cost class as take_snapshot(). Snapshots without
// identity (legacy/manual) yield items with has_identity false.
[[nodiscard]] inline Frame frame_from_snapshot(const world::Snapshot &s) {
  Frame f;
  f.step = s.step_index;
  f.time = s.sim_time;
  const bool have_id = s.has_particle_identity();
  for (std::size_t k = 0; k < s.particle_count(); ++k) {
    FrameItem it;
    it.x = s.pos_x[k];
    it.y = s.pos_y[k];
    it.vx = s.vel_x[k];
    it.vy = s.vel_y[k];
    if (have_id) {
      it.entity_index = s.particle_index[k];
      it.entity_generation = s.particle_generation[k];
      it.has_identity = true;
    }
    f.items.push_back(it);
  }
  f.channels.push_back({"count", static_cast<double>(s.particle_count())});
  if (!f.items.empty()) {
    const FrameItem &p = f.items.front();
    f.channels.push_back({"p0.x", p.x});
    f.channels.push_back({"p0.y", p.y});
    f.channels.push_back({"p0.vx", p.vx});
    f.channels.push_back({"p0.vy", p.vy});
    f.channels.push_back({"p0.speed", std::hypot(p.vx, p.vy)});
  }
  return f;
}

// Inverse particle adapter for the record path (bus -> CSV): Frame items
// become dense Snapshot slots in order, step/time carried over, identity
// carried when every item has it (else UNAVAILABLE: Snapshot identity
// vectors stay empty, never fabricated from dense positions). Lossy by
// format necessity, documented: item z/vz have no CSV column and are
// dropped, channels are ignored (CSV is particle-shaped). Within those
// bounds it is exact — snapshot_from_frame(frame_from_snapshot(s)) == s —
// so a bus recording of a CSV playback is byte-identical to the source
// log. May allocate (vectors); never call inside the sim loop.
[[nodiscard]] inline world::Snapshot snapshot_from_frame(const Frame &f) {  world::Snapshot s;
  s.step_index = f.step;
  s.sim_time = f.time;
  s.pos_x.reserve(f.items.size());
  s.pos_y.reserve(f.items.size());
  s.vel_x.reserve(f.items.size());
  s.vel_y.reserve(f.items.size());
  bool all_have_id = true;
  for (const FrameItem &it : f.items) {
    if (!it.has_identity) {
      all_have_id = false;
      break;
    }
  }
  if (all_have_id && !f.items.empty()) {
    s.particle_index.reserve(f.items.size());
    s.particle_generation.reserve(f.items.size());
  }
  for (const FrameItem &it : f.items) {
    s.pos_x.push_back(it.x);
    s.pos_y.push_back(it.y);
    s.vel_x.push_back(it.vx);
    s.vel_y.push_back(it.vy);
    if (all_have_id && !f.items.empty()) {
      s.particle_index.push_back(it.entity_index);
      s.particle_generation.push_back(it.entity_generation);
    }
  }
  return s;
}

// Signal adapter (SIG-2): Snapshot signal arrays -> generic Frame. One
// channel per live source (`sig.<slot>.U`, suffix per stamped quantity)
// plus the live count; no items (signals are scalar probes, not glyphs).
// Deterministic: dense slot order in, channel order out, so round-trips
// are exactly equal. Allocates like the particle adapter above.
[[nodiscard]] inline Frame frame_from_signal_snapshot(const world::Snapshot &s) {
  Frame f;
  f.step = s.step_index;
  f.time = s.sim_time;
  f.channels.reserve(s.signal_count() + 1);
  f.channels.push_back({"sig.count", static_cast<double>(s.signal_count())});
  for (std::size_t k = 0; k < s.signal_count(); ++k) {
    const auto qty = static_cast<signal::QuantityId>(s.sig_quantity[k]);
    std::string name = "sig." + std::to_string(s.sig_serial[k]) + "." +
                       std::string(signal::quantity_suffix(qty));
    f.channels.push_back({name, s.sig_value[k]});
  }
  return f;
}

} // namespace wengine::link
