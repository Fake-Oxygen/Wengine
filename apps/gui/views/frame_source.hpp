#pragma once

// Generic view-model seam between engine snapshots and GUI views.
//
// Views (2D canvas, 3D viewport, graphs) consume ONLY DataFrame: spatial
// items plus named scalar channels. They never see Snapshot particle
// arrays, so new solvers (fields, circuits, rigid bodies, ...) feed the
// same views unchanged — only a new adapter entry is required.
//
// Plumbing:
// - SpatialItem: one drawable entity (position, optional velocity stub,
//   glyph size hint, color hint with 0 = palette by index).
// - Channel: one named scalar per frame for time-series plots. The
//   particle adapter describes the primary item (slot 0, the CLI p0
//   convention) plus the live count; future adapters expose probes such
//   as node voltages the same way.
// - FrameSourceDef registry: solver name -> adapter. Snapshot carries
//   only particles today, so unknown names fall back to the particle
//   mapping; when Snapshot gains new domains, add entries here.
// - FrameHistory: bounded drop-oldest ring (matches project policy),
//   auto-cleared when a fresh run replays step 0.
// - FrameRecorder: the emit::ISink observer that adapts + pushes; best
//   effort like all sinks (never throws into the runner loop).
//
// Multi-solver note: Runner already steps N solvers per tick; the
// registry resolves names the same way, so a future multi-solver run
// only needs one adapter per member solver. Engine-only includes.

#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::gui {

struct Channel {
  std::string name;
  double value = 0.0;
};

struct SpatialItem {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  bool has_velocity = true;
  double size = 1.0; // glyph scale hint (rigid bodies render bigger)
  std::uint32_t color = 0; // 0 = palette by index (future: heat/color data)
};

struct DataFrame {
  std::uint64_t step = 0;
  double time = 0.0;
  std::vector<SpatialItem> items;
  std::vector<Channel> channels; // stable order per source
};

[[nodiscard]] inline DataFrame frame_from_particles(const world::Snapshot &s) {
  DataFrame f;
  f.step = s.step_index;
  f.time = s.sim_time;
  for (std::size_t k = 0; k < s.particle_count(); ++k) {
    SpatialItem it;
    it.x = s.pos_x[k];
    it.y = s.pos_y[k];
    it.vx = s.vel_x[k];
    it.vy = s.vel_y[k];
    f.items.push_back(it);
  }
  f.channels.push_back({"count", static_cast<double>(s.particle_count())});
  if (!f.items.empty()) {
    const SpatialItem &p = f.items.front();
    f.channels.push_back({"p0.x", p.x});
    f.channels.push_back({"p0.y", p.y});
    f.channels.push_back({"p0.vx", p.vx});
    f.channels.push_back({"p0.vy", p.vy});
    f.channels.push_back({"p0.speed", std::hypot(p.vx, p.vy)});
  }
  return f;
}

using FrameAdapter = DataFrame (*)(const world::Snapshot &);

struct FrameSourceDef {
  std::string_view solver;
  FrameAdapter adapt;
};

inline constexpr std::array<FrameSourceDef, 1> kFrameSources = {{
    {"projectile", &frame_from_particles},
}};

[[nodiscard]] inline DataFrame adapt_frame(const world::Snapshot &s,
                                           std::string_view solver) {
  for (const auto &d : kFrameSources) {
    if (d.solver == solver) {
      return d.adapt(s);
    }
  }
  return frame_from_particles(s); // snapshot is particle-shaped: fallback
}

// Preferred plot channel: height of the primary item, else first channel.
[[nodiscard]] inline std::string default_channel(const DataFrame &f) {
  for (const auto &c : f.channels) {
    if (c.name == "p0.y") {
      return c.name;
    }
  }
  return f.channels.empty() ? std::string{} : f.channels.front().name;
}

class FrameHistory {
 public:
  static constexpr std::size_t kCapacity = 1024;

  void push(const DataFrame &f) {
    if (!frames_.empty() && f.step <= frames_.back().step) {
      frames_.clear();
    }
    if (frames_.size() >= kCapacity) {
      frames_.erase(frames_.begin());
    }
    frames_.push_back(f);
  }

  void clear() noexcept { frames_.clear(); }
  [[nodiscard]] bool empty() const noexcept { return frames_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return frames_.size(); }
  [[nodiscard]] const std::vector<DataFrame> &frames() const noexcept {
    return frames_;
  }

 private:
  std::vector<DataFrame> frames_;
};

class FrameRecorder : public emit::ISink {
 public:
  explicit FrameRecorder(FrameHistory &history) : history_(history) {}

  void set_solver(std::string_view solver) { solver_ = std::string(solver); }

  void on_snapshot(const world::Snapshot &s) override {
    try {
      history_.push(adapt_frame(s, solver_));
    } catch (...) {
      // Best-effort: drop the frame, never break the simulation loop.
    }
  }

 private:
  FrameHistory &history_;
  std::string solver_{"projectile"};
};

} // namespace wengine::gui
