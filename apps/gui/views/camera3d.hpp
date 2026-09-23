#pragma once

// 3D viewport math: orbit camera, perspective projection to viewport pixels,
// ground-grid generation. Pure glm-double math with engine-only includes —
// unit-tested; ImGui rasterization lives in viewport3d_panel.hpp.
//
// Convention: world X right, Y up, Z toward the viewer. Sim snapshots are
// 2D (x, y): world = (x, y, 0). The floor grid spans the XZ plane at y = 0.
//
// Rendering approach (recorded choice): CPU projection rasterized through
// ImGui's ImDrawList — no shader toolchain is installed here, and current
// entity counts (tens to hundreds) never approach the point where a raw
// Vulkan pipeline pays off. Camera3d + const Snapshot is the seam a future
// GPU pipeline fills without touching callers.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace wengine::gui {

struct Camera3d {
  double target_x = 0.0;
  double target_y = 2.0;
  double target_z = 0.0;
  double yaw = 0.7;   // orbit around Y, radians
  double pitch = 0.45; // elevation, radians, clamped to (-1.55, 1.55)
  double distance = 30.0; // clamped to [1, 500]
  double fov_y_deg = 45.0;
  bool track = true; // follow the primary item each frame; manual pan
                     // releases (orbit/zoom stay compatible with tracking)

  [[nodiscard]] glm::dvec3 target() const noexcept {
    return {target_x, target_y, target_z};
  }

  [[nodiscard]] glm::dvec3 eye() const noexcept {
    const double cp = std::cos(pitch);
    return {target_x + distance * cp * std::sin(yaw),
            target_y + distance * std::sin(pitch),
            target_z + distance * cp * std::cos(yaw)};
  }

  [[nodiscard]] glm::dvec3 forward() const noexcept {
    return glm::normalize(target() - eye());
  }

  [[nodiscard]] glm::dvec3 right() const noexcept {
    return glm::normalize(glm::cross(forward(), glm::dvec3{0.0, 1.0, 0.0}));
  }

  [[nodiscard]] glm::dvec3 up() const noexcept {
    return glm::cross(right(), forward());
  }

  // Follow mode: point the orbit target at (x, y, z) — the panel calls
  // this with the primary item while track holds, so entities stay framed
  // no matter how far the run travels (the fixed default target only suits
  // the origin neighborhood). Pure target move; orbit/zoom keep working.
  void follow(double x, double y, double z) noexcept {
    target_x = x;
    target_y = y;
    target_z = z;
  }

  [[nodiscard]] glm::dmat4 view_matrix() const {
    return glm::lookAt(eye(), target(), up());
  }

  [[nodiscard]] glm::dmat4 proj_matrix(double aspect) const {
    return glm::perspective(glm::radians(fov_y_deg), aspect, 0.1, 2000.0);
  }

  struct Screen {
    double x = 0.0; // viewport-local pixels, (0,0) top-left
    double y = 0.0;
    double depth = 0.0; // view-space distance in front, for sort/cull
    bool behind = false;
  };

  [[nodiscard]] Screen project(glm::dvec3 world, double vp_w, double vp_h) const {
    Screen s;
    if (vp_w <= 0.0 || vp_h <= 0.0) {
      s.behind = true;
      return s;
    }
    const glm::dmat4 vp = proj_matrix(vp_w / vp_h) * view_matrix();
    const glm::dvec4 clip = vp * glm::dvec4(world, 1.0);
    if (clip.w <= 0.0) {
      s.behind = true;
      return s;
    }
    const glm::dvec3 ndc = glm::dvec3(clip) / clip.w;
    s.x = (ndc.x * 0.5 + 0.5) * vp_w;
    s.y = (0.5 - ndc.y * 0.5) * vp_h;
    const glm::dvec4 view = view_matrix() * glm::dvec4(world, 1.0);
    s.depth = -view.z;
    s.behind = s.depth <= 0.0;
    return s;
  }

  void orbit(double dx_pixels, double dy_pixels) noexcept {
    constexpr double kRadPerPixel = 0.005;
    yaw -= dx_pixels * kRadPerPixel;
    pitch += dy_pixels * kRadPerPixel;
    if (pitch > 1.55) {
      pitch = 1.55;
    } else if (pitch < -1.55) {
      pitch = -1.55;
    }
  }

  void zoom(double wheel_steps) noexcept {
    distance *= std::pow(1.1, -wheel_steps);
    if (distance < 1.0) {
      distance = 1.0;
    } else if (distance > 500.0) {
      distance = 500.0;
    }
  }

  void pan(double dx_pixels, double dy_pixels) noexcept {
    track = false; // manual pan takes over from follow mode
    const double s = distance * 0.0016;
    const glm::dvec3 r = right();
    const glm::dvec3 u = up();
    target_x -= (r.x * dx_pixels - u.x * dy_pixels) * s;
    target_y -= (r.y * dx_pixels - u.y * dy_pixels) * s;
    target_z -= (r.z * dx_pixels - u.z * dy_pixels) * s;
  }
};

// Floor grid on y = 0: (2*n + 1) lines per direction, n = round(half/step).
[[nodiscard]] inline std::vector<std::pair<glm::dvec3, glm::dvec3>>
grid_segments(double half_extent, double step) {
  std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
  if (!(half_extent > 0.0) || !(step > 0.0)) {
    return segs;
  }
  const auto n =
      static_cast<std::size_t>(std::llround(half_extent / step));
  segs.reserve(4 * n + 2);
  for (std::size_t k = 0; k <= 2 * n; ++k) {
    const double c = -half_extent + static_cast<double>(k) * step;
    segs.emplace_back(glm::dvec3{-half_extent, 0.0, c},
                      glm::dvec3{half_extent, 0.0, c});
    segs.emplace_back(glm::dvec3{c, 0.0, -half_extent},
                      glm::dvec3{c, 0.0, half_extent});
  }
  return segs;
}

} // namespace wengine::gui
