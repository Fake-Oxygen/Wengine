#pragma once

// 2D world canvas: top-down view of the simulation plane with a world-unit
// grid, axes, entity glyphs, velocity vectors, and trajectory trails from
// the shared frame history.
//
// Frame-driven and read-only (const link::Frame + const FrameHistory):
// pan/zoom never touch sim state or timing. Auto-fit tracks the sim until
// the first manual zoom/pan; the Fit button resumes tracking.

#include "imgui.h"
#include "views/frame_source.hpp"
#include "views/palette.hpp"
#include "views/worldview2d.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace wengine::gui {

struct Canvas2dPanel {
  View2d view;

  void draw(const link::Frame &frame, const FrameHistory &history) {
    ImGui::SetNextWindowSize(ImVec2(430.0F, 430.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(430.0F, 20.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("2D Canvas");
    const ImVec2 region_origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 60.0F || avail.y < 120.0F) {
      ImGui::TextDisabled("Canvas too small.");
      ImGui::End();
      return;
    }
    if (ImGui::Button("Fit")) {
      view.auto_fit = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("drag pan, wheel zoom");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float used_h = origin.y - region_origin.y;
    const ImVec2 size(avail.x, avail.y - used_h);
    if (size.x < 60.0F || size.y < 60.0F) {
      ImGui::End();
      return;
    }
    ImGui::InvisibleButton("##canvas2d", size);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO &io = ImGui::GetIO();
    const double vp_w = static_cast<double>(size.x);
    const double vp_h = static_cast<double>(size.y);

    if (view.auto_fit) {
      std::vector<double> xs;
      std::vector<double> ys;
      xs.reserve(frame.items.size());
      ys.reserve(frame.items.size());
      for (const auto &it : frame.items) {
        xs.push_back(it.x);
        ys.push_back(it.y);
      }
      view.fit(xs, ys, vp_w, vp_h);
    }
    if (hovered) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        view.pan(static_cast<double>(io.MouseDelta.x),
                 static_cast<double>(io.MouseDelta.y));
      }
      if (io.MouseWheel != 0.0F) {
        view.zoom(static_cast<double>(io.MouseWheel));
      }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(8, 10, 14, 255));
    const float ox = origin.x;
    const float oy = origin.y;
    auto px = [&](double wx) {
      return ox + static_cast<float>(view.world_to_sx(wx, 0.0, vp_w));
    };
    auto py = [&](double wy) {
      return oy + static_cast<float>(view.world_to_sy(wy, 0.0, vp_h));
    };

    // World-unit grid + axes through the origin.
    const double step = nice_step(80.0 / view.scale);
    const double gx0 = view.screen_to_wx(0.0, 0.0, vp_w);
    const double gx1 = view.screen_to_wx(vp_w, 0.0, vp_w);
    const double gy0 = view.screen_to_wy(vp_h, 0.0, vp_h);
    const double gy1 = view.screen_to_wy(0.0, 0.0, vp_h);
    for (double gx = std::floor(gx0 / step) * step; gx <= gx1; gx += step) {
      const float sx = px(gx);
      // Vertical line x = const: the Y axis when gx == 0 (green).
      const bool axis = std::fabs(gx) < step * 0.5;
      dl->AddLine(ImVec2(sx, oy), ImVec2(sx, oy + static_cast<float>(vp_h)),
                  axis ? IM_COL32(90, 190, 90, 255) : IM_COL32(40, 48, 62, 255),
                  axis ? 1.5F : 1.0F);
    }
    for (double gy = std::floor(gy0 / step) * step; gy <= gy1; gy += step) {
      const float sy = py(gy);
      // Horizontal line y = const: the X axis when gy == 0 (red).
      const bool axis = std::fabs(gy) < step * 0.5;
      dl->AddLine(ImVec2(ox, sy), ImVec2(ox + static_cast<float>(vp_w), sy),
                  axis ? IM_COL32(200, 90, 90, 255) : IM_COL32(40, 48, 62, 255),
                  axis ? 1.5F : 1.0F);
    }

    // Trails from the shared history (past positions, faint).
    std::size_t slots = frame.items.size();
    for (const auto &f : history.frames()) {
      if (f.items.size() > slots) {
        slots = f.items.size();
      }
    }
    for (std::size_t k = 0; k < slots; ++k) {
      std::vector<ImVec2> trail;
      for (const auto &f : history.frames()) {
        if (k < f.items.size()) {
          trail.push_back(ImVec2(px(f.items[k].x), py(f.items[k].y)));
        }
      }
      if (trail.size() >= 2) {
        dl->AddPolyline(trail.data(), static_cast<int>(trail.size()),
                        IM_COL32(110, 150, 210, 110), ImDrawFlags_None, 1.0F);
      }
    }

    // Live entities + velocity vectors.
    for (std::size_t k = 0; k < frame.items.size(); ++k) {
      const link::FrameItem &it = frame.items[k];
      const ImVec2 c(px(it.x), py(it.y));
      dl->AddCircleFilled(c, static_cast<float>(2.5 + it.size * 2.5),
                          item_color(k, it.color));
      if (it.has_velocity) {
        dl->AddLine(c, ImVec2(px(it.x + it.vx * 0.5), py(it.y + it.vy * 0.5)),
                    IM_COL32(120, 220, 130, 255), 1.5F);
      }
    }
    dl->PopClipRect();

    if (frame.items.empty()) {
      ImGui::TextDisabled("No items — grid only.");
    } else {
      ImGui::TextDisabled("%llu items | %.1f px/unit%s",
                          static_cast<unsigned long long>(frame.items.size()),
                          view.scale, view.auto_fit ? " | tracking" : "");
    }
    ImGui::End();
  }
};

} // namespace wengine::gui
