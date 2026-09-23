#pragma once

// Signal source kinds, physical-quantity tags, parameters, and pure
// evaluation functions (SIG-2 engine core).
//
// Closed form over absolute source time `t` (seconds since the source
// started, `t = sim_time + dt - t0` at emit): value(t) is exactly
// reproducible for fixed inputs. Phase uses integer cycle counting
// (`phase01`) so square/pulse edges and triangle/saw peaks stay exact
// across `dt` scales; no unbounded phase accumulator exists to drift.
// Noise is seeded splitmix64 advanced once per step in slot order.
//
// Hot path: `noexcept`, no allocation, no strings. Parsing/validation are
// config-edge only. `std::sin` keeps `eval_signal` out of `constexpr`
// (correct, not restrictive); the integer RNG is `constexpr` with a
// `static_assert` on the published splitmix64 vector.

#include "wengine/core/status.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace wengine::signal {

enum class SignalKind : std::uint8_t {
  kSine = 0,
  kSquare = 1,
  kTriangle = 2,
  kSaw = 3,
  kDc = 4,
  kPulse = 5,
  kNoise = 6,
};

// Canonical kind names for CLI/GUI/control-plane vocabularies (single
// source of truth; `parse_kind` accepts exactly these).
inline constexpr std::array<std::string_view, 7> kKindNames{
    {"sine", "square", "triangle", "saw", "dc", "pulse", "noise"}};

// Minimal physical-quantity tag (SIG-2). Display/suffix only on the hot
// path; full `units::Value<Dim>` binding + the one-entry scaling path are
// SIG-3. Stored as `uint8` in pools/snapshots, never as strings.
enum class QuantityId : std::uint8_t { kVoltage = 0, kCurrent = 1 };

struct SignalParams {
  SignalKind kind = SignalKind::kSine;
  QuantityId quantity = QuantityId::kVoltage;
  double freq_hz = 1.0; // periodic kinds only; ignored by dc/noise
  double amp = 1.0; // SI peak: V or A per `quantity`
  double offset = 0.0; // SI base
  double phase_rad = 0.0;
  double duty = 0.5; // square/pulse high fraction in [0,1]
  std::uint64_t seed = 1; // noise only

  bool operator==(const SignalParams &) const noexcept = default;
};

[[nodiscard]] inline constexpr double two_pi() noexcept { return 6.283185307179586; }

// Fractional cycle position in [0,1). `floor` keeps this exact for
// negative phases as well.
[[nodiscard]] inline double phase01(double freq_hz, double t_seconds,
                                    double phase_rad) noexcept {
  const double cycles = freq_hz * t_seconds + phase_rad / two_pi();
  return cycles - std::floor(cycles);
}

// Pure waveform evaluation. `u01` feeds noise only (uniform in [0,1),
// from `u01_from_bits`); periodic kinds ignore it.
[[nodiscard]] inline double eval_signal(const SignalParams &p, double t_seconds,
                                        double u01) noexcept {
  switch (p.kind) {
  case SignalKind::kDc:
    return p.offset;
  case SignalKind::kNoise:
    return p.offset + p.amp * (2.0 * u01 - 1.0);
  case SignalKind::kSine:
    return p.offset + p.amp * std::sin(two_pi() * p.freq_hz * t_seconds + p.phase_rad);
  case SignalKind::kSquare:
  case SignalKind::kPulse:
  case SignalKind::kTriangle:
  case SignalKind::kSaw: {
    const double ph = phase01(p.freq_hz, t_seconds, p.phase_rad);
    switch (p.kind) {
    case SignalKind::kSquare:
      return (ph < p.duty) ? p.offset + p.amp : p.offset - p.amp;
    case SignalKind::kPulse:
      return (ph < p.duty) ? p.offset + p.amp : p.offset;
    case SignalKind::kTriangle:
      return p.offset + p.amp * (1.0 - 4.0 * std::fabs(ph - 0.5));
    default: // kSaw (all kinds covered; no silent fallthrough)
      return p.offset + p.amp * (-1.0 + 2.0 * ph);
    }
  }
  }
  return p.offset; // unreachable: every enumerator handled above
}

// splitmix64 (Steele et al.): takes the pre-increment state, returns the
// output; the caller stores `rng_next(rng)` back as the new state.
[[nodiscard]] inline constexpr std::uint64_t rng_next(std::uint64_t state) noexcept {
  std::uint64_t z = state + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

// Published splitmix64 test vector (seed 0 -> 0xE220A8397B1DCDAF),
// verified against an independent Python implementation 2026-09-23.
// Checked as halves so a single mistyped nibble fails loudly.
static_assert((rng_next(0) >> 32) == 0xE220A839u);
static_assert((rng_next(0) & 0xFFFFFFFFu) == 0x7B1DCDAFu);

// Top 53 bits -> uniform double in [0,1). Exact: inputs below 2^53 convert
// without rounding.
[[nodiscard]] inline constexpr double u01_from_bits(std::uint64_t z) noexcept {
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

// Channel-name suffix for a stamped quantity (`sig.<slot>.U`).
[[nodiscard]] inline constexpr std::string_view quantity_suffix(QuantityId q) noexcept {
  return q == QuantityId::kCurrent ? "I" : "U";
}

// Config-edge parsing (may allocate on failure strings; never on hot path).
[[nodiscard]] inline core::Result<SignalKind> parse_kind(std::string_view s) noexcept {
  for (std::size_t k = 0; k < kKindNames.size(); ++k) {
    if (s == kKindNames[k]) {
      return core::Result<SignalKind>::success(static_cast<SignalKind>(k));
    }
  }
  return core::Result<SignalKind>::failure("unknown signal kind");
}

[[nodiscard]] inline core::Result<QuantityId>
parse_quantity(std::string_view s) noexcept {
  if (s == "U" || s == "V") {
    return core::Result<QuantityId>::success(QuantityId::kVoltage);
  }
  if (s == "I" || s == "A") {
    return core::Result<QuantityId>::success(QuantityId::kCurrent);
  }
  return core::Result<QuantityId>::failure("unknown quantity (want U|I)");
}

// Config validation: periodic kinds need f > 0; duty is checked only for
// the kinds that read it (other kinds ignore it, lenient by design).
[[nodiscard]] inline core::Status validate_params(const SignalParams &p) noexcept {
  switch (p.kind) {
  case SignalKind::kSine:
  case SignalKind::kSquare:
  case SignalKind::kTriangle:
  case SignalKind::kSaw:
  case SignalKind::kPulse:
    if (!(p.freq_hz > 0.0)) {
      return {core::StatusCode::kInvalidArgument};
    }
    break;
  case SignalKind::kDc:
  case SignalKind::kNoise:
    break;
  }
  if ((p.kind == SignalKind::kSquare || p.kind == SignalKind::kPulse) &&
      !(p.duty >= 0.0 && p.duty <= 1.0)) {
    return {core::StatusCode::kInvalidArgument};
  }
  return core::Status::success();
}

} // namespace wengine::signal
