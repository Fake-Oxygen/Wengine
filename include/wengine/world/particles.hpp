#pragma once

/**
 * @file
 * The first per-domain SoA component pool: point particles (position +
 * velocity), indexed by entity slot with a `present` occupancy column.
 * Owned by world::State, which keeps the registry and every pool
 * consistent on despawn (detach before slot release). The parallel-
 * vectors + occupancy shape is the template for future domain pools —
 * new pools copy this file's shape, not its code. This header must stay
 * free of solver knowledge: update semantics live in the solvers, the
 * pool only stores.
 */

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

/**
 * @brief SoA pool of point particles: position, velocity, and
 *        occupancy per entity slot.
 *
 * @note Arrays are parallel and indexed by entity slot; holes (present
 *       == 0) are skipped by every iteration. Positions and velocities
 *       are raw SI doubles (metres and metres per second) — solvers own
 *       the update semantics (semi-implicit Euler steps these in
 *       place), the pool only stores.
 */
struct Particles {
  /**
   * @brief X position per slot, in metres (_m); meaningful only when
   *        the slot is present.
   */
  std::vector<double> pos_x;
  /**
   * @brief Y position per slot, in metres (_m); meaningful only when
   *        the slot is present.
   */
  std::vector<double> pos_y;
  /**
   * @brief X velocity per slot, in metres per second (_m/s).
   */
  std::vector<double> vel_x;
  /**
   * @brief Y velocity per slot, in metres per second (_m/s).
   */
  std::vector<double> vel_y;
  /**
   * @brief Occupancy column: 1 = live particle in the slot, 0 = hole.
   */
  std::vector<std::uint8_t> present; // 1 = live particle in slot
  /**
   * @brief Number of present slots (exact, maintained by
   *        attach()/detach()).
   */
  std::size_t live_count{0};

  /**
   * @brief Number of allocated slots (present.size()).
   *
   * @return slots addressable by ensure()/attach() today.
   */
  [[nodiscard]] std::size_t capacity() const noexcept { return present.size(); }

  /**
   * @brief Occupancy predicate.
   *
   * @param[in] index slot to test.
   * @return true when the index is in range and the slot is present.
   */
  [[nodiscard]] bool has(std::uint32_t index) const noexcept {
    return index < present.size() && present[index] != 0;
  }

  /**
   * @brief Grow arrays so `index` is addressable. New slots are absent
   *        and zeroed.
   *
   * @param[in] index slot that must become addressable.
   * @note Setup-path allocation (not the hot step): all five vectors
   *       resize together so the SoA columns stay index-aligned.
   */
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

  /**
   * @brief Attach a live particle to `index` (idempotent; keeps
   *        existing data).
   *
   * @param[in] index slot to mark present; ensure()d first when needed.
   * @note Existing columns are preserved, so callers may pre-write
   *       data and attach afterwards.
   */
  // Attach a live particle to `index` (idempotent; keeps existing data).
  void attach(std::uint32_t index) {
    ensure(index);
    if (present[index] == 0) {
      present[index] = 1;
      ++live_count;
    }
  }

  /**
   * @brief Detach (idempotent no-op when absent). Capacity is retained.
   *
   * @param[in] index slot to clear.
   * @note Clears presence only: the despawn path detaches every pool
   *       before the slot is released; data columns keep their last
   *       values and are overwritten on the next attach.
   */
  // Detach (idempotent no-op when absent). Capacity is retained.
  void detach(std::uint32_t index) noexcept {
    if (index < present.size() && present[index] != 0) {
      present[index] = 0;
      --live_count;
    }
  }

  /**
   * @brief Reset the pool: no slots, no capacity, zero live count.
   */
  void clear() noexcept {
    pos_x.clear();
    pos_y.clear();
    vel_x.clear();
    vel_y.clear();
    present.clear();
    live_count = 0;
  }

  /**
   * @brief Whole-pool equality against another pool: every column and
   *        the live count.
   *
   * @return true when all arrays and the counters match.
   */
  bool operator==(const Particles &) const noexcept = default;
};

} // namespace wengine::world
