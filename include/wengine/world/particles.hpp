#pragma once

// First per-domain SoA component pool: point particles (position + velocity).
// Indexed by entity slot; `present` marks live components so entities may
// exist without particles (statics, sensors, ...). Arrays grow lazily via
// ensure()/attach(); iteration skips holes. Template for future domains:
// same shape (parallel vectors + occupancy), owned by world::State which
// keeps registry and pools consistent on despawn.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::world {

struct Particles {
  std::vector<double> pos_x;
  std::vector<double> pos_y;
  std::vector<double> vel_x;
  std::vector<double> vel_y;
  std::vector<std::uint8_t> present; // 1 = live particle in slot
  std::size_t live_count{0};

  [[nodiscard]] std::size_t capacity() const noexcept { return present.size(); }

  [[nodiscard]] bool has(std::uint32_t index) const noexcept {
    return index < present.size() && present[index] != 0;
  }

  // Grow arrays so `index` is addressable. New slots are absent and zeroed.
  void ensure(std::uint32_t index) {
    const std::size_t need = static_cast<std::size_t>(index) + 1;
    if (need <= present.size()) {
      return;
    }
    pos_x.resize(need, 0.0);
    pos_y.resize(need, 0.0);
    vel_x.resize(need, 0.0);
    vel_y.resize(need, 0.0);
    present.resize(need, 0);
  }

  // Attach a live particle to `index` (idempotent; keeps existing data).
  void attach(std::uint32_t index) {
    ensure(index);
    if (present[index] == 0) {
      present[index] = 1;
      ++live_count;
    }
  }

  // Detach (idempotent no-op when absent). Capacity is retained.
  void detach(std::uint32_t index) noexcept {
    if (index < present.size() && present[index] != 0) {
      present[index] = 0;
      --live_count;
    }
  }

  void clear() noexcept {
    pos_x.clear();
    pos_y.clear();
    vel_x.clear();
    vel_y.clear();
    present.clear();
    live_count = 0;
  }

  bool operator==(const Particles &) const noexcept = default;
};

} // namespace wengine::world
