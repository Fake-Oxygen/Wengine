#pragma once

// Dynamically-sized set of argument indices for guided TUI builders.
//
// The forms and run builders used to track selected/allowed arguments in
// a std::uint64_t bitmask, which capped every form at 64 arguments: the
// `ic` form alone needs 28 for two chips and 41 more for a third, so the
// cap had to go. Indices are unbounded now — registering new chips or
// options never revisits this header.
//
// Vocabulary (kept deliberately close to the old one so definitions read
// the same): `bit(i)` denotes the singleton {i}; `|`/`&` are union and
// intersection; `-` is set difference. There is no complement operator:
// live selections only ever hold indices from their own form, so every
// former `selected & ~allowed` is spelled `selected - allowed`.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::tui {

class ArgSet {
 public:
  ArgSet() = default;

  // Singleton {index}, spelling the old bit(i).
  [[nodiscard]] static ArgSet single(std::size_t index) {
    ArgSet out;
    out.insert(index);
    return out;
  }

  // Range {0, ..., count-1}, spelling the old bit(count)-1.
  [[nodiscard]] static ArgSet below(std::size_t count) {
    ArgSet out;
    for (std::size_t i = 0; i < count; ++i) {
      out.insert(i);
    }
    return out;
  }

  [[nodiscard]] bool empty() const noexcept {
    for (std::uint64_t word : words_) {
      if (word != 0U) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool any() const noexcept { return !empty(); }

  [[nodiscard]] bool contains(std::size_t index) const noexcept {
    const std::size_t word = index / 64U;
    if (word >= words_.size()) {
      return false;
    }
    return (words_[word] & (std::uint64_t{1} << (index % 64U))) != 0U;
  }

  void insert(std::size_t index) {
    const std::size_t word = index / 64U;
    if (word >= words_.size()) {
      words_.resize(word + 1U, 0U);
    }
    words_[word] |= std::uint64_t{1} << (index % 64U);
    trim();
  }

  [[nodiscard]] bool operator==(const ArgSet &rhs) const noexcept {
    return words_ == rhs.words_; // both sides stay trimmed
  }

  [[nodiscard]] bool operator!=(const ArgSet &rhs) const noexcept {
    return !(*this == rhs);
  }

  friend ArgSet operator|(ArgSet lhs, const ArgSet &rhs) {
    lhs |= rhs;
    return lhs;
  }

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

  // Set difference (replaces `lhs & ~rhs`, which has no meaning without
  // a fixed word width).
  friend ArgSet operator-(const ArgSet &lhs, const ArgSet &rhs) {
    ArgSet out = lhs;
    out -= rhs;
    return out;
  }

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

  ArgSet &operator&=(const ArgSet &rhs) {
    const std::size_t n = words_.size() < rhs.words_.size() ? words_.size() : rhs.words_.size();
    words_.resize(n, 0U);
    for (std::size_t i = 0; i < n; ++i) {
      words_[i] &= rhs.words_[i];
    }
    trim();
    return *this;
  }

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
