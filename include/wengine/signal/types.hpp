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
// Hot path stores SI `double` + `QuantityId` tag (zero-cost stamping —
// see below). Parsing/validation are config-edge only. `std::sin` keeps
// `eval_signal` out of `constexpr` (correct, not restrictive); the integer
// RNG is `constexpr` with a `static_assert` on the published splitmix64
// vector.
//
// STAMPING MODEL (SIG-3): every sample carries its physical quantity from
// evaluation to display. The pool is homogeneous (plain SI doubles —
// templating it per dimension is impossible), so dimensional safety lives
// at the boundaries: `kQuantities` is the single table mapping each tag to
// its channel suffix, human name, accepted spellings, and units-owned
// suffix scales; `DimensionFor` maps each tag to its `units::Dimension`
// for typed `units::Value<Dim>` work; `stamp()` builds a dimensionally
// typed value for control/display edges. Adding a quantity = one table row
// + one `DimensionFor` line (see "Adding a physical quantity" in
// docs/signals.md); the hot path never changes.

#include "wengine/core/status.hpp"
#include "wengine/signal/expr.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/units/scales.hpp"
#include "wengine/units/units.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace wengine::signal {

enum class SignalKind : std::uint8_t {
  kSine = 0,
  kSquare = 1,
  kTriangle = 2,
  kSaw = 3,
  kDc = 4,
  kPulse = 5,
  kNoise = 6,
  kSum = 7, // combinator: sums operand sources (SIG-5); solver-only kind
  kExpression = 8, // closed-form custom math (SIG-4a); solver-only evaluation
  kTable = 9, // CSV waveform replay (SIG-4a); solver-only evaluation
  kLua = 10, // Lua scripting source (SIG-4b); evaluated by the Lua extra solver
};

// Canonical kind names for CLI/GUI/control-plane vocabularies (single
// source of truth; `parse_kind` accepts exactly these). APPEND-ONLY:
// serialized kind bytes must stay stable, so new kinds go last.
inline constexpr std::array<std::string_view, 11> kKindNames{
    {"sine", "square", "triangle", "saw", "dc", "pulse", "noise", "sum",
     "expression", "table", "lua"}};

// Physical-quantity tag. Stored as `uint8` in pools/snapshots/wire, never
// as strings. U/I first; the mechanism is generic (see kQuantities).
enum class QuantityId : std::uint8_t { kVoltage = 0, kCurrent = 1 };

// Single quantity table: every per-quantity fact lives in exactly one row
// (channel/CSV suffix, human name, accepted `--sig-quantity` spellings,
// units-owned suffix scales for parsing). Extension = append one row;
// the `static_assert`s below keep rows, codes, and `DimensionFor` in sync.
struct QuantityDesc {
  QuantityId id;
  std::string_view suffix; // "U" — channel (`sig.0.U`), CSV, console tag
  std::string_view name; // "voltage" — messages, future GUI/control display
  std::span<const units::SuffixScale> scales; // units-owned parse table
  std::array<std::string_view, 2> spellings; // accepted --sig-quantity text
};

inline constexpr std::array<QuantityDesc, 2> kQuantities{{
    {QuantityId::kVoltage, "U", "voltage", units::scales_for<units::ElectricPotential>::value,
     {"U", "V"}},
    {QuantityId::kCurrent, "I", "current", units::scales_for<units::ElectricCurrent>::value,
     {"I", "A"}},
}};

inline constexpr std::size_t kQuantityCount = kQuantities.size();

namespace detail {
[[nodiscard]] inline constexpr const QuantityDesc *find_quantity(QuantityId q) noexcept {
  const std::size_t i = static_cast<std::size_t>(q);
  return i < kQuantities.size() ? &kQuantities[i] : nullptr;
}
} // namespace detail

static_assert(kQuantities[static_cast<std::size_t>(QuantityId::kVoltage)].id ==
              QuantityId::kVoltage);
static_assert(kQuantities[static_cast<std::size_t>(QuantityId::kCurrent)].id ==
              QuantityId::kCurrent);

