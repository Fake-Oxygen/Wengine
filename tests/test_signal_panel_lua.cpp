// GUI SimController scripting-runtime wiring (task 62 via SIG-9): optional
// caller-owned extras + has_lua_runtime flag threaded into the shared
// builder, Lua batch bit-parity with headless, loud script failures with
// inquiryable text, and rejection matrix (dummy/non-Lua extra, missing
// flag, null entries). Links wengine_signal_lua; the wengine_core-only
// test_signal_panel binary stays runtime-free.

#include "controllers/sim_controller.hpp"
#include "models/signal_model.hpp"
#include "views/frame_source.hpp"
#include "wengine/signal/lua.hpp"
#include "wengine/solvers/run.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>

using namespace wengine;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

core::RunConfig lua_batch_cfg(const std::string &script) {
  gui::SignalModel m;
  m.kind = "lua";
  m.lua_script = script;
  m.dt = "0.1";
  m.steps = "5";
  const gui::SignalBuild b = m.build();
  assert(b.ok);
  return b.cfg;
}

void test_lua_rejects_without_runtime() {
  gui::SimController sim;
  const core::RunConfig cfg = lua_batch_cfg("return t * 2");
  assert(!sim.set_config(cfg).ok());
  assert(!sim.error().empty());
  assert(sim.state().step_index == 0); // live run untouched
}

void test_lua_batch_parity() {
  const core::RunConfig cfg = lua_batch_cfg("return t * 2");
  auto lua = lua::create_lua_solver();
  std::array<core::ISolver *, 1> extra{lua.get()};

  gui::SimController sim;
  sim.set_script_error(&lua->last_error());
  assert(sim
             .set_config(cfg,
                         std::span<core::ISolver *>(extra.data(), extra.size()),
                         /*has_lua_runtime=*/true)
             .ok());
  assert(sim.has_lua_runtime());
  assert(sim.extras().size() == 1 && sim.extras()[0] == lua.get());
  assert(sim.run_exact(cfg.steps).ok());
  assert(sim.script_error().empty());
  // t = 0.5 s at step 5 (end-of-step stamping): 2t = 1.
  assert(near(sim.snapshot().sig_value[0], 1.0, 1e-9));

  world::State ref;
  assert(solvers::run_headless(
             cfg, std::span<emit::ISink *>{}, ref,
             std::span<core::ISolver *>(extra.data(), extra.size()))
             .ok());
  assert(sim.state() == ref);
  assert(sim.snapshot() == world::take_snapshot(ref));

  // reset() reuses the stored runtime: Reset + full run matches headless.
  sim.reset();
  assert(sim.run_exact(cfg.steps).ok());
  assert(sim.state() == ref);

  // Panels forward extras()/has_lua_runtime() on every Apply (a span over
  // the controller's own storage — the copy must survive the aliasing):
  // a batch tweak preserves the runtime and stays bit-identical.
  core::RunConfig tweaked = cfg;
  tweaked.steps = 3;
  assert(sim
             .set_config(tweaked, sim.extras(), sim.has_lua_runtime())
             .ok());
  assert(sim.has_lua_runtime() && sim.extras().size() == 1);
  assert(sim.run_exact(tweaked.steps).ok());
  world::State ref_short;
  assert(solvers::run_headless(
             tweaked, std::span<emit::ISink *>{}, ref_short,
             std::span<core::ISolver *>(extra.data(), extra.size()))
             .ok());
  assert(sim.state() == ref_short);
}

void test_lua_failure_surfaces_text() {
  const core::RunConfig cfg = lua_batch_cfg("return nil");
  auto lua = lua::create_lua_solver();
  std::array<core::ISolver *, 1> extra{lua.get()};

  gui::SimController sim;
  sim.set_script_error(&lua->last_error());
  assert(sim
             .set_config(cfg,
                         std::span<core::ISolver *>(extra.data(), extra.size()),
                         /*has_lua_runtime=*/true)
             .ok());
  assert(!sim.run_exact(1).ok());
  assert(!sim.error().empty());
  assert(!sim.script_error().empty()); // inline panel line source
  // Clearing the borrow reads empty (lifetime inquiry, never hot path).
  sim.set_script_error(nullptr);
  assert(sim.script_error().empty());
}

void test_lua_rejection_matrix() {
  const core::RunConfig cfg = lua_batch_cfg("return 1");
  auto lua = lua::create_lua_solver();
  solvers::SignalSolver dummy; // non-Lua extra: must not satisfy kLua
  gui::SimController sim;

  std::array<core::ISolver *, 1> lua_extra{lua.get()};
  std::array<core::ISolver *, 1> dummy_extra{&dummy};
  std::array<core::ISolver *, 1> null_extra{nullptr};

  // Lua extra without the flag rejects.
  assert(!sim
              .set_config(cfg,
                          std::span<core::ISolver *>(lua_extra.data(),
                                                     lua_extra.size()),
                          /*has_lua_runtime=*/false)
              .ok());
  // Dummy/non-Lua extra with the flag rejects.
  assert(!sim
              .set_config(cfg,
                          std::span<core::ISolver *>(dummy_extra.data(),
                                                     dummy_extra.size()),
                          /*has_lua_runtime=*/true)
              .ok());
  // Null entries reject.
  assert(!sim
              .set_config(cfg,
                          std::span<core::ISolver *>(null_extra.data(),
                                                     null_extra.size()),
                          /*has_lua_runtime=*/true)
              .ok());
  assert(!sim.error().empty());
  assert(sim.state().step_index == 0); // every rejection keeps the live run
}

void test_reset_reattaches_sinks_with_runtime() {
  // Rebuild reuses the caller-owned extras and reattaches caller-owned
  // sinks: a recorder attached once keeps receiving signal frames across
  // reset() with the Lua runtime active (caller keeps ownership of both).
  const core::RunConfig cfg = lua_batch_cfg("return t * 2");
  auto lua = lua::create_lua_solver();
  std::array<core::ISolver *, 1> extra{lua.get()};

  gui::SimController sim;
  assert(sim
             .set_config(cfg,
                         std::span<core::ISolver *>(extra.data(), extra.size()),
                         /*has_lua_runtime=*/true)
             .ok());
  gui::FrameHistory history;
  gui::FrameRecorder recorder(history);
  recorder.set_solver("signal");
  sim.add_sink(recorder);
  assert(sim.run_exact(cfg.steps).ok());
  assert(history.size() == cfg.steps); // emit_every defaults to 1
  assert(history.frames().back().channels[1].name == "sig.0.U");

  sim.reset(); // rebuild: extras re-used, observers re-attached
  assert(sim.has_lua_runtime());
  assert(sim.run_exact(2).ok());
  // Fresh run regresses steps (fresh series after the step drop)...
  assert(history.size() == 2);
  // ...and the frames are live Lua evaluations, not stale copies
  // (step 2 at dt=0.1 stamps value(0.2) = 2t = 0.4).
  assert(near(history.frames().back().channels[1].value, 0.4, 1e-9));
  assert(history.frames().back() ==
         link::frame_from_signal_snapshot(sim.snapshot()));
}

} // namespace

int main() {
  test_lua_rejects_without_runtime();
  test_lua_batch_parity();
  test_lua_failure_surfaces_text();
  test_lua_rejection_matrix();
  test_reset_reattaches_sinks_with_runtime();
  std::printf("test_signal_panel_lua: OK\n");
  return 0;
}
