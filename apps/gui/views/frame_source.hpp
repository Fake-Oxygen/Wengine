#pragma once

// View-model seam between engine snapshots and GUI views.
//
// Views (2D canvas, 3D viewport, graphs) consume ONLY link::Frame — the
// shared simulation view-model (link/frame.hpp), also the wengine-link
// encode input. One source of truth, not twins: views never see Snapshot
// internals and never see wire structs, so new solvers (fields, circuits,
// rigid bodies, ...) feed the same views unchanged — only a new adapter
// function plus one registry entry below are required.
//
// Plumbing:
// - FrameItem: one drawable entity (position, optional velocity stub,
//   glyph size hint, color hint with 0 = palette by index).
// - FrameChannel: one named scalar per frame for time-series plots. The
//   particle adapter describes the primary item (slot 0, the CLI p0
//   convention) plus the live count; future adapters expose probes such
//   as node voltages the same way.
// - FrameSourceDef registry: solver name -> adapter (each adapter lives
//   beside link::Frame). Snapshot carries only particles today, so unknown
//   names fall back to the particle mapping; when new domains arrive, add
//   entries here.
// - FrameHistory: bounded drop-oldest ring (matches project policy),
//   auto-cleared when a fresh run replays step 0.
// - FrameRecorder: the emit::ISink observer that adapts + pushes; best
//   effort like all sinks (never throws into the runner loop).
//
// Multi-solver note: Runner already steps N solvers per tick; the
// registry resolves names the same way, so a future multi-solver run
// only needs one adapter per member solver. Engine-only includes.

#include "wengine/emit/sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/world/state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::gui {

using FrameAdapter = link::FrameAdapter;

struct FrameSourceDef {
  std::string_view solver;
  FrameAdapter adapt;
};

inline constexpr std::array<FrameSourceDef, 2> kFrameSources = {{
    {"projectile", &link::frame_from_snapshot},
    {"signal", &link::frame_from_signal_snapshot},
}};

[[nodiscard]] inline link::Frame adapt_frame(const world::Snapshot &s,
                                             std::string_view solver) {
  for (const auto &d : kFrameSources) {
    if (d.solver == solver) {
      return d.adapt(s);
    }
  }
  return link::frame_from_snapshot(s); // only domain today: fallback
}

// Preferred plot channel: height of the primary item, else the first
// signal channel (skipping the "sig.count" metadata row), else the first
// channel. Particle behavior is unchanged (their schema keeps "p0.y").
[[nodiscard]] inline std::string default_channel(const link::Frame &f) {
  for (const auto &c : f.channels) {
    if (c.name == "p0.y") {
      return c.name;
    }
  }
  for (const auto &c : f.channels) {
    if (c.name.size() > 4 && c.name.substr(0, 4) == "sig." && c.name != "sig.count") {
      return c.name;
    }
  }
  return f.channels.empty() ? std::string{} : f.channels.front().name;
}

class FrameHistory {
 public:
  static constexpr std::size_t kCapacity = 1024;

  void push(const link::Frame &f) {
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
  [[nodiscard]] const std::vector<link::Frame> &frames() const noexcept {
    return frames_;
  }

 private:
  std::vector<link::Frame> frames_;
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
