#pragma once

// Shared simulation view-model: the single source of truth for "one frame
// of simulation output" consumed by GUI views AND the wengine-link wire
// encoder. Solver-agnostic by design: clock (step/time) plus generic
// spatial items (position, velocity, glyph hints) plus named scalar
// channels. Particles, circuits, EM fields, FEA meshes, rigid bodies —
// every domain describes itself in these terms, so views and readers never
// learn solver internals.
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

// Particle adapter: the only domain adapter today. Preserves dense slot
// order; channels describe the primary item (slot 0, the CLI p0
// convention) plus the live count. Future adapters (node voltages, field
// probes, ...) expose their scalars the same way. Allocates in the same
// cost class as take_snapshot().
[[nodiscard]] inline Frame frame_from_snapshot(const world::Snapshot &s) {
  Frame f;
  f.step = s.step_index;
  f.time = s.sim_time;
  for (std::size_t k = 0; k < s.particle_count(); ++k) {
    FrameItem it;
    it.x = s.pos_x[k];
    it.y = s.pos_y[k];
    it.vx = s.vel_x[k];
    it.vy = s.vel_y[k];
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

} // namespace wengine::link
