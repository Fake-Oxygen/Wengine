// 3D viewport math: orbit camera projection, grid generation, guards.
// Pure glm-double computation; the ImGui rasterization is verified visually.

#include "views/camera3d.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_target_projects_to_center() {
  // Any camera looking at its target maps it to viewport center.
  wengine::gui::Camera3d cam;
  cam.target_x = 1.0;
  cam.target_y = 3.0;
  cam.target_z = -2.0;
  cam.yaw = 0.7;
  cam.pitch = 0.45;
  cam.distance = 30.0;
  const wengine::gui::Camera3d::Screen s =
      cam.project({1.0, 3.0, -2.0}, 800.0, 600.0);
  assert(!s.behind);
  assert(near(s.x, 400.0));
  assert(near(s.y, 300.0));
  assert(near(s.depth, 30.0, 1e-6));
}

void test_behind_camera_culled() {
  wengine::gui::Camera3d cam;
  const glm::dvec3 eye = cam.eye();
  const glm::dvec3 fwd = cam.forward();
  // In front: toward the scene along forward.
  const wengine::gui::Camera3d::Screen front =
      cam.project(eye + fwd * 5.0, 800.0, 600.0);
  assert(!front.behind && front.depth > 0.0);
  // Behind: opposite the view direction.
  const wengine::gui::Camera3d::Screen back =
      cam.project(eye - fwd * 5.0, 800.0, 600.0);
  assert(back.behind);
}

void test_depth_orders_along_view() {
  wengine::gui::Camera3d cam;
  const glm::dvec3 eye = cam.eye();
  const glm::dvec3 fwd = cam.forward();
  const wengine::gui::Camera3d::Screen p_near =
      cam.project(eye + fwd * 2.0, 800.0, 600.0);
  const wengine::gui::Camera3d::Screen p_far =
      cam.project(eye + fwd * 10.0, 800.0, 600.0);
  assert(!p_near.behind && !p_far.behind);
  assert(p_near.depth < p_far.depth);
  assert(near(p_near.depth, 2.0, 1e-6));
  assert(near(p_far.depth, 10.0, 1e-6));
}

void test_viewport_guards() {
  wengine::gui::Camera3d cam;
  assert(cam.project({0.0, 0.0, 0.0}, 0.0, 600.0).behind);
  assert(cam.project({0.0, 0.0, 0.0}, 800.0, -1.0).behind);
}

void test_orbit_zoom_pan() {
  wengine::gui::Camera3d cam;
  const wengine::gui::Camera3d::Screen before =
      cam.project({5.0, 1.0, 0.0}, 800.0, 600.0);
  cam.orbit(100.0, 0.0);
  const wengine::gui::Camera3d::Screen after =
      cam.project({5.0, 1.0, 0.0}, 800.0, 600.0);
  assert(!after.behind);
  assert(!near(before.x, after.x, 1e-6)); // yaw visibly moved the point

  // Pitch clamps short of the pole (right() never degenerates).
  cam.orbit(0.0, 1e6);
  assert(cam.pitch == 1.55);
  cam.orbit(0.0, -1e6);
  assert(cam.pitch == -1.55);

  // Zoom clamps to [1, 500].
  cam.zoom(1e6);
  assert(cam.distance == 1.0);
  cam.zoom(-1e6);
  assert(cam.distance == 500.0);

  // Pan moves the target (grab semantics: content follows the cursor).
  const double tx = cam.target_x;
  cam.pan(10.0, 0.0);
  assert(cam.target_x != tx);
}

void test_grid_segments() {
  const auto segs = wengine::gui::grid_segments(10.0, 5.0);
  // (2*2 + 1) lines per direction.
  assert(segs.size() == 10);
  for (const auto &seg : segs) {
    for (const glm::dvec3 p : {seg.first, seg.second}) {
      assert(p.y == 0.0);
      assert(p.x >= -10.0 && p.x <= 10.0);
      assert(p.z >= -10.0 && p.z <= 10.0);
    }
  }
  assert(wengine::gui::grid_segments(10.0, 0.0).empty());
  assert(wengine::gui::grid_segments(-1.0, 5.0).empty());
}

} // namespace

int main() {
  test_target_projects_to_center();
  test_behind_camera_culled();
  test_depth_orders_along_view();
  test_viewport_guards();
  test_orbit_zoom_pan();
  test_grid_segments();
  std::printf("test_viewport3d: OK\n");
  return 0;
}
