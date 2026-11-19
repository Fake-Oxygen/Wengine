#pragma once

/**
 * @file
 * Entity identity for world state: a stable slot index plus a
 * generation counter, managed by an arena-style registry with a
 * deterministic LIFO freelist. The generation guards against ABA bugs
 * from stale handles: despawn bumps it, so an old handle never aliases a
 * recycled slot. Consumed by every world pool (particles, signals) and
 * by serialization, which stores the freelist verbatim so a round-trip
 * preserves exact spawn-reuse behavior. This header owns no physics and
 * no I/O.
 */

// Entity identity for world state: stable slot index + generation counter.
// Slots are reused via freelist (dense, deterministic order); the generation
// guards against ABA bugs from stale handles: despawn bumps it, so an old
// handle never aliases a recycled slot.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wengine::world {

/**
 * @brief Entity handle: slot index plus the slot's generation.
 *
 * @note Pools are indexed by `index`; `generation` disambiguates slot
 *       reuse. A handle from before a despawn compares unequal to any
 *       handle minted after the slot recycled, so stale handles are
 *       detectable, never silently aliasing.
 */
struct Entity {
  /**
   * @brief Sentinel slot index marking a handle that was never spawned
   *        (also the full 32-bit slot space limit).
   */
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

  /**
   * @brief Slot index into the pools and registry; stable while the
   *        entity is alive, recycled after despawn.
   */
  std::uint32_t index{kInvalidIndex};
  /**
   * @brief Generation of the slot at spawn time; despawn bumps the
   *        slot's stored generation, invalidating this handle.
   */
  std::uint32_t generation{0};

  /**
   * @brief Whether this handle references anything at all.
   *
   * @return true when index != kInvalidIndex; false for default and
   *         kInvalidEntity handles.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
  /**
   * @brief Value equality against another handle (index and
   *        generation must both match).
   *
   * @return true when both fields match.
   */
  constexpr bool operator==(const Entity &) const noexcept = default;
};

/**
 * @brief The default invalid handle: index kInvalidIndex, generation 0.
 */
inline constexpr Entity kInvalidEntity{};

/**
 * @brief Slot arena: mints Entity handles, recycles slots through a
 *        freelist, and keeps per-slot generations.
 *
 * @note Reuse order is deterministic: despawned slots go to a LIFO
 *       stack and spawn pops its back, so identical call sequences
 *       produce identical slot assignments (a serialization
 *       round-trip preserves this exactly by storing the freelist).
 */
class EntityRegistry {
public:
  /**
   * @brief Number of live entities.
   *
   * @return current live count.
   */
  [[nodiscard]] std::size_t alive_count() const noexcept { return alive_; }
  /**
   * @brief Number of slots ever allocated (live plus free).
   *
   * @return generations vector size; slots are never freed, only
   *         recycled.
   */
  [[nodiscard]] std::size_t capacity() const noexcept { return generations_.size(); }

  /**
   * @brief Mint a new entity handle.
   *
   * @return Entity with the reused freelist slot and its current
   *         generation, or a fresh slot with generation 0 when the
   *         freelist is empty (deterministic LIFO reuse).
   */
  Entity spawn() {
    if (!free_.empty()) {
      const std::uint32_t slot = free_.back();
      free_.pop_back();
      ++alive_;
      return {slot, generations_[slot]};
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(generations_.size());
    generations_.push_back(0);
    ++alive_;
    return {slot, 0};
  }

  /**
   * @brief Release an entity's slot back to the freelist.
   *
   * @param[in] e handle to release.
   * @return true when e was live and is now released; false when the
   *         handle is stale, foreign, or already despawned (nothing
   *         changed).
   * @note Bumps the slot's generation, so any older handle minted
   *       before this despawn stops matching the recycled slot.
   */
  // False when the handle is stale, foreign, or already despawned.
  bool despawn(Entity e) {
    if (!alive(e)) {
      return false;
    }
    ++generations_[e.index];
    free_.push_back(e.index);
    --alive_;
    return true;
  }

  /**
   * @brief Liveness predicate for a handle.
   *
   * @param[in] e handle to test.
   * @return true when e's index is in range and its generation matches
   *         the slot's current generation.
   */
  [[nodiscard]] bool alive(Entity e) const noexcept {
    return e.index < generations_.size() && generations_[e.index] == e.generation;
  }

  /**
   * @brief Reset the registry: no live entities, no allocated slots.
   *
   * @note Unlike the pools' clear(), this releases all storage;
   *       minted handles from before are all stale afterwards.
   */
  void clear() noexcept {
    generations_.clear();
    free_.clear();
    alive_ = 0;
  }

  /**
   * @brief Serialization support: full-fidelity view of the per-slot
   *        generations.
   *
   * @return const reference to the generations vector; the registry owns
   *         the storage.
   */
  // Serialization support: full-fidelity access (freelist order included,
  // so a round-trip preserves exact spawn-reuse behavior).
  // restore() trusts validated input; use load_state() for checked loading.
  [[nodiscard]] const std::vector<std::uint32_t> &generations() const noexcept {
    return generations_;
  }
  /**
   * @brief Serialization support: full-fidelity view of the freelist
   *        (back() is the next slot spawn reuses).
   *
   * @return const reference to the freelist vector; the registry owns
   *         the storage.
   */
  [[nodiscard]] const std::vector<std::uint32_t> &free_list() const noexcept { return free_; }

  /**
   * @brief Serialization support: replace the registry contents.
   *
   * @param[in] generations per-slot generation counters, moved in.
   * @param[in] free_list free-slot stack in exact reuse order, moved
   *            in.
   * @note Trusts validated input (sizes, duplicates, ranges are the
   *       caller's contract); use load_state() for checked loading.
   *       alive_ is recomputed from the two vectors.
   */
  void restore(std::vector<std::uint32_t> generations, std::vector<std::uint32_t> free_list) {
    generations_ = std::move(generations);
    free_ = std::move(free_list);
    alive_ = generations_.size() >= free_.size() ? generations_.size() - free_.size() : 0;
  }

  /**
   * @brief Whole-registry equality against another registry
   *        (generations, freelist, live count).
   *
   * @return true when all serialized state matches.
   */
  bool operator==(const EntityRegistry &) const noexcept = default;

private:
  std::vector<std::uint32_t> generations_;
  std::vector<std::uint32_t> free_; // stack: back() is the next reused slot
  std::size_t alive_{0};
};

} // namespace wengine::world
