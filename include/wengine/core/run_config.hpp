#pragma once

// Headless-run seam between frontends (CLI today, GUI tomorrow) and the
// engine. Frontends fill a RunConfig with plain data and hand it to
// solvers::run_headless() (see solvers/run.hpp); the engine consumes the
// config only and never frontend types — no CLI/GUI includes here or below.
//
// Layout: `loop` reuses core::RunnerConfig so fixed-step tuning has one
// source of truth (max_substeps only matters for advance()-style driving;
// batch runs ignore it). `solver` is a registry name resolved engine-side
// (empty matches nothing and is rejected — the choice stays explicit).
// `gravity` is projectile-scoped: honored for solver "projectile",
// rejected with any other solver. Particle population is generic setup.
// Sinks are NOT part of the config: callers create and own them (file,
// console, future SHM writer) and pass them to the driver, so all I/O
// wiring stays at the edge.

#include "wengine/core/runner.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::core {

struct RunConfig {
  RunnerConfig loop; // dt, emit_every, max_substeps
  std::uint64_t steps = 100; // exact batch length (>= 1)
  std::string_view solver; // registry name, e.g. "projectile"
  double gravity = 9.81; // projectile-scoped
  bool has_gravity = false; // set when gravity was explicitly provided
  std::uint64_t particles = 1; // spawned identically at (x0, y0, vx0, vy0)
  double x0 = 0.0;
  double y0 = 0.0;
  double vx0 = 10.0;
  double vy0 = 10.0;
  // Signal-scoped (SIG-2): honored for solver "signal", rejected with any
  // other solver (same pattern as gravity). `signals` sources spawn with
  // identical params; per-source tuning is SIG-6/SIG-10 control-plane work.
  std::string_view sig_kind = "sine";
  double sig_freq = 1.0; // Hz
  double sig_amp = 1.0; // SI peak (V or A per sig_quantity)
  double sig_offset = 0.0; // SI base
  double sig_phase = 0.0; // rad
  double sig_duty = 0.5; // square/pulse high fraction
  std::uint64_t sig_seed = 1; // noise only
  std::string_view sig_quantity = "U"; // U|V (voltage) or I|A (current)
  std::uint64_t signals = 1;
  bool has_signal_opts = false; // set when any --sig-* was explicitly provided
};

} // namespace wengine::core
