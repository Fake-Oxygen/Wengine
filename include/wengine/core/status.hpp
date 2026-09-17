#pragma once

namespace wengine::core {

// Lightweight status for hot paths (no exceptions, no strings).
enum class StatusCode : int {
  kOk = 0,
  kInvalidArgument = 1,
  kOutOfRange = 2,
  kNotImplemented = 3,
  kInternal = 4,
};

struct Status {
  StatusCode code{StatusCode::kOk};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == StatusCode::kOk;
  }
  [[nodiscard]] static constexpr Status success() noexcept { return {}; }
};

} // namespace wengine::core
