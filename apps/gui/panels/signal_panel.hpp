#pragma once

// Dear ImGui signal-source panel: batch signal configuration (kind,
// stamped params, run length) with an explicit Apply step, driving the
// shared SimController through the engine-only SignalModel.
//
// One batch source only (per-source list/add/drop, Lua, and expressions
// need SIG-6/SIG-10 and are out of scope). Transport (Play/Pause/Step)
// stays in SimPanel; both panels drive the same controller without
// fighting: SimPanel rebuilds only on its own fields' diff, and this
// panel applies only on button press (no per-keystroke rebuilds).
//
// ID scope: this panel shares the "WEngine" window with SimPanel, so every
// widget label carries the ##signal suffix (duplicate labels across panels
// would otherwise collide and misbehave).

#include "imgui.h"
#include "controllers/sim_controller.hpp"
#include "models/signal_model.hpp"
#include "wengine/signal/types.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace wengine::gui {

namespace detail {

[[nodiscard]] inline const char *signal_kind_getter(void *data, int idx) {
  (void)data;
  if (idx < 0 ||
      static_cast<std::size_t>(idx) >= signal::kKindNames.size()) {
    return "";
  }
  // Kind names are static string literals: NUL-terminated and stable.
  return signal::kKindNames[static_cast<std::size_t>(idx)].data();
}

} // namespace detail

struct SignalPanel {
  static constexpr std::size_t kFieldSize = 32;
  char freq[kFieldSize] = "1";
  char amp[kFieldSize] = "1";
  char offset[kFieldSize] = "0";
  char phase[kFieldSize] = "0";
  char duty[kFieldSize] = "0.5";
  char seed[kFieldSize] = "1";
  char count[kFieldSize] = "1";
  char dt[kFieldSize] = "0.01";
  char steps[kFieldSize] = "100";
  char emit_every[kFieldSize] = "1";
  int kind_idx = 0; // index into signal::kKindNames (sine default)
  int qty_idx = 0; // 0 = U, 1 = I
  std::string error; // last Apply rejection (frontend-owned, may allocate)

  void sync_from(const core::RunConfig &cfg) {
    if (cfg.solver != solvers::SignalSolver::kName) {
      return; // leave fields alone when another solver drives the run
    }
    const signal::SignalParams &p = cfg.signal.params;
    for (std::size_t k = 0; k < signal::kKindNames.size(); ++k) {
      if (static_cast<signal::SignalKind>(k) == p.kind) {
        kind_idx = static_cast<int>(k);
      }
    }
    qty_idx = (p.quantity == signal::QuantityId::kCurrent) ? 1 : 0;
    std::snprintf(freq, kFieldSize, "%g", p.freq_hz);
    std::snprintf(amp, kFieldSize, "%g", p.amp);
    std::snprintf(offset, kFieldSize, "%g", p.offset);
    std::snprintf(phase, kFieldSize, "%g", p.phase_rad);
    std::snprintf(duty, kFieldSize, "%g", p.duty);
    std::snprintf(seed, kFieldSize, "%llu", static_cast<unsigned long long>(p.seed));
    std::snprintf(count, kFieldSize, "%llu",
                  static_cast<unsigned long long>(cfg.signal.count));
    std::snprintf(dt, kFieldSize, "%g", cfg.loop.dt);
    std::snprintf(steps, kFieldSize, "%llu", static_cast<unsigned long long>(cfg.steps));
    std::snprintf(emit_every, kFieldSize, "%llu",
                  static_cast<unsigned long long>(cfg.loop.emit_every));
  }

  void draw(SimController &sim) {
    ImGui::Separator();
    ImGui::Text("Signals (batch source)");
    ImGui::Combo("kind##signal", &kind_idx, &detail::signal_kind_getter, nullptr,
                 static_cast<int>(signal::kKindNames.size()));
    const char *qty_items[2] = {"U", "I"};
    ImGui::Combo("quantity##signal", &qty_idx, qty_items, 2);
    ImGui::InputText("freq (Hz)##signal", freq, kFieldSize);
    ImGui::InputText("amp##signal", amp, kFieldSize);
    ImGui::InputText("offset##signal", offset, kFieldSize);
    ImGui::InputText("phase (rad)##signal", phase, kFieldSize);
    ImGui::InputText("duty##signal", duty, kFieldSize);
    ImGui::InputText("seed##signal", seed, kFieldSize);
    ImGui::InputText("count##signal", count, kFieldSize);
    ImGui::InputText("dt (s)##signal", dt, kFieldSize);
    ImGui::InputText("steps##signal", steps, kFieldSize);
    ImGui::InputText("emit every##signal", emit_every, kFieldSize);
    ImGui::TextDisabled("Apply rebuilds the run from step 0.");
    if (ImGui::Button("Apply##signal")) {
      SignalModel model;
      model.kind = signal::kKindNames[static_cast<std::size_t>(kind_idx)];
      model.quantity = qty_idx == 1 ? "I" : "U";
      model.freq = freq;
      model.amp = amp;
      model.offset = offset;
      model.phase = phase;
      model.duty = duty;
      model.seed = seed;
      model.count = count;
      model.dt = dt;
      model.steps = steps;
      model.emit_every = emit_every;
      const SignalBuild built = model.build();
      if (built.ok) {
        error.clear();
        sim.set_config(built.cfg);
      } else {
        error = built.error;
      }
    }
    if (!error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      ImGui::TextWrapped("Error: %s", error.c_str());
      ImGui::PopStyleColor();
    }
  }
};

} // namespace wengine::gui
