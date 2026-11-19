#pragma once

/**
 * @file
 * Shared configured-run builder: the single validation and
 * initialization path behind solvers::run_headless() (headless/CLI) and
 * gui::SimController (GUI) — both frontends step the same population in
 * the same order. Owns the primary solver lifetime and the initialized
 * Runner state; engine-only (no frontend I/O, no Lua headers, no
 * ImGui/SDL/Vulkan, never CLI types). Consumed by run.hpp, the `sim
 * run`/`sig run` commands, and the GUI sim controller.
 */

// Shared configured-run builder: the single validation + initialization
// implementation behind solvers::run_headless() (headless/CLI) and
// gui::SimController (GUI). Owns the primary solver lifetime and the
// initialized Runner state so both drivers step the same population in
// the same order; callers keep owning their sinks and any extra solvers.
//
// Engine-only: no frontend I/O, no Lua headers, no ImGui/SDL/Vulkan.
// Config-edge failures return kInvalidArgument and, when `error` is
// non-null, fill it with the same human-readable text the GUI controller
// has always surfaced (dt/steps/solver scoping, signal payload placement,
// table/CSV, Lua script/runtime, setup). The headless wrapper passes
// nullptr and maps to Status only, preserving its contract.
//
// Chunked driving: the returned run keeps its state and owned solvers, so
// callers may invoke run(N) repeatedly (paced GUI frames, wall-clock
// batching) instead of rebuilding per chunk. Sinks stay caller-owned and
// are (re)attached by the caller — rebuilds reattach, rejections leave the
// live run untouched (the controller builds into a temp first).
//
// Lua runtime (SIG-4b) is explicit: kLua batch sources require BOTH
// `has_lua_runtime == true` AND an extra solver named "lua" in `extra`.
// An arbitrary nonempty extra span (dummy/mock, null, wrong kind) must not
// satisfy it. The name literal intentionally mirrors
// lua::LuaSignalSolver::kName without including signal/lua.hpp, keeping
// this header Lua-free; the CLI derives the flag from its actual extras,
// the GUI passes false (no scripting runtime in wengine_core).

#include "wengine/core/run_config.hpp"
#include "wengine/core/runner.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_config.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/world/state.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace wengine::solvers {

// Literal for the scripting runtime solver name (see header comment: kept
// as a literal so this header never includes signal/lua.hpp).
/** @brief Scripting-runtime solver name ("lua"), mirroring lua::LuaSignalSolver::kName. */
inline constexpr std::string_view kLuaSolverName = "lua";

// True when `extra` holds a live solver claiming the Lua runtime name.
// Null entries never satisfy it (they are rejected separately).
/**
 * @brief Check whether the extras span contains the Lua runtime solver.
 *
 * @param[in] extra caller-owned extra solvers (may contain nulls).
 * @return true when some non-null extra reports the kLuaSolverName
 *         identity; null entries never satisfy it.
 * @note Ownership: extras are borrowed — the caller keeps them alive for
 *       the whole run and steps them before the primary solver.
 */
