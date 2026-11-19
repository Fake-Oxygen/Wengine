#pragma once

/**
 * @file
 * The headless-run seam between frontends (CLI today, GUI tomorrow) and
 * the engine: frontends fill a RunConfig with plain data and hand it to
 * the run driver (solvers/run.hpp); the engine consumes the config only
 * and never frontend types — no CLI/GUI includes here or below.
 * Sinks are NOT part of the config: callers create and own them and
 * pass them to the driver, so all I/O wiring stays at the edge. Each
 * solver owns its section struct beside itself and RunConfig aggregates
 * one member per solver; flat per-solver fields are forbidden (gravity
 * is the grandfathered exception).
 */

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
// Solver-scoped sections (currently `signal` only) follow one rule: each
// solver owns its section struct beside itself, RunConfig aggregates one
// member per solver, and the driver honors a section iff its solver is
// selected — rejecting it when `set` with any other solver. New solvers
// add a section, never flat fields (gravity is grandfathered flat).
// Sinks are NOT part of the config: callers create and own them (file,
// console, future SHM writer) and pass them to the driver, so all I/O
// wiring stays at the edge.

#include "wengine/core/runner.hpp"
#include "wengine/logic/seq_config.hpp"
#include "wengine/signal/types.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::core {

/**
 * @brief Plain-data description of one configured run: loop tuning,
 *        solver choice, generic setup, and one nested section per
 *        solver domain.
 *
 * @note Section discipline: the driver honors a section iff its solver
 *       is selected and rejects it with any other solver when set.
 *       New solvers add a section struct (owned beside themselves) and
 *       one member here — never flat fields; `gravity` is the
 *       grandfathered flat exception. All values are parsed data, no
 *       heap strings: `solver` is a string_view and must point at
 *       storage the caller keeps alive for the config's lifetime.
 */
struct RunConfig {
  /**
   * @brief Fixed-step loop tuning: dt in seconds (_s), emit cadence,
   *        and the stall guard.
   */
  RunnerConfig loop; // dt, emit_every, max_substeps
  /**
   * @brief Exact batch length for headless runs (>= 1).
   */
  std::uint64_t steps = 100; // exact batch length (>= 1)
  /**
   * @brief Solver registry name to run (e.g. "projectile"); empty
   *        matches nothing and is rejected — the choice stays explicit.
   */
  std::string_view solver; // registry name, e.g. "projectile"
  /**
   * @brief Projectile-scoped gravitational acceleration in metres per
   *        second squared (m/s^2); honored for solver "projectile"
   *        only, rejected with any other solver.
   */
  double gravity = 9.81; // projectile-scoped
  /**
   * @brief True when gravity was explicitly provided (drives the
   *        set/unset rejection logic).
   */
  bool has_gravity = false; // set when gravity was explicitly provided
  /**
   * @brief Particle population spawned identically at (x0, y0, vx0,
   *        vy0); generic setup, not solver-scoped.
   */
  std::uint64_t particles = 1; // spawned identically at (x0, y0, vx0, vy0)
  /**
   * @brief Initial particle x position in metres (_m).
   */
  double x0 = 0.0;
  /**
   * @brief Initial particle y position in metres (_m).
   */
  double y0 = 0.0;
  /**
   * @brief Initial particle x velocity in metres per second (_m/s).
   */
  double vx0 = 10.0;
  /**
   * @brief Initial particle y velocity in metres per second (_m/s).
   */
  double vy0 = 10.0;
  /**
   * @brief Signal-scoped section (count/params/set), parsed values with
   *        no strings; honored iff solver "signal" is selected,
   *        rejected when set otherwise.
   */
  signal::SignalRunConfig signal; // signal-scoped (count/params/set)
  /**
   * @brief Logic/sequential-scoped section (netlist/stimulus/members/
   *        set); honored iff the sequential solver is selected,
   *        rejected when set otherwise.
   */
  logic::SequentialRunConfig seq; // logic/sequential-scoped (netlist/stimulus/members/set)
};

} // namespace wengine::core
