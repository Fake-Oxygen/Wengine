#pragma once

// Optimizer option vocabulary (levels, targets, parse). Extracted from
// optimize.hpp so pass/emit/cleanup headers stay standalone without a
// circular include.

#include "wengine/core/status.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::logic {

enum class OptLevel : std::uint8_t {
  kO0 = 0,
  kO1 = 1,
  kO2 = 2,
};

enum class OptTarget : std::uint8_t {
  kNand = 0,
  kNor = 1,
};

struct OptimizeOptions {
  OptLevel level = OptLevel::kO0;
  OptTarget target = OptTarget::kNand; // read only at O2
};

// Case-insensitive "O0|O1|O2" (exactly two characters).
[[nodiscard]] inline core::Result<OptLevel> parse_opt_level(std::string_view text) noexcept {
  if (text.size() == 2 && (text[0] == 'O' || text[0] == 'o') && text[1] >= '0' && text[1] <= '2') {
    const auto v = static_cast<std::uint8_t>(text[1] - '0');
    return core::Result<OptLevel>::success(static_cast<OptLevel>(v));
  }
  return core::Result<OptLevel>::failure("bad opt level: want O0|O1|O2");
}

// Case-insensitive "nand"|"nor".
[[nodiscard]] inline core::Result<OptTarget> parse_opt_target(std::string_view text) noexcept {
  if (text.size() == 4) {
    const bool nand = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'A' || text[1] == 'a') &&
                      (text[2] == 'N' || text[2] == 'n') && (text[3] == 'D' || text[3] == 'd');
    if (nand) {
      return core::Result<OptTarget>::success(OptTarget::kNand);
    }
  } else if (text.size() == 3) {
    const bool nor = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'O' || text[1] == 'o') && (text[2] == 'R' || text[2] == 'r');
    if (nor) {
      return core::Result<OptTarget>::success(OptTarget::kNor);
    }
  }
  return core::Result<OptTarget>::failure("bad opt target: want nand|nor");
}

} // namespace wengine::logic
