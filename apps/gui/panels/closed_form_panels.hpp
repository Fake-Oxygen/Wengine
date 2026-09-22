#pragma once

// Closed-form solver panel registry: one entry per solver panel.
// Adding a solver panel = define its draw function in cf_panels.cpp and
// append one line to kClosedFormPanels below (mirrors the CLI registry's
// single-entry rule). Deliberately ImGui-free: draw functions are declared
// only, so unit tests can check the registry size without linking ImGui.

#include <array>
#include <string_view>

namespace wengine::gui {

void draw_ohm_panel();
void draw_s0_panel();
void draw_i2c_panel();

struct ClosedFormPanel {
  std::string_view name;
  std::string_view description;
  void (*draw)();
};

extern const std::array<ClosedFormPanel, 3> kClosedFormPanels;

} // namespace wengine::gui
