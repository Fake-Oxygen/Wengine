#pragma once

// 2D world-view math: top-down camera (center + pixels-per-unit scale),
// world<->screen mapping, fit-to-data, and nice grid steps. Pure doubles,
// engine-only includes — unit-tested; ImGui rasterization lives in
// canvas2d_panel.hpp.
//
// Convention: world X right, Y up; screen X right, Y down. Follows the
// sim live (auto-fit) until the user zooms or pans, exactly like a
// tracking camera that yields on first manual input.

#include <cmath>
#include <cstddef>
#include <vector>

namespace wengine::gui {

struct View2d {
  double center_x = 0.0;
  double center_y = 0.0;
  double scale = 20.0; // pixels per world unit, clamped to [0.5, 2000]
  bool auto_fit = true;

  [[nodiscard]] double world_to_sx(double wx, double vp_x, double vp_w) const noexcept {
    return vp_x + vp_w * 0.5 + (wx - center_x) * scale;
  }

  [[nodiscard]] double world_to_sy(double wy, double vp_y, double vp_h) const noexcept {
    return vp_y + vp_h * 0.5 - (wy - center_y) * scale;
  }

  [[nodiscard]] double screen_to_wx(double sx, double vp_x, double vp_w) const noexcept {
    return center_x + (sx - vp_x - vp_w * 0.5) / scale;
  }

  [[nodiscard]] double screen_to_wy(double sy, double vp_y, double vp_h) const noexcept {
    return center_y - (sy - vp_y - vp_h * 0.5) / scale;
  }

  // Frame the given points (plus the origin for context) with a margin.
  // No points: fall back to a unit view around the origin.
  void fit(const std::vector<double> &xs, const std::vector<double> &ys, double vp_w,
           double vp_h) noexcept {
    double x0 = 0.0;
    double x1 = 0.0;
    double y0 = 0.0;
    double y1 = 0.0;
    for (std::size_t k = 0; k < xs.size() && k < ys.size(); ++k) {
      if (xs[k] < x0) x0 = xs[k];
      if (xs[k] > x1) x1 = xs[k];
      if (ys[k] < y0) y0 = ys[k];
      if (ys[k] > y1) y1 = ys[k];
    }
    double span = x1 - x0;
    if (y1 - y0 > span) {
      span = y1 - y0;
    }
    span *= 1.2; // margin
    if (!(span > 0.0)) {
      span = 10.0;
    }
    center_x = (x0 + x1) * 0.5;
    center_y = (y0 + y1) * 0.5;
    double s = vp_w < vp_h ? vp_w / span : vp_h / span;
    if (s < 0.5) {
      s = 0.5;
    } else if (s > 2000.0) {
      s = 2000.0;
    }
    scale = s;
  }

  void zoom(double wheel_steps) noexcept {
    auto_fit = false;
    scale *= std::pow(1.1, wheel_steps);
    if (scale < 0.5) {
      scale = 0.5;
    } else if (scale > 2000.0) {
      scale = 2000.0;
    }
  }

  void pan(double dx_pixels, double dy_pixels) noexcept {
    auto_fit = false;
    center_x -= dx_pixels / scale;
    center_y += dy_pixels / scale;
  }
};

// Grid step from {1, 2, 5} x 10^n targeting ~80 px between lines.
[[nodiscard]] inline double nice_step(double world_per_80px) noexcept {
  if (!(world_per_80px > 0.0)) {
    return 1.0;
  }
  const double exp = std::floor(std::log10(world_per_80px));
  const double base = std::pow(10.0, exp);
  const double m = world_per_80px / base;
  if (m < 1.5) {
    return base;
  }
  if (m < 3.5) {
    return 2.0 * base;
  }
  if (m < 7.5) {
    return 5.0 * base;
  }
  return 10.0 * base;
}

} // namespace wengine::gui
