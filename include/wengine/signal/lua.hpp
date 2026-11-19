#pragma once

/**
 * @file
 * Lua scripting source (SIG-4b): advanced custom waveforms behind an
 * isolated static TU. The only Lua-aware engine header, including no Lua
 * headers itself. Depends on core/solver, core/status, and world leaf
 * types; the interpreter (vendored PUC Lua) stays hidden in the TU.
 * Consumed by the app edge, which registers LuaSignalSolver before
 * SignalSolver so sums over Lua operands read fresh values.
 */
// Lua scripting source (SIG-4b): advanced/custom waveforms without growing
// the expression language. This is the ONLY Lua-aware engine header, and it
// includes NO Lua headers: the interpreter lives behind an isolated STATIC
// TU (`wengine_signal_lua`, vendored PUC Lua 5.4.9), so hot engine headers
// and every existing target stay Lua-free.
//
// Model: a script is a Lua chunk evaluated once per step per live source,
// as a PURE function of its inputs — `t` (source seconds since start, same
// time base as every other generator), `seed` (spawn seed, exposed per the
// SIG-4 contract), `step` (1-based number of the step being computed,
// matching emitted snapshots), `dt` (fixed step). The chunk
// must return a number; anything else (runtime error, wrong type,
// non-finite result, instruction-budget overrun) fails the step loudly
// (`kInvalidArgument`, time/index frozen), exactly like a failing
// expression. Purity is structural: each call runs with a FRESH `_ENV`
// (sandbox + inputs), so scripts cannot keep state across steps and
// save/load/continue is exact by construction. There is deliberately no
// RNG in 4b (scripts needing noise sum a noise source); `seed` is data.
//
// Sandbox (locked, see docs/signals.md sec. 7): `math` (minus
// `random`/`randomseed` — C `rand` differs across libcs) and `string`
// (minus `dump`) plus a small pure base (`assert`, `error`, `ipairs`,
// `next`, `pairs`, `pcall`, `select`, `tonumber`, `type`, `_VERSION`).
// Everything else is absent: no `io`/`os`/`package`/`debug`/`coroutine`/
// `utf8`, no `load`/`dofile`/`print`/`warn`/`collectgarbage`, no
// metatables, and no `tostring`/`string.format` (address-exposure channels
// that would break the determinism contract). Libraries are read-only.
// Chunks load in text mode only (`"t"` — bytecode rejected). Budgets are
// instruction counts, never wall-clock.
//
// Iteration policy (determinism contract): stock pairs/next visit keys in
// per-state hash order (Lua randomizes string hashing per lua_State), so
// the sandbox replaces both globals with implementations over one total
// key order — booleans (false < true), numbers (ascending), strings (byte
// order); `ipairs` keeps stock array-part order (already deterministic).
// Only scalar keys iterate: identity-based keys (table/function/userdata/
// thread) and NaN fail the step loudly. `pairs` snapshots the sorted keys
// once (added keys unseen, removed keys read nil); `next` re-sorts the
// live keys per call. Snapshot work is C-side O(n log n), outside the
// instruction budget.
//
// Wiring: `LuaSignalSolver` is an EXTRA solver (see solvers/run.hpp): the
// app registers it BEFORE `SignalSolver`, which skips `kLua` slots while
// this solver evaluates them in slot order first — so sums over Lua
// operands still read fresh values. Scripts persist as pool text (v5);
// the solver caches compiled chunks by source serial and recompiles when
// the text differs, so a fresh solver over a loaded state just works.
// Single-threaded only (like the Runner); solvers are non-copyable.

#include "wengine/core/solver.hpp"
#include "wengine/core/status.hpp"
#include "wengine/world/entity.hpp"
#include "wengine/world/signals.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace wengine::lua {

// Pinned language: PUC Lua 5.4 (see the `release()` TU check). The pin is
// the `external/lua` submodule commit; the test asserts it.
/**
 * @brief Pinned language version (PUC Lua 5.4).
 * @return 504; a submodule bump fails goldens loudly until re-verified.
 */
[[nodiscard]] inline constexpr int language_version() noexcept { return 504; }

// Pinned interpreter release string (`LUA_RELEASE`, e.g. "Lua 5.4.9"): the
// test asserts it, so a submodule bump fails loudly until goldens are
// re-verified (dependency policy). Compiler-enforced in the TU too.
/**
 * @brief Pinned interpreter release string (LUA_RELEASE, e.g. "Lua 5.4.9").
 * @return Release text; the test asserts it so dependency drift fails loudly.
 */
[[nodiscard]] std::string_view release() noexcept;

/**
 * @brief Budget configuration for the Lua extra solver.
 * @note Single-threaded like the Runner; solvers are non-copyable.
 */