// Compile-time tag -> Dimension binding for typed `units::Value<Dim>`
// work at control/display edges. One line per quantity, beside the table.
template <QuantityId Q>
struct DimensionFor;
template <>
struct DimensionFor<QuantityId::kVoltage> {
  using type = units::ElectricPotential;
};
template <>
struct DimensionFor<QuantityId::kCurrent> {
  using type = units::ElectricCurrent;
};
template <QuantityId Q>
using DimensionForT = typename DimensionFor<Q>::type;

static_assert(std::is_same_v<DimensionForT<QuantityId::kVoltage>, units::ElectricPotential>);
static_assert(std::is_same_v<DimensionForT<QuantityId::kCurrent>, units::ElectricCurrent>);

// Dimensionally typed sample for boundary code (control plane, display):
// carries the dimension tag AND a matching `Value<Dim>`, so misreading
// volts as amps is a compile-time error wherever this type is used.
using StampedValue =
    std::variant<units::Value<units::ElectricPotential>, units::Value<units::ElectricCurrent>>;

[[nodiscard]] inline StampedValue stamp(QuantityId q, double si_value) {
  if (q == QuantityId::kCurrent) {
    return StampedValue{units::Value<units::ElectricCurrent>{si_value}};
  }
  return StampedValue{units::Value<units::ElectricPotential>{si_value}};
}

