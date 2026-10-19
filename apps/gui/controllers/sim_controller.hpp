#pragma once

// Frontend-owned stepped-sim driver for apps/gui (Dear ImGui panel) and its
// unit test. Owns a solvers::ConfiguredRun (shared validation/population
// with solvers::run_headless() — see solvers/configured_run.hpp), rebuilds
// it from a plain core::RunConfig on every accepted edit, and exposes
// snapshots/status for display. Transport: play (wall-clock advance),
// pause, exact single-step, reset to the current config.
//
// Determinism contract: rebuild + fixed-step driving reproduce
// solvers::run_headless() bit-for-bit for the same config (same Runner
// sequencing, same spawn order and initial values) because both frontends
// use the one shared builder — there is no duplicated spawn block left to
// drift.
//
// Per-source control (SIG-10 verbs): the controller also owns a
// solvers::ControlBus with one local channel (GUI). Panels enqueue the same
// ADD/SET/START/STOP/DROP/LIST lines the CLI and Zephyr clients speak; the
// bus drains at deterministic step boundaries (before every step in
// run_exact, once per advance_frame call in play mode), in fixed channel
// order. An empty bus drains as a no-op, so verb-free runs stay
// bit-identical to headless. Table/Lua sources need bulk payloads and ride
// batch rebuilds, never one-line verbs (same rule as the CLI family).
//
// Scripting runtime (SIG-4b): caller-owned extra solvers plus an explicit
// has_lua_runtime flag thread into the shared builder (defaults: no extras,
// no Lua — current behavior preserved). The controller never includes Lua
// headers (wengine_core only): the GUI app owns the Lua solver, passes it
// as an extra, and lends the solver's error string for inline display (see
// set_script_error). Without the runtime, kLua configs are rejected by the
// shared builder with a message.
//
// Engine-only includes: no ImGui, SDL, or Vulkan here, so unit tests link
// wengine_core alone. GUI owns all I/O; failures surface as Status plus an
// inquiryable message (frontend code may allocate — the hot path inside
// Runner::run/advance still does not).

#include "wengine/core/run_config.hpp"
#include "wengine/emit/sink.hpp"
#include "wengine/solvers/configured_run.hpp"
#include "wengine/solvers/signal_control.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wengine::gui {

class SimController {
 public:
  // Local control-bus provenance id (answers HELLO from the panel verbs;
  // never enters simulation state, so it cannot perturb determinism).
  static constexpr std::uint64_t kGuiRunId = 1;

  SimController() : bus_(kGuiRunId) {
    bus_.add_channel(); // channel 0: local GUI (first in drain order)
    core::RunConfig cfg;
    cfg.loop.dt = 0.01;
    cfg.loop.emit_every = 1;
    cfg.steps = 100;
    cfg.solver = solvers::ProjectileSolver::kName;
    cfg.gravity = 9.81;
    cfg.has_gravity = false;
    cfg.particles = 1;
    cfg.x0 = 0.0;
    cfg.y0 = 0.0;
    cfg.vx0 = 10.0;
    cfg.vy0 = 10.0;
    last_status_ = set_config(cfg);
  }

  // Validate and adopt a new config, rebuilding the run from step 0.
  // Rejected edits leave the live run untouched; the message explains why.
  // Extra solvers are caller-owned (borrowed, stepped before the primary in
  // span order) and kept for reset(); has_lua_runtime declares a live Lua
  // scripting runtime (a "lua" extra must still be present — a dummy
  // non-Lua span, a Lua extra without the flag, and null entries all
  // reject, exactly like the headless driver). Defaults preserve the
  // runtime-free behavior. Panels forward extras()/has_lua_runtime() on
  // every edit so batch tweaks never silently drop the runtime.
  core::Status set_config(const core::RunConfig &cfg,
                          std::span<core::ISolver *> extra = {},
                          bool has_lua_runtime = false) {
    solvers::ConfiguredRun next;
    std::string message;
    const core::Status st = solvers::ConfiguredRun::build(
        cfg, extra, has_lua_runtime, next, &message);
    if (!st.ok()) {
      error_ = message.empty() ? "invalid config" : message;
      last_status_ = st;
      return last_status_;
    }
    config_ = cfg;
    // Copy first: callers forward extras() (a span over extras_ itself),
    // and vector::assign from an overlapping range is undefined.
    const std::vector<core::ISolver *> keep(extra.begin(), extra.end());
    extras_ = keep;
    has_lua_runtime_ = has_lua_runtime;
    run_ = std::move(next);
    for (emit::ISink *sink : observers_) {
      run_.add_sink(*sink);
    }
    error_.clear();
    last_status_ = core::Status::success();
    return last_status_;
  }

  // Lend the scripting solver's error string for inline display (borrowed,
  // never consulted on the hot path: the app owns the Lua solver and keeps
  // it alive while the controller runs; nullptr clears). Script failures
  // still fail the step loudly through the normal Status path.
  void set_script_error(const std::string *text) noexcept { script_error_ = text; }

  [[nodiscard]] std::string script_error() const {
    return (script_error_ != nullptr) ? *script_error_ : std::string{};
  }

  // Rebuild from the current (already validated) config plus the stored
  // extras/flag. Step index and clock return to 0; transport keeps its
  // play/pause state. The verb queue survives (undrained lines apply to the
  // fresh run at step 0); applied effects do not (same as any rebuild).
  void reset() {
    solvers::ConfiguredRun next;
    std::string message;
    const core::Status st = solvers::ConfiguredRun::build(
        config_, std::span<core::ISolver *>(extras_.data(), extras_.size()),
        has_lua_runtime_, next, &message);
    if (!st.ok()) {
      error_ = message.empty() ? "signal setup failed" : message;
      last_status_ = st;
      return; // unreachable post-validation; live run left intact
    }
    run_ = std::move(next);
    for (emit::ISink *sink : observers_) {
      run_.add_sink(*sink);
    }
    error_.clear();
    last_status_ = core::Status::success();
  }

