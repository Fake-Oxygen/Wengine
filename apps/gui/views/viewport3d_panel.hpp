#pragma once

// 3D viewport panel: orbit/zoom/pan camera over a perspective-projected
// scene (floor grid, RGB axes, entity glyphs with velocity vectors),
// drawn into an ImGui window via ImDrawList.
//
// Frame-driven and read-only: takes `const DataFrame &` (any solver
// source), never touches live sim state, so camera moves cannot perturb
// the simulation. Far-to-near painter order; segments with an endpoint
// behind the camera are skipped.

#include "imgui.h"
#include "views/camera3d.hpp"
#include "views/frame_source.hpp"
#include "views/palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::gui {

struct Viewport3dPanel {
  Camera3d camera;

  void draw(const DataFrame &frame) {
    ImGui::SetNextWindowSize(ImVec2(420.0F, 340.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(880.0F, 60.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("3D Viewport");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 50.0F || avail.y < 50.0F) {
      ImGui::TextDisabled("Viewport too small.");
      ImGui::End();
      return;
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##view3d", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiIO &io = ImGui::GetIO();
    if (hovered) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        camera.orbit(static_cast<double>(io.MouseDelta.x),
                     static_cast<double>(io.MouseDelta.y));
      } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        camera.pan(static_cast<double>(io.MouseDelta.x),
                   static_cast<double>(io.MouseDelta.y));
      }
      if (io.MouseWheel != 0.0F) {
        camera.zoom(static_cast<double>(io.MouseWheel));
      }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin,
                     ImVec2(origin.x + avail.x, origin.y + avail.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                      IM_COL32(8, 10, 14, 255));
    const double vp_w = static_cast<double>(avail.x);
    const double vp_h = static_cast<double>(avail.y);
    const float ox = origin.x;
    const float oy = origin.y;
    auto to_screen = [&](const Camera3d::Screen &s) {
      return ImVec2(ox + static_cast<float>(s.x), oy + static_cast<float>(s.y));
    };

    // Floor grid + axes.
    for (const auto &seg : grid_segments(20.0, 5.0)) {
      const Camera3d::Screen a = camera.project(seg.first, vp_w, vp_h);
      const Camera3d::Screen b = camera.project(seg.second, vp_w, vp_h);
      if (a.behind || b.behind) {
        continue;
      }
      dl->AddLine(to_screen(a), to_screen(b), IM_COL32(45, 55, 70, 255));
    }
    constexpr double kAxisLen = 10.0;
    const glm::dvec3 axes_o{0.0, 0.0, 0.0};
    const glm::dvec3 axes_e[3] = {{kAxisLen, 0.0, 0.0},
                                  {0.0, kAxisLen, 0.0},
                                  {0.0, 0.0, kAxisLen}};
    const std::uint32_t axes_c[3] = {IM_COL32(220, 70, 70, 255),
                                     IM_COL32(90, 200, 90, 255),
                                     IM_COL32(90, 140, 230, 255)};
    for (std::size_t a = 0; a < 3; ++a) {
      const Camera3d::Screen p0 = camera.project(axes_o, vp_w, vp_h);
      const Camera3d::Screen p1 = camera.project(axes_e[a], vp_w, vp_h);
      if (p0.behind || p1.behind) {
        continue;
      }
      dl->AddLine(to_screen(p0), to_screen(p1), axes_c[a], 2.0F);
    }

    // Entities far-to-near; velocity stub shows heading.
    std::vector<std::size_t> order(frame.items.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
      order[k] = k;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      const Camera3d::Screen pa = camera.project(
          {frame.items[a].x, frame.items[a].y, frame.items[a].z}, vp_w, vp_h);
      const Camera3d::Screen pb = camera.project(
          {frame.items[b].x, frame.items[b].y, frame.items[b].z}, vp_w, vp_h);
      return pa.depth > pb.depth;
    });
    std::size_t drawn = 0;
    for (std::size_t k : order) {
      const SpatialItem &it = frame.items[k];
      const glm::dvec3 p{it.x, it.y, it.z};
      const Camera3d::Screen s = camera.project(p, vp_w, vp_h);
      if (s.behind) {
        continue;
      }
      const float r =
          static_cast<float>(140.0 / (s.depth + 20.0) * it.size);
      const float radius = r < 2.5F ? 2.5F : (r > 9.0F ? 9.0F : r);
      const ImVec2 c = to_screen(s);
      dl->AddCircleFilled(c, radius, item_color(k, it.color));
      if (it.has_velocity) {
        const Camera3d::Screen tip = camera.project(
            {p.x + it.vx * 0.5, p.y + it.vy * 0.5, p.z + it.vz * 0.5}, vp_w,
            vp_h);
        if (!tip.behind) {
          dl->AddLine(c, to_screen(tip), IM_COL32(120, 220, 130, 255), 1.5F);
        }
      }
      ++drawn;
    }
    dl->PopClipRect();

    if (frame.items.empty()) {
      ImGui::TextDisabled("No items — grid only.");
    } else {
      ImGui::TextDisabled("%llu items (%llu drawn) | drag orbit, "
                          "right-drag pan, wheel zoom",
                          static_cast<unsigned long long>(frame.items.size()),
                          static_cast<unsigned long long>(drawn));
    }
    ImGui::End();
  }
};

} // namespace wengine::gui