struct SignalParams {
  SignalKind kind = SignalKind::kSine;
  QuantityId quantity = QuantityId::kVoltage;
  double freq_hz = 1.0; // periodic kinds only; ignored by dc/noise/sum
  double amp = 1.0; // SI peak: V or A per `quantity`
  double offset = 0.0; // SI base
  double phase_rad = 0.0;
  double duty = 0.5; // square/pulse high fraction in [0,1]
  std::uint64_t seed = 1; // noise only
  // Sum operands: operand source serials (kSum only, 1..kMaxOps). Stored as
  // serials (stable across despawn/respawn), resolved to slots per step.
  static constexpr std::size_t kMaxOps = 8;
  std::array<std::uint64_t, kMaxOps> ops{};
  std::uint8_t op_count = 0;
  // Closed expression program (kExpression only; empty otherwise). Fixed
  // RPN nodes — no allocation, no strings — so per-step copies stay memcpy.
  // Table points (kTable) do NOT live here: variable-length data would
  // allocate on every per-step copy. Tables live in the pool's append-only
  // store (see world::Signals) and in SignalRunConfig::table for batches.
  ExprProgram expr;

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
  case SignalKind::kSum:
    // Solver-only kind: sums operand sources inside SignalSolver (pool
    // context required). Direct evaluation is the identity by definition.
    return p.offset;
  case SignalKind::kExpression:
  case SignalKind::kTable:
  case SignalKind::kLua:
    // Solver-only kinds like kSum: expression needs fallible RPN eval,
    // table needs pool-store context, Lua needs the scripting runtime — so
    // direct evaluation is the identity and the solvers branch explicitly.
    return p.offset;
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

// Channel-name suffix for a stamped quantity (`sig.<slot>.U`). Unknown
// codes (corrupt input that slipped validation) render as "?".
[[nodiscard]] inline constexpr std::string_view quantity_suffix(QuantityId q) noexcept {
  const QuantityDesc *d = detail::find_quantity(q);
  return d != nullptr ? d->suffix : "?";
}

// Human name for messages and future GUI/control display.
[[nodiscard]] inline constexpr std::string_view quantity_name(QuantityId q) noexcept {
  const QuantityDesc *d = detail::find_quantity(q);
  return d != nullptr ? d->name : "?";
}

// Validity gate for QuantityId values arriving as raw bytes (pool stores,
// snapshots, wire) or casts: every stamped sample must resolve in
// kQuantities, so config edges reject the rest loudly instead of
// rendering "?" downstream.
[[nodiscard]] inline constexpr bool is_valid_quantity(QuantityId q) noexcept {
  return detail::find_quantity(q) != nullptr;
}

// Dimensional rule for kSum (SIG-5): every operand must carry the same
// quantity as the sum output. Equality only — no per-quantity switch —
// so future kQuantities rows work untouched.
[[nodiscard]] inline constexpr bool sum_quantities_compatible(QuantityId sum_q, std::span<const QuantityId> operand_qs) noexcept {
  if (!is_valid_quantity(sum_q)) {
    return false;
  }
  for (QuantityId q : operand_qs) {
    if (!is_valid_quantity(q) || q != sum_q) {
      return false;
    }
  }
  return true;
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
  for (const QuantityDesc &d : kQuantities) {
    for (std::string_view spelling : d.spellings) {
      if (s == spelling) {
        return core::Result<QuantityId>::success(d.id);
      }
    }
  }
  return core::Result<QuantityId>::failure("unknown quantity (want U|I)");
}

// Run-scoped section owned by the signal module and aggregated by
// core::RunConfig (one member per solver — see run_config.hpp). Parsed
// values only (frontends parse kind/quantity/amplitude/expression text and
// store the result here; table points and Lua script text ride along as
// bounded blobs); the driver validates + spawns. `set` marks explicitly
// provided options so the driver can reject them with a non-signal solver
// (same pattern as RunConfig::has_gravity). `table` carries CSV points for
// kTable batches (config-edge allocation, copied into the pool store once
// per run — never on the hot path); `lua` carries the script text for kLua
// batches under the same rule (compiled and evaluated by the Lua extra
// solver, never here). Each must be empty/non-empty exactly for its kind.
struct SignalRunConfig {
  SignalParams params;
  std::uint64_t count = 1;
  bool set = false;
  std::vector<TablePoint> table;
  std::string lua;
};

// Size cap for one Lua script (pool store, batch section, v5 blob alike):
// bounds memory and keeps corrupt blobs load-rejectable. Scripts are text,
// so 64 KiB is generous (typical sources are a few lines).
inline constexpr std::size_t kMaxLuaScriptBytes = 65536;

// Config validation: periodic kinds need f > 0; duty is checked only for
// the kinds that read it (other kinds ignore it, lenient by design); sums
// need 1..kMaxOps operand serials (existence/kind checked at configure
// time against the live pool, not here); expressions need a structurally
// valid program; tables and Lua pass here structurally (points/scripts are
// validated where they are supplied: configure_table_source /
// lua::configure_lua_source, the batch driver, the GUI controller —
// SignalParams carries neither by design).
[[nodiscard]] inline core::Status validate_params(const SignalParams &p) noexcept {
  // Per-kind active numerics must be finite (inert fields are unchecked —
  // dc ignores amp/phase, expression/table/lua carry programs/stores, so
  // only the values evaluation actually reads are gated). Offset stamps
  // the initial sample of every kind, so it is always active.
  switch (p.kind) {
  case SignalKind::kSine:
  case SignalKind::kSquare:
  case SignalKind::kTriangle:
  case SignalKind::kSaw:
  case SignalKind::kPulse:
    if (!(p.freq_hz > 0.0) || !std::isfinite(p.freq_hz)) {
      return {core::StatusCode::kInvalidArgument};
    }
    if (!std::isfinite(p.amp) || !std::isfinite(p.phase_rad)) {
      return {core::StatusCode::kInvalidArgument};
    }
    break;
  case SignalKind::kNoise:
    if (!std::isfinite(p.amp)) {
      return {core::StatusCode::kInvalidArgument};
    }
    break;
  case SignalKind::kDc:
  case SignalKind::kTable:
  case SignalKind::kLua:
    break;
  case SignalKind::kSum:
    if (p.op_count < 1 || p.op_count > SignalParams::kMaxOps) {
      return {core::StatusCode::kInvalidArgument};
    }
    break;
  case SignalKind::kExpression:
    if (!validate_program(p.expr)) {
      return {core::StatusCode::kInvalidArgument};
    }
    break;
  default:
    return {core::StatusCode::kInvalidArgument}; // unknown kind enumerator
  }
  if (!std::isfinite(p.offset)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if (!is_valid_quantity(p.quantity)) {
    return {core::StatusCode::kInvalidArgument};
  }
  if ((p.kind == SignalKind::kSquare || p.kind == SignalKind::kPulse) &&
      !(p.duty >= 0.0 && p.duty <= 1.0)) {
    return {core::StatusCode::kInvalidArgument};
  }
  return core::Status::success();
}

} // namespace wengine::signal
