#pragma once

// Shared entity palette for the 2D/3D views: stable per-index hues,
// honoring per-item color hints (non-zero wins, e.g. future heat data).

#include "imgui.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wengine::gui {

[[nodiscard]] inline std::uint32_t item_color(std::size_t index,
                                              std::uint32_t hint) {
  if (hint != 0) {
    return hint;
  }
  const double h = static_cast<double>(index) * 0.61803398875;
  return ImColor::HSV(static_cast<float>(h - std::floor(h)), 0.75F, 0.95F);
}

} // namespace wengine::gui