  // Enqueue one SIG-10 verb line on the local channel (same vocabulary the
  // CLI `sig add/set/start/stop/drop` and Zephyr clients speak: unit
  // suffixes are a panel concern — the bus carries SI doubles). Drained at
  // step boundaries (see run_exact/advance_frame) or immediately via
  // drain_commands(); verdicts surface through take_responses().
  bool send_command(std::string line) { return bus_.enqueue(0, std::move(line)); }

  // Drain the local queue against the live state at the current step.
  // Empty-queue drains are no-ops (verb-free runs stay bit-identical).
  solvers::DrainStats drain_commands() {
    return bus_.drain(run_.state(), run_.state().step_index);
  }

  // Collect and clear the local channel's pending verdict lines.
  [[nodiscard]] std::vector<std::string> take_responses() {
    return bus_.take_responses(0);
  }

  [[nodiscard]] const std::vector<solvers::AppliedCommand> &command_log() const noexcept {
    return bus_.log();
  }

  // Advance exactly `steps` fixed steps (single-step = run_exact(1)).
  // Deterministic: identical call sequences give identical states. The bus
  // drains before every step, so verbs apply at exact steps and log for
  // replay (an empty bus drains as a no-op: Runner::run(n) is already a
  // step_once loop, so verb-free parity with headless is exact).
  // A solver failure stops play: the Runner latches the failure and its
  // live state holds a partial step, not a completed tick (see
  // core/runner.hpp) — reset() rebuilds before stepping again.
  core::Status run_exact(std::uint64_t steps) {
    for (std::uint64_t k = 0; k < steps; ++k) {
      (void)drain_commands();
      last_status_ = run_.run(1);
      if (!last_status_.ok()) {
        playing_ = false;
        error_ = "simulation step failed: partial step state is not a completed tick — reset to rebuild";
        return last_status_;
      }
    }
    return last_status_;
  }

  // Fold wall-clock time into fixed steps (play mode). Capped per call by
  // RunnerConfig::max_substeps, so stalls cannot spiral. While playing, the
  // frame is additionally clamped to the remaining step budget, so play
  // parks exactly at `steps` and Reset + Play reproduces a headless batch
  // bit-for-bit regardless of frame rate. Reaching the budget auto-pauses.
  // A solver failure also auto-pauses with the same partial-step error as
  // run_exact (the Runner latches; reset() rebuilds).
  // Verbs drain once per call (frame boundaries, not substep boundaries:
  // exact verb timing in play mode is wall-driven by construction).
  core::Status advance_frame(double frame_dt_seconds) {
    (void)drain_commands();
    if (playing_) {
      const std::uint64_t done = run_.state().step_index;
      if (done < config_.steps) {
        const std::uint64_t remaining = config_.steps - done;
        const double budget_dt =
            static_cast<double>(remaining) * config_.loop.dt;
        if (frame_dt_seconds > budget_dt) {
          frame_dt_seconds = budget_dt;
        }
      }
    }
    last_status_ = run_.runner().advance(frame_dt_seconds);
    if (!last_status_.ok()) {
      playing_ = false;
      error_ = "simulation step failed: partial step state is not a completed tick — reset to rebuild";
      return last_status_;
    }
    if (run_.state().step_index >= config_.steps) {
      playing_ = false;
    }
    return last_status_;
  }

  void set_playing(bool playing) noexcept { playing_ = playing; }
  [[nodiscard]] bool playing() const noexcept { return playing_; }

  // Attach a best-effort snapshot observer (graphs, future views). The
  // controller never owns sinks: callers keep them alive for as long as
  // the controller runs. Re-attached automatically on every rebuild, so
  // config edits and resets never silently detach observers.
  void add_sink(emit::ISink &sink) {
    observers_.push_back(&sink);
    run_.add_sink(sink);
  }

  [[nodiscard]] const core::RunConfig &config() const noexcept { return config_; }
  // Stored runtime (for panels to forward on every set_config so batch
  // tweaks preserve it). Borrowed pointers; the caller keeps extras alive.
  [[nodiscard]] std::span<core::ISolver *> extras() noexcept {
    return std::span<core::ISolver *>(extras_.data(), extras_.size());
  }
  [[nodiscard]] bool has_lua_runtime() const noexcept { return has_lua_runtime_; }
  [[nodiscard]] const world::State &state() const noexcept {
    return run_.state();
  }
  // Independent deep copy: safe to hold across rebuilds and frames.
  [[nodiscard]] world::Snapshot snapshot() const {
    return world::take_snapshot(run_.state());
  }
  [[nodiscard]] bool ok() const noexcept { return last_status_.ok(); }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

 private:
  core::RunConfig config_{};
  // Shared builder output: owns the primary solver + initialized Runner.
  // Caller-owned extras (borrowed) + the Lua-runtime flag, stored on every
  // accepted set_config for reset() and for panels to forward.
  std::vector<core::ISolver *> extras_;
  bool has_lua_runtime_ = false;
  solvers::ControlBus bus_;
  // Borrowed scripting-solver error text for inline display (see setter).
  const std::string *script_error_ = nullptr;
  solvers::ConfiguredRun run_;
  // Non-owning observers, re-attached on every rebuild (see add_sink).
  std::vector<emit::ISink *> observers_;
  bool playing_ = false;
  core::Status last_status_{core::StatusCode::kInvalidArgument};
  std::string error_{"not configured"};
};

} // namespace wengine::gui