struct LuaConfig {
/** @brief Instruction budget per source per step; 0 rejects. Wall-clock is never a budget. */
  std::uint32_t max_instructions = 65536; // per source per step; 0 = reject
};

/**
 * @brief Extra solver evaluating kLua sources in slot order before SignalSolver.
 * @note Each chunk runs as a PURE function of t/seed/step/dt with a FRESH _ENV,
 * so scripts keep no state across steps and save/load/continue is exact.
 * Scripts persist as pool text; chunks cache by source serial and recompile
 * when the text differs. There is deliberately no RNG in 4b (scripts needing
 * noise sum a noise source); seed is data.
 */
class LuaSignalSolver : public core::ISolver {
public:
/** @brief Solver registry name. */
  static constexpr std::string_view kName = "lua";

/**
 * @brief Construct the solver with an instruction budget.
 * @param[in] cfg budget configuration (see LuaConfig).
 */
  explicit LuaSignalSolver(LuaConfig cfg = {});
/** @brief Destructor (owns the isolated interpreter state). */
  ~LuaSignalSolver() override;
/** @brief Non-copyable (single-threaded interpreter ownership). */
  LuaSignalSolver(const LuaSignalSolver &) = delete;
/** @brief Non-copyable (see constructor). */
  LuaSignalSolver &operator=(const LuaSignalSolver &) = delete;
/** @brief Non-movable (interpreter state is pinned to the instance). */
  LuaSignalSolver(LuaSignalSolver &&) = delete;
/** @brief Non-movable (see move constructor). */
  LuaSignalSolver &operator=(LuaSignalSolver &&) = delete;

/**
 * @brief Solver registry name.
 * @return kName ("lua").
 */
  [[nodiscard]] std::string_view name() const noexcept override { return kName; }

/**
 * @brief Evaluate every live kLua slot in slot order for one fixed step.
 * @param[in,out] s live simulation state (kLua slots evaluated first, so sums read fresh values).
 * @param[in] dt fixed step in seconds.
 * @return kOk, or kInvalidArgument when a script fails loudly (runtime error, wrong
 * type, non-finite result, instruction-budget overrun; time/index frozen like a
 * failing expression).
 */
  core::Status step(world::State &s, double dt) noexcept override;

  // Last script failure, human-readable (for CLI/GUI display after a failed
  // step; empty when the last step succeeded). Frontend-owned inquiry —
  // never consulted on the hot path.
/**
 * @brief Last script failure, human-readable, for CLI/GUI display after a failed step.
 * @return Failure text; empty when the last step succeeded.
 * @note Frontend-owned inquiry, never consulted on the hot path.
 */
  [[nodiscard]] const std::string &last_error() const noexcept;
/** @brief Successful evaluations, ever. @return Count of successful script evaluations. */
  [[nodiscard]] std::uint64_t evals() const noexcept; // successful evals, ever
/** @brief Failed evaluations, ever. @return Count of failed script evaluations. */
  [[nodiscard]] std::uint64_t failures() const noexcept; // failed evals, ever

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] inline std::unique_ptr<LuaSignalSolver>
/**
 * @brief Factory for the Lua extra solver.
 * @param[in] cfg budget configuration (see LuaConfig).
 * @return Owning pointer to a LuaSignalSolver.
 */
create_lua_solver(LuaConfig cfg = {}) {
  return std::make_unique<LuaSignalSolver>(cfg);
}

// Compile-only check (config edge): syntax + text-mode load, no
// evaluation. The CLI maps failure to a usage error; direct API users get
// the same gate at configure_lua_source.
/**
 * @brief Compile-only gate (config edge): syntax + text-mode load, no evaluation.
 * @param[in] text script source to check.
 * @return kOk, or kInvalidArgument for syntax/load failure; the CLI maps failure to a usage error.
 */
[[nodiscard]] core::Status check_script(std::string_view text);

// Configure entity `e` as a Lua source: syntax-checked, appended to the
// pool script store, attached with spawn sample 0 (Lua sources sample on
// their first stepped evaluation). On failure nothing is attached and the
// pool is untouched.
/**
 * @brief Configure an entity as a Lua source: syntax-checked, appended to the pool script store.
 * @param[in] e entity to attach (spawn sample is 0: Lua sources sample on their first stepped evaluation).
 * @param[in,out] pool signal pool receiving the script text and attachment.
 * @param[in] script script text (bounded by kMaxLuaScriptBytes).
 * @param[in] quantity stamped quantity of the samples.
 * @param[in] t_now current simulation time in seconds.
 * @return kOk, or kInvalidArgument on failure (nothing is attached and the pool is untouched).
 */
[[nodiscard]] core::Status configure_lua_source(world::Entity e, world::Signals &pool,
                                               std::string_view script,
                                               signal::QuantityId quantity, double t_now);

} // namespace wengine::lua
