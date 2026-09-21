#pragma once

// Entity identity for world state: stable slot index + generation counter.
// Slots are reused via freelist (dense, deterministic order); the generation
// guards against ABA bugs from stale handles: despawn bumps it, so an old
// handle never aliases a recycled slot.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wengine::world {

struct Entity {
  static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

  std::uint32_t index{kInvalidIndex};
  std::uint32_t generation{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
  constexpr bool operator==(const Entity &) const noexcept = default;
};

inline constexpr Entity kInvalidEntity{};

class EntityRegistry {
public:
  [[nodiscard]] std::size_t alive_count() const noexcept { return alive_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return generations_.size(); }

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

  [[nodiscard]] bool alive(Entity e) const noexcept {
    return e.index < generations_.size() && generations_[e.index] == e.generation;
  }

  void clear() noexcept {
    generations_.clear();
    free_.clear();
    alive_ = 0;
  }

  // Serialization support: full-fidelity access (freelist order included,
  // so a round-trip preserves exact spawn-reuse behavior).
  // restore() trusts validated input; use load_state() for checked loading.
  [[nodiscard]] const std::vector<std::uint32_t> &generations() const noexcept {
    return generations_;
  }
  [[nodiscard]] const std::vector<std::uint32_t> &free_list() const noexcept { return free_; }

  void restore(std::vector<std::uint32_t> generations, std::vector<std::uint32_t> free_list) {
    generations_ = std::move(generations);
    free_ = std::move(free_list);
    alive_ = generations_.size() >= free_.size() ? generations_.size() - free_.size() : 0;
  }

  bool operator==(const EntityRegistry &) const noexcept = default;

private:
  std::vector<std::uint32_t> generations_;
  std::vector<std::uint32_t> free_; // stack: back() is the next reused slot
  std::size_t alive_{0};
};

} // namespace wengine::world
