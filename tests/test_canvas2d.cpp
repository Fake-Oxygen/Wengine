// 2D world-view math: mapping round-trips, fit, zoom/pan, grid steps.

#include "views/worldview2d.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_mapping_round_trip() {
  wengine::gui::View2d v;
  v.center_x = 3.0;
  v.center_y = -2.0;
  v.scale = 25.0;
  const double sx = v.world_to_sx(7.5, 0.0, 600.0);
  const double sy = v.world_to_sy(1.0, 0.0, 400.0);
  assert(near(sx, 300.0 + 4.5 * 25.0));
  // Screen Y grows downward while world Y grows upward.
  assert(near(sy, 200.0 - 3.0 * 25.0));
  assert(near(v.screen_to_wx(sx, 0.0, 600.0), 7.5));
  assert(near(v.screen_to_wy(sy, 0.0, 400.0), 1.0));
}

void test_fit() {
  wengine::gui::View2d v;
  // Points (0,0),(10,0),(0,5); origin is inside the box already.
  // Span = max(10, 5) * 1.2 = 12; 400px-tall viewport drives the scale.
  v.fit({0.0, 10.0, 0.0}, {0.0, 0.0, 5.0}, 600.0, 400.0);
  assert(near(v.center_x, 5.0));
  assert(near(v.center_y, 2.5));
  assert(near(v.scale, 400.0 / 12.0));
  // Empty input: unit view around the origin.
  wengine::gui::View2d e;
  e.fit({}, {}, 600.0, 400.0);
  assert(near(e.center_x, 0.0) && near(e.center_y, 0.0));
  assert(near(e.scale, 400.0 / 10.0));
}

void test_zoom_pan() {
  wengine::gui::View2d v;
  assert(v.auto_fit);
  v.zoom(1.0);
  assert(!v.auto_fit && near(v.scale, 20.0 * 1.1));
  v.zoom(1e6);
  assert(v.scale == 2000.0);
  v.zoom(-1e6);
  assert(v.scale == 0.5);
  // Drag right moves the world right (center shifts left), drag down
  // moves the world down (center shifts up in world Y).
  v.scale = 20.0;
  v.center_x = 0.0;
  v.center_y = 0.0;
  v.pan(40.0, 0.0);
  assert(near(v.center_x, -2.0));
  v.pan(0.0, 20.0);
  assert(near(v.center_y, 1.0));
}

void test_nice_step() {
  using wengine::gui::nice_step;
  assert(nice_step(80.0) == 100.0);
  assert(nice_step(3.0) == 2.0);
  assert(near(nice_step(0.4), 0.5));
  assert(nice_step(0.0) == 1.0);
  assert(nice_step(-5.0) == 1.0);
}

} // namespace

int main() {
  test_mapping_round_trip();
  test_fit();
  test_zoom_pan();
  test_nice_step();
  std::printf("test_canvas2d: OK\n");
  return 0;
}
