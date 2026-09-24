#pragma once

// Dear ImGui stepped-sim control panel: full `sim run` parity (solver
// select from the compile-time registry, dt/steps/particles/gravity/initial
// conditions, emit stride) plus Play/Pause/Single-step/Reset transport.
// Every accepted field edit rebuilds the controller's RunConfig live from
// step 0 — the only deterministic semantic, since a new dt or solver
// invalidates the old trajectory. Rejected edits keep the live run and show
// the reason inline. New solvers appear in the combo by registry entry only.

#include "imgui.h"
#include "controllers/sim_controller.hpp"
#include "views/frame_source.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <cstddef>

namespace wengine::gui {

namespace detail {

[[nodiscard]] inline const char *solver_combo_getter(void *data, int idx) {
  (void)data;
  const std::span<const solvers::SolverDef> all = solvers::SolverRegistry::all();
  if (idx < 0 || static_cast<std::size_t>(idx) >= all.size()) {
    return "";
  }
  // Registry names are static string literals: NUL-terminated and stable.
  return all[static_cast<std::size_t>(idx)].name.data();
}

} // namespace detail

struct SimPanel {
  double dt = 0.01;
  int steps = 100;
  int particles = 1;
  int emit_every = 1;
  double gravity = 9.81;
  double x0 = 0.0;
  double y0 = 0.0;
  double vx0 = 10.0;
  double vy0 = 10.0;
  int solver_idx = 0;

  void sync_from(const core::RunConfig &cfg) {
    const std::span<const solvers::SolverDef> all =
        solvers::SolverRegistry::all();
    for (std::size_t i = 0; i < all.size(); ++i) {
      if (all[i].name == cfg.solver) {
        solver_idx = static_cast<int>(i);
      }
    }
    dt = cfg.loop.dt;
    steps = static_cast<int>(cfg.steps);
    particles = static_cast<int>(cfg.particles);
    emit_every = static_cast<int>(cfg.loop.emit_every);
    gravity = cfg.gravity;
    x0 = cfg.x0;
    y0 = cfg.y0;
    vx0 = cfg.vx0;
    vy0 = cfg.vy0;
  }

  void draw(SimController &sim) {
    const std::span<const solvers::SolverDef> all =
        solvers::SolverRegistry::all();
    const int solver_count = static_cast<int>(all.size());

    ImGui::Separator();
    ImGui::Text("Simulation (stepped solvers)");
    if (solver_count > 0) {
      if (solver_idx < 0 || solver_idx >= solver_count) {
        solver_idx = 0;
      }
      ImGui::Combo("solver", &solver_idx, &detail::solver_combo_getter,
                   nullptr, solver_count);
    }
    ImGui::InputDouble("dt (s)", &dt, 0.001, 0.01, "%.6f");
    ImGui::InputInt("steps (budget)", &steps);
    ImGui::InputInt("particles", &particles);
    ImGui::InputInt("emit every", &emit_every);
    ImGui::InputDouble("gravity", &gravity, 0.1, 1.0, "%.4f");
    ImGui::InputDouble("x0", &x0, 0.5, 1.0, "%.4f");
    ImGui::InputDouble("y0", &y0, 0.5, 1.0, "%.4f");
    ImGui::InputDouble("vx0", &vx0, 0.5, 1.0, "%.4f");
    ImGui::InputDouble("vy0", &vy0, 0.5, 1.0, "%.4f");
    ImGui::TextDisabled("Editing parameters rebuilds the run from step 0.");

    core::RunConfig cfg = sim.config();
    cfg.loop.dt = dt;
    cfg.loop.emit_every = emit_every < 0 ? 0U : static_cast<std::uint64_t>(emit_every);
    cfg.steps = steps < 0 ? 0U : static_cast<std::uint64_t>(steps);
    cfg.particles = particles < 0 ? 0U : static_cast<std::uint64_t>(particles);
    cfg.gravity = gravity;
    cfg.x0 = x0;
    cfg.y0 = y0;
    cfg.vx0 = vx0;
    cfg.vy0 = vy0;
    if (!all.empty()) {
      cfg.solver = all[static_cast<std::size_t>(solver_idx)].name;
    }
    // Gravity is projectile-scoped (mirrors the CLI/engine contract): only
    // mark it explicit for that solver so future solvers stay selectable.
    cfg.has_gravity = (cfg.solver == solvers::ProjectileSolver::kName);
    if (cfg.loop.dt != sim.config().loop.dt ||
        cfg.loop.emit_every != sim.config().loop.emit_every ||
        cfg.steps != sim.config().steps ||
        cfg.particles != sim.config().particles ||
        cfg.gravity != sim.config().gravity ||
        cfg.x0 != sim.config().x0 || cfg.y0 != sim.config().y0 ||
        cfg.vx0 != sim.config().vx0 || cfg.vy0 != sim.config().vy0 ||
        cfg.solver != sim.config().solver) {
      sim.set_config(cfg);
    }

    if (sim.playing()) {
      if (ImGui::Button("Pause")) {
        sim.set_playing(false);
      }
    } else {
      if (ImGui::Button("Play")) {
        sim.set_playing(true);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
      sim.run_exact(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      sim.reset();
      sync_from(sim.config());
    }
  }

  // Status renders from the adapted frame (solver-agnostic): step, time,
  // item count, and the primary item's kinematics when present. Item-less
  // domains (signals) show their first channels instead.
  void draw_status(SimController &sim, const link::Frame &frame) {
    ImGui::Text("step=%llu t=%.4fs solver=%.*s%s",
                static_cast<unsigned long long>(frame.step), frame.time,
                static_cast<int>(sim.config().solver.size()),
                sim.config().solver.data(), sim.playing() ? " (playing)" : "");
    if (frame.items.empty()) {
      std::size_t shown = 0;
      for (const link::FrameChannel &c : frame.channels) {
        if (c.name == "sig.count" || c.name == "count") {
          continue; // metadata rows, not data (preserves the empty-run text)
        }
        ImGui::Text("%s = %.5f", c.name.c_str(), c.value);
        if (++shown >= 4) {
          break;
        }
      }
      if (shown == 0) {
        ImGui::Text("primary: (no items)");
      }
    } else {
      const link::FrameItem &p = frame.items.front();
      ImGui::Text("primary: x=%.5f y=%.5f z=%.5f", p.x, p.y, p.z);
      if (p.has_velocity) {
        ImGui::Text("         v=(%.3f, %.3f, %.3f)", p.vx, p.vy, p.vz);
      }
    }
    if (!sim.ok()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      ImGui::TextWrapped("Error: %s", sim.error().c_str());
      ImGui::PopStyleColor();
    }
  }
};

} // namespace wengine::gui
