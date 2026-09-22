#pragma once

// Realtime graphs panel: custom ImDrawList time-series plots fed from the
// snapshot stream (height-vs-time, trajectory), with axis auto-fit.
//
// Design notes (decisions recorded for the task):
// - Custom renderer, not ImPlot: avoids a third third-party dependency
//   (project policy) and matches the proven 3D-viewport approach. Revisit
//   if plot needs ever exceed polylines (log axes, histograms, 100k+
//   points per frame).
// - Feed path: a PlotHistory sink attached to the SimController. Sinks
//   fire only on emitted steps, so the series is decimated to the run's
//   emit rate by construction, and pause-freeze is automatic (a paused
//   sim emits nothing, the plot simply stops growing).
// - The panel never touches the sim: const-snapshot observer only.

#include "imgui.h"
#include "controllers/sim_controller.hpp"
#include "models/plot_history.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace wengine::gui {

namespace graphs_detail {

struct Bounds {
  double x0 = 0.0;
  double x1 = 1.0;
  double y0 = 0.0;
  double y1 = 1.0;
};

[[nodiscard]] inline Bounds fit_bounds(const std::vector<std::vector<ImVec2>> &series) {
  Bounds b;
  bool first = true;
  for (const auto &line : series) {
    for (const ImVec2 p : line) {
      const double x = static_cast<double>(p.x);
      const double y = static_cast<double>(p.y);
      if (first) {
        b.x0 = b.x1 = x;
        b.y0 = b.y1 = y;
        first = false;
      } else {
        if (x < b.x0) b.x0 = x;
        if (x > b.x1) b.x1 = x;
        if (y < b.y0) b.y0 = y;
        if (y > b.y1) b.y1 = y;
      }
    }
  }
  if (first) {
    return b;
  }
  const double padx = (b.x1 - b.x0) * 0.05;
  const double pady = (b.y1 - b.y0) * 0.05;
  b.x0 -= padx > 0.0 ? padx : 1.0;
  b.x1 += padx > 0.0 ? padx : 1.0;
  b.y0 -= pady > 0.0 ? pady : 1.0;
  b.y1 += pady > 0.0 ? pady : 1.0;
  return b;
}

inline void draw_plot(ImDrawList *dl, const char *title, const char *x_label,
                      const std::vector<std::vector<ImVec2>> &series, float h) {
  const float w = ImGui::GetContentRegionAvail().x;
  if (w < 60.0F || h < 60.0F) {
    ImGui::TextDisabled("%s (too small)", title);
    return;
  }
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(title, ImVec2(w, h));
  const Bounds b = fit_bounds(series);
  constexpr float kLeft = 46.0F;
  constexpr float kBottom = 16.0F;
  constexpr float kTop = 16.0F;
  const float pw = w - kLeft - 6.0F;
  const float ph = h - kTop - kBottom;
  auto map = [&](double x, double y) {
    const double fx = (x - b.x0) / (b.x1 - b.x0);
    const double fy = (y - b.y0) / (b.y1 - b.y0);
    return ImVec2(origin.x + kLeft + static_cast<float>(fx * pw),
                  origin.y + kTop + static_cast<float>((1.0 - fy) * ph));
  };
  dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + h),
                    IM_COL32(8, 10, 14, 255));
  char label[32];
  for (int g = 0; g <= 4; ++g) {
    const double f = static_cast<double>(g) / 4.0;
    const ImVec2 hp0 = map(b.x0, b.y0 + f * (b.y1 - b.y0));
    const ImVec2 hp1 = map(b.x1, b.y0 + f * (b.y1 - b.y0));
    dl->AddLine(hp0, hp1, IM_COL32(35, 42, 55, 255));
    std::snprintf(label, sizeof(label), "%.3g", b.y0 + f * (b.y1 - b.y0));
    dl->AddText(ImVec2(origin.x + 2.0F, hp0.y - 7.0F), IM_COL32(140, 150, 165, 255),
                label);
    const ImVec2 vp0 = map(b.x0 + f * (b.x1 - b.x0), b.y0);
    const ImVec2 vp1 = map(b.x0 + f * (b.x1 - b.x0), b.y1);
    dl->AddLine(vp0, vp1, IM_COL32(30, 36, 48, 255));
  }
  std::snprintf(label, sizeof(label), "%.3g", b.x0);
  dl->AddText(ImVec2(origin.x + kLeft, origin.y + h - 14.0F),
              IM_COL32(140, 150, 165, 255), label);
  std::snprintf(label, sizeof(label), "%.3g", b.x1);
  dl->AddText(ImVec2(origin.x + w - 44.0F, origin.y + h - 14.0F),
              IM_COL32(140, 150, 165, 255), label);
  dl->AddText(ImVec2(origin.x + kLeft, origin.y + 1.0F),
              IM_COL32(170, 180, 195, 255), title);
  dl->AddText(ImVec2(origin.x + w - 44.0F, origin.y + 1.0F),
              IM_COL32(120, 130, 145, 255), x_label);
  for (std::size_t k = 0; k < series.size(); ++k) {
    if (series[k].size() < 2) {
      continue;
    }
    std::vector<ImVec2> pts;
    pts.reserve(series[k].size());
    for (const ImVec2 p : series[k]) {
      pts.push_back(map(static_cast<double>(p.x), static_cast<double>(p.y)));
    }
    const double golden = static_cast<double>(k) * 0.61803398875;
    const float hue = static_cast<float>(golden - std::floor(golden));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                    ImColor::HSV(hue, 0.7F, 0.9F), ImDrawFlags_None, 1.5F);
  }
}

} // namespace graphs_detail

struct GraphsPanel {
  PlotHistory history;
  bool attached = false;

  void draw(SimController &sim) {
    // One-time observer attach; the controller re-attaches it on every
    // rebuild, so config edits and resets never detach the graphs.
    if (!attached) {
      sim.add_sink(history);
      attached = true;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0F, 300.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(790.0F, 410.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("Graphs");
    const auto &snaps = history.snapshots();
    if (snaps.empty()) {
      ImGui::TextDisabled("Run the simulation to collect points.");
      ImGui::TextDisabled("Series follow the run's emit rate; pausing "
                          "freezes the plots.");
      ImGui::End();
      return;
    }
    std::size_t slots = 0;
    for (const auto &s : snaps) {
      if (s.particle_count() > slots) {
        slots = s.particle_count();
      }
    }
    std::vector<std::vector<ImVec2>> height;
    std::vector<std::vector<ImVec2>> traj;
    height.resize(slots);
    traj.resize(slots);
    for (const auto &s : snaps) {
      for (std::size_t k = 0; k < s.particle_count() && k < slots; ++k) {
        height[k].push_back(ImVec2(static_cast<float>(s.sim_time),
                                   static_cast<float>(s.pos_y[k])));
        traj[k].push_back(ImVec2(static_cast<float>(s.pos_x[k]),
                                 static_cast<float>(s.pos_y[k])));
      }
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    graphs_detail::draw_plot(dl, "Height y(t)", "t [s]", height, 118.0F);
    graphs_detail::draw_plot(dl, "Trajectory y(x)", "x [m]", traj, 118.0F);
    ImGui::TextDisabled("%llu snapshots (cap %llu) | steps %llu..%llu",
                        static_cast<unsigned long long>(snaps.size()),
                        static_cast<unsigned long long>(PlotHistory::kCapacity),
                        static_cast<unsigned long long>(snaps.front().step_index),
                        static_cast<unsigned long long>(snaps.back().step_index));
    ImGui::End();
  }
};

} // namespace wengine::gui