[[nodiscard]] inline bool contains_lua_solver(std::span<core::ISolver *> extra) noexcept {
  for (core::ISolver *e : extra) {
    if (e != nullptr && e->name() == kLuaSolverName) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Move-only run handle: primary solver + initialized Runner.
 *
 * Built via build(); keeps its state and owned solvers across chunked
 * run(N) calls so callers can pace or batch driving (GUI frames,
 * wall-clock batching) without rebuilding per chunk. Sinks stay
 * caller-owned and are (re)attached by the caller after every rebuild.
 */
class ConfiguredRun {
 public:
  /** @brief Default constructor: an empty handle (no solver, no steps). */
  ConfiguredRun() = default;
  /** @brief Non-copyable (owns the solver and runner). */
  ConfiguredRun(const ConfiguredRun &) = delete;
  /** @brief Non-copyable (owns the solver and runner). */
  ConfiguredRun &operator=(const ConfiguredRun &) = delete;
  /** @brief Move constructor (defaulted): transfers the owned solver and runner; source left in a valid empty state. */
  ConfiguredRun(ConfiguredRun &&) noexcept = default;
  /** @brief Move assignment (defaulted): transfers ownership, frees the old run; source left in a valid empty state. @return *this */
  ConfiguredRun &operator=(ConfiguredRun &&) noexcept = default;

  // Build + validate + populate a run from `cfg`. Extras are borrowed
  // (caller keeps them alive, stepped BEFORE the primary in span order);
  // the primary solver is owned here. No sinks are attached and no steps
  // are taken — the caller attaches sinks then drives run(N) chunks.
  // `has_lua_runtime` declares that the caller provides a live Lua
  // scripting runtime (the CLI passes contains_lua_solver(extra), the GUI
  // passes false). On failure returns kInvalidArgument (or the spawn
  // Status) and, when `error` is non-null, explains why.
  /**
   * @brief Build, validate, and populate a run from cfg.
   *
   * Validates the loop (finite dt > 0, steps >= 1), resolves the solver
   * by name, scopes per-solver sections (gravity to projectile, signal
   * params to signal, sequential section to logic — misplaced sections
   * are rejected, never ignored), rejects batch kSum and Lua sources
   * without their explicit runtime, then spawns the population with
   * deterministic seeds (noise seed + index) and populates the
   * singleton sequential pool.
   *
   * @param[in] cfg parsed run configuration (loop, solver, sections).
   * @param[in] extra borrowed extra solvers, non-null, stepped before
   *            the primary in span order; the caller keeps them alive.
   * @param[in] has_lua_runtime true only when the caller provides a live
   *            Lua scripting runtime (CLI: contains_lua_solver(extra);
   *            GUI: false — wengine_core ships no scripting runtime).
   * @param[out] out receives the built run (unchanged on failure —
   *             rejections leave the live run untouched).
   * @param[out] error optional human-readable explanation for GUI
   *             display; pass nullptr for Status-only handling.
   * @return kOk on success; kInvalidArgument for every config problem
   *         above; a spawn Status unchanged when pool population fails
   *         (table/Lua payload append, source configure).
   * @note No sinks are attached and no steps are taken here — callers
   *       attach sinks, then drive run(N) chunks.
   */
  static core::Status build(const core::RunConfig &cfg, std::span<core::ISolver *> extra,
                            bool has_lua_runtime, ConfiguredRun &out,
                            std::string *error = nullptr) {
    auto fail = [&](std::string_view msg) {
      if (error != nullptr) {
        *error = std::string(msg);
      }
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    };
    if (!core::validate_runner_config(cfg.loop).ok()) {
      return fail("dt must be finite and > 0");
    }
    if (cfg.particles > 0 && (!std::isfinite(cfg.x0) || !std::isfinite(cfg.y0) || !std::isfinite(cfg.vx0) || !std::isfinite(cfg.vy0))) {
      return fail("initial conditions must be finite");
    }
    if (cfg.steps < 1) {
      return fail("steps must be >= 1");
    }
    const SolverDef *sdef = SolverRegistry::find(cfg.solver);
    if (sdef == nullptr) {
      if (error != nullptr) {
        *error = std::string("unknown solver '") + std::string(cfg.solver) + "'";
      }
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    for (core::ISolver *e : extra) {
      if (e == nullptr) {
        return fail("invalid extra solver (null)");
      }
    }

    std::unique_ptr<core::ISolver> solver = sdef->create();
    const bool is_projectile = solver->name() == ProjectileSolver::kName;
    if (is_projectile) {
      if (!std::isfinite(cfg.gravity)) {
        return fail("gravity must be finite");
      }
      static_cast<ProjectileSolver *>(solver.get())->set_gravity(cfg.gravity);
    } else if (cfg.has_gravity) {
      return fail("--gravity only applies to solver 'projectile'");
    }

    // Signal-scoped section: parsed values only, driver validates + spawns.
    // Batch spawns identical sources, so kSum is rejected (sums need
    // distinct per-source operands). kLua needs a script plus an explicit
    // Lua runtime (flag AND a "lua" extra solver); without the runtime the
    // sources would freeze silently at their spawn sample, so reject.
    const bool is_signal = solver->name() == SignalSolver::kName;
    if (is_signal) {
      const bool wants_lua = cfg.signal.params.kind == signal::SignalKind::kLua;
      const bool lua_ready =
          has_lua_runtime && contains_lua_solver(extra);
      if (!signal::validate_params(cfg.signal.params).ok() ||
          cfg.signal.params.kind == signal::SignalKind::kSum ||
          (wants_lua && !lua_ready)) {
        return fail("invalid signal params (freq > 0, duty in 0..1; no batch sums or lua)");
      }
      const bool is_expr = cfg.signal.params.kind == signal::SignalKind::kExpression;
      const bool is_table = cfg.signal.params.kind == signal::SignalKind::kTable;
      const bool is_lua = wants_lua; // validated ready above
      // Custom-source payloads must ride with their kind (expression
      // program in params, table points in cfg.signal.table, Lua script
      // in cfg.signal.lua); leftovers are rejected, never ignored.
      if (!is_expr && cfg.signal.params.expr.size != 0) {
        return fail("invalid signal params (custom-source payload misplaced)");
      }
      if ((is_expr || is_lua) && !cfg.signal.table.empty()) {
        return fail("invalid signal params (custom-source payload misplaced)");
      }
      if (!is_lua && !cfg.signal.lua.empty()) {
        return fail("invalid signal params (custom-source payload misplaced)");
      }
      if (is_table) {
        if (!signal::validate_table_points(std::span(cfg.signal.table.data(),
                                                     cfg.signal.table.size()))
                 .ok()) {
          return fail("invalid signal params (table needs CSV points)");
        }
      } else if (!cfg.signal.table.empty()) {
        return fail("invalid signal params (table points need kind=table)");
      }
      if (is_lua) {
        if (cfg.signal.lua.empty() ||
            cfg.signal.lua.size() > signal::kMaxLuaScriptBytes) {
          return fail("invalid signal params (lua needs script and runtime)");
        }
      }
    } else if (cfg.signal.set) {
      return fail("signal options only apply to solver 'signal'");
    }

    // Sequential-scoped section: parsed values only, driver validates +
    // populates the singleton State pool. The topology is required (no
    // defaults run): solver "logic" needs set, any other solver rejects
    // set. Stimulus rides in block-interface order (Low/High only);
    // members is the flattened edge-flop (DFF/JK/T) slot list — empty
    // iff step_all_domains (default all-domains path).
    const bool is_logic = solver->name() == SequentialSolver::kName;
    if (is_logic) {
      if (!cfg.seq.set) {
        return fail("logic run requires a sequential section (set)");
      }
      if (!logic::topo::validate(cfg.seq.netlist, logic::EdgeFilter::kCombOnly).ok()) {
        return fail("invalid sequential net (combinational cycle or bad topology)");
      }
      if (cfg.seq.stimulus.size() != logic::block_input_count(cfg.seq.netlist)) {
        return fail("invalid sequential stimulus (size must match block inputs)");
      }
      for (logic::LogicValue v : cfg.seq.stimulus) {
        if (v != logic::LogicValue::kLow && v != logic::LogicValue::kHigh) {
          return fail("invalid sequential stimulus (Low/High only)");
        }
      }
      if (cfg.seq.step_all_domains && !cfg.seq.members.empty()) {
        return fail("invalid sequential members (must be empty when stepping all domains)");
      }
      if (!cfg.seq.step_all_domains && cfg.seq.members.empty()) {
        return fail("invalid sequential members (empty selection steps nothing)");
      }
      for (std::size_t k = 0; k < cfg.seq.members.size(); ++k) {
        const std::uint16_t m = cfg.seq.members[k];
        if (m >= logic::kMaxGates || !cfg.seq.netlist.gates[m].live) {
          return fail("invalid sequential member (not a live slot)");
        }
        const logic::GateKind mk = cfg.seq.netlist.gates[m].kind;
        const bool edge_kind = mk == logic::GateKind::kDff || mk == logic::GateKind::kJk ||
                               mk == logic::GateKind::kT;
        if (!edge_kind) {
          return fail("invalid sequential member (not an edge flop)");
        }
        for (std::size_t j = 0; j < k; ++j) {
          if (cfg.seq.members[j] == m) {
            return fail("invalid sequential members (duplicate slot)");
          }
        }
      }
    } else if (cfg.seq.set) {
      return fail("logic options only apply to solver 'logic'");
    }

    ConfiguredRun run;
    run.solver_ = std::move(solver);
    run.runner_ = core::Runner(cfg.loop);
    for (core::ISolver *e : extra) {
      run.runner_.add_solver(*e);
    }
    run.runner_.add_solver(*run.solver_);
    for (std::uint64_t k = 0; k < cfg.particles; ++k) {
      world::Entity e = run.runner_.state().spawn_entity();
      run.runner_.state().particles.attach(e.index);
      run.runner_.state().particles.pos_x[e.index] = cfg.x0;
      run.runner_.state().particles.pos_y[e.index] = cfg.y0;
      run.runner_.state().particles.vel_x[e.index] = cfg.vx0;
      run.runner_.state().particles.vel_y[e.index] = cfg.vy0;
    }
    if (is_signal) {
      // Batch sources share params; per-source seeds derive deterministically
      // (seed + index) so N noise sources never alias. Table/Lua batches
      // share one appended range; Lua spawn samples are 0 until the extra
      // solver's first evaluation.
      const bool is_table = cfg.signal.params.kind == signal::SignalKind::kTable;
      const bool is_lua = cfg.signal.params.kind == signal::SignalKind::kLua;
      std::uint64_t table_off = 0;
      std::uint64_t table_len = 0;
      std::uint64_t lua_off = 0;
      std::uint64_t lua_len = 0;
      if (is_table) {
        if (!world::append_table_points(
                 run.runner_.state().signals,
                 std::span(cfg.signal.table.data(), cfg.signal.table.size()), table_off)
                 .ok()) {
          return fail("signal setup failed");
        }
        table_len = static_cast<std::uint64_t>(cfg.signal.table.size());
      }
      if (is_lua) {
        const std::string_view script(cfg.signal.lua.data(), cfg.signal.lua.size());
        if (!world::append_lua_script(run.runner_.state().signals, script, lua_off).ok()) {
          return fail("signal setup failed");
        }
        lua_len = static_cast<std::uint64_t>(script.size());
      }
      for (std::uint64_t k = 0; k < cfg.signal.count; ++k) {
        world::Entity e = run.runner_.state().spawn_entity();
        if (is_table) {
          if (!world::attach_table_source(e, run.runner_.state().signals, table_off, table_len,
                                          cfg.signal.params.quantity, 0.0)
                   .ok()) {
            return fail("signal setup failed");
          }
          continue;
        }
        if (is_lua) {
          if (!world::attach_lua_source(e, run.runner_.state().signals, lua_off, lua_len,
                                        cfg.signal.params.quantity, 0.0)
                   .ok()) {
            return fail("signal setup failed");
          }
          continue;
        }
        signal::SignalParams p = cfg.signal.params;
        p.seed = cfg.signal.params.seed + k;
        if (!world::configure_source(e, run.runner_.state().signals, p, 0.0).ok()) {
          return fail("signal setup failed");
        }
      }
    }
    if (is_logic) {
      // Populate the singleton sequential pool from the validated
      // section: topology copy (immutable during the run), clocked-state
      // sync (fresh serials, power-on Low), retained stimulus + member
      // list, and settled outputs sized to the block interface (Low
      // until the first step rewrites them).
      world::State &st = run.runner_.state();
      st.netlist = cfg.seq.netlist;
      logic::init_seq_state(st.seq, st.netlist);
      // Workspace power-on values (006): seed every live sequential
      // slot from the section, pre-first-step. All-Low default keeps
      // direct-constructed configs on the legacy fresh-attach behavior.
      for (std::uint16_t s = 0; s < logic::kMaxGates; ++s) {
        if (st.seq.valid[s] != 0 && logic::is_sequential_kind(st.netlist.gates[s].kind)) {
          st.seq.q[s] = cfg.seq.init_q[s];
        }
      }
      st.seq_inputs = cfg.seq.stimulus;
      st.seq_members = cfg.seq.members;
      st.seq_outputs.assign(logic::block_output_count(st.netlist), logic::LogicValue::kLow);
    }
    out = std::move(run);
    return core::Status::success();
  }

  // Chunked driving over the same state and owned solvers (multiple
  // run(N) calls accumulate; interruption stops at step boundaries).
  /**
   * @brief Advance the run by a chunk of fixed steps.
   *
   * @param[in] steps number of fixed-dt steps for this chunk (multiple
   *            calls accumulate over the same state and solvers).
   * @param[in] stop_requested optional poll checked between chunks;
   *            null drives all steps uninterrupted. A stop ends the
   *            chunked drive with kOk (partial prefix, like
   *            Runner::run_until).
   * @return kOk on success or interruption; the solver/runner Status
   *         unchanged when a step fails (loud, deterministic).
   */
  core::Status run(std::uint64_t steps,
                   bool (*stop_requested)() noexcept = nullptr) {
    return runner_.run_until(steps, stop_requested);
  }

  /**
   * @brief Attach one non-owning sink observer for future steps.
   * @param[in] sink observer the caller keeps alive for the run; the
   *            runner never owns sinks and re-attach is the caller's
   *            job after any rebuild.
   */
  void add_sink(emit::ISink &sink) { runner_.add_sink(sink); }

  /** @brief Mutable access to the owned runner (rebuilds/config edges). @return Reference to the runner. */
  [[nodiscard]] core::Runner &runner() noexcept { return runner_; }
  /** @brief Read-only access to the owned runner. @return Const reference to the runner. */
  [[nodiscard]] const core::Runner &runner() const noexcept { return runner_; }
  /** @brief Mutable access to the live world state (spawn/attach edges). @return Reference to the state. */
  [[nodiscard]] world::State &state() noexcept { return runner_.state(); }
  /** @brief Read-only access to the live world state. @return Const reference to the state. */
  [[nodiscard]] const world::State &state() const noexcept { return runner_.state(); }

 private:
  // Declared before runner_: Runner only borrows the solver pointer.
  std::unique_ptr<core::ISolver> solver_;
  core::Runner runner_;
};

} // namespace wengine::solvers
