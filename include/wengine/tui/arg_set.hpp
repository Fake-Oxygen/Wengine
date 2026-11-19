#pragma once

/**
 * @file
 * Dynamically-sized set of argument-slot indices shared by the guided TUI
 * forms and run builders. The TUI is a front end: it owns presentation and
 * never simulation state, and these sets carry only which form/builder
 * argument slots are selected, allowed, or blocked — the ncurses layer
 * reads the resulting sets but never computes command semantics (the CLI
 * owns those verbatim, per docs/tui.md D-TUI-1). Consumers are
 * `wengine/tui/forms/types.hpp`, `wengine/tui/batches/types.hpp`, and the
 * per-command definitions beside them; nothing here touches ncurses or
 * the engine, so the selection algebra stays unit-testable without a
 * terminal.
 *
 * The forms and run builders used to track selected/allowed arguments in
 * a std::uint64_t bitmask, which capped every form at 64 arguments: the
 * `ic` form alone needs 28 for two chips and 41 more for a third, so the
 * cap had to go. Indices are unbounded now — registering new chips or
 * options never revisits this header.
 *
 * Vocabulary (kept deliberately close to the old one so definitions read
 * the same): `bit(i)` denotes the singleton {i}; `|`/`&` are union and
 * intersection; `-` is set difference. There is no complement operator:
 * live selections only ever hold indices from their own form, so every
 * former `selected & ~allowed` is spelled `selected - allowed`.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::tui {

/**
 * @brief Unbounded set of argument-slot indices.
 *
 * @note Word-backed and always trimmed (trailing zero words removed), so
 *       sets built by different code paths compare by value, not by word
 *       count. Indices have no fixed width: `single`, `below`, and
 *       `insert` grow storage on demand, and no operation ever needs a
 *       complement because live selections only hold indices from their
 *       own form.
 */
class ArgSet {
 public:
  /**
   * @brief Empty set.
   */
  ArgSet() = default;

  /**
   * @brief Singleton {index}, spelling the old `bit(index)`.
   *
   * @param[in] index Argument slot index (unbounded, 0-based).
   * @return The set containing exactly `index`.
   */
  [[nodiscard]] static ArgSet single(std::size_t index) {
    ArgSet out;
    out.insert(index);
    return out;
  }

  /**
   * @brief Range {0, ..., count-1}, spelling the old `bit(count)-1`.
   *
   * @param[in] count Number of leading indices to include.
   * @return The set {0, ..., count-1}; empty when count is 0.
   */
  [[nodiscard]] static ArgSet below(std::size_t count) {
    ArgSet out;
    for (std::size_t i = 0; i < count; ++i) {
      out.insert(i);
    }
    return out;
  }

  /**
   * @brief Whether the set holds no index.
   *
   * @return True when empty.
   */
  [[nodiscard]] bool empty() const noexcept {
    for (std::uint64_t word : words_) {
      if (word != 0U) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Whether the set holds at least one index.
   *
   * @return True when not empty.
   */
  [[nodiscard]] bool any() const noexcept { return !empty(); }

  /**
   * @brief Whether index is a member.
   *
   * @param[in] index Argument slot index (unbounded, 0-based).
   * @return True when `index` was inserted; false for indices beyond the
   *         stored words (never-inserted indices are simply absent).
   */
  [[nodiscard]] bool contains(std::size_t index) const noexcept {
    const std::size_t word = index / 64U;
    if (word >= words_.size()) {
      return false;
    }
    return (words_[word] & (std::uint64_t{1} << (index % 64U))) != 0U;
  }

  /**
   * @brief Add index to the set (idempotent).
   *
   * @param[in] index Argument slot index (unbounded, 0-based).
   * @note Grows the word storage when `index` lands beyond the current
   *       width; the set stays trimmed afterwards.
   */
  void insert(std::size_t index) {
    const std::size_t word = index / 64U;
    if (word >= words_.size()) {
      words_.resize(word + 1U, 0U);
    }
    words_[word] |= std::uint64_t{1} << (index % 64U);
    trim();
  }

  /**
   * @brief Value equality.
   *
   * @param[in] rhs Other set.
   * @return True when both sets hold the same indices (both sides stay
   *         trimmed, so word count never matters).
   */
  [[nodiscard]] bool operator==(const ArgSet &rhs) const noexcept {
    return words_ == rhs.words_; // both sides stay trimmed
  }

  /**
   * @brief Value inequality.
   *
   * @param[in] rhs Other set.
   * @return True when the sets differ.
   */
  [[nodiscard]] bool operator!=(const ArgSet &rhs) const noexcept {
    return !(*this == rhs);
  }

  /**
   * @brief Union.
   *
   * @param[in] lhs Left set (taken by value; the result is built in it).
   * @param[in] rhs Right set.
   * @return Every index held by either set.
   */
  friend ArgSet operator|(ArgSet lhs, const ArgSet &rhs) {
    lhs |= rhs;
    return lhs;
  }

  /**
   * @brief Intersection.
   *
   * @param[in] lhs Left set.
   * @param[in] rhs Right set.
   * @return The indices held by both sets.
   */
  friend ArgSet operator&(const ArgSet &lhs, const ArgSet &rhs) {
    ArgSet out;
    const std::size_t n = lhs.words_.size() < rhs.words_.size() ? lhs.words_.size() : rhs.words_.size();
    out.words_.resize(n, 0U);
    for (std::size_t i = 0; i < n; ++i) {
      out.words_[i] = lhs.words_[i] & rhs.words_[i];
    }
    out.trim();
    return out;
  }

  /**
   * @brief Set difference.
   *
   * @param[in] lhs Left set.
   * @param[in] rhs Right set.
   * @return The indices held by `lhs` but not by `rhs`.
   * @note Replaces `lhs & ~rhs`, which has no meaning without a fixed
   *       word width: there is no complement of an unbounded index set.
   */
  friend ArgSet operator-(const ArgSet &lhs, const ArgSet &rhs) {
    ArgSet out = lhs;
    out -= rhs;
    return out;
  }

  /**
   * @brief In-place union.
   *
   * @param[in] rhs Other set.
   * @return Reference to this set.
   * @note Grows to the wider of the two word counts, then trims.
   */
  ArgSet &operator|=(const ArgSet &rhs) {
    if (words_.size() < rhs.words_.size()) {
      words_.resize(rhs.words_.size(), 0U);
    }
    for (std::size_t i = 0; i < rhs.words_.size(); ++i) {
      words_[i] |= rhs.words_[i];
    }
    trim();
    return *this;
  }

  /**
   * @brief In-place intersection.
   *
   * @param[in] rhs Other set.
   * @return Reference to this set.
   * @note Truncates to the narrower word count, then trims.
   */
  ArgSet &operator&=(const ArgSet &rhs) {
    const std::size_t n = words_.size() < rhs.words_.size() ? words_.size() : rhs.words_.size();
    words_.resize(n, 0U);
    for (std::size_t i = 0; i < n; ++i) {
      words_[i] &= rhs.words_[i];
    }
    trim();
    return *this;
  }

  /**
   * @brief In-place set difference.
   *
   * @param[in] rhs Other set.
   * @return Reference to this set (holding the indices not in `rhs`).
   */
  ArgSet &operator-=(const ArgSet &rhs) {
    const std::size_t n = words_.size() < rhs.words_.size() ? words_.size() : rhs.words_.size();
    for (std::size_t i = 0; i < n; ++i) {
      words_[i] &= ~rhs.words_[i];
    }
    trim();
    return *this;
  }

 private:
  void trim() {
    while (!words_.empty() && words_.back() == 0U) {
      words_.pop_back();
    }
  }

  std::vector<std::uint64_t> words_;
};

} // namespace wengine::tui
