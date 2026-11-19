#pragma once

/**
 * @file
 * Signal source kinds, quantity tags, parameters, and pure evaluation.
 * Owns the kQuantities single table (suffix, human name, accepted
 * spellings, units-owned scales), the DimensionFor tag-to-Dimension
 * trait, and the StampedValue typed-sample variant. Depends only on
 * units and core/status leaf types; never on world, emit, or the CLI.
 * Consumed by the signal solver, the control plane, CLI parse edges,
 * emit signal sinks, and link Frame adapters.
 */
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

/**
 * @brief Signal waveform source kinds (parametric, combinator, stored-program).
 * @note APPEND-ONLY: kind bytes persist in pool state, snapshots, and the
 * wire, so new kinds go last. kSum, kExpression, kTable, and kLua are
 * solver-only: direct eval_signal evaluation is the identity and the
 * owning solver branches explicitly.
 */
enum class SignalKind : std::uint8_t {
/** @brief Sine generator: offset + amp * sin(2*pi*f*t + phase). */
  kSine = 0,
/** @brief Square generator: +/-amp about offset, high while phase01 < duty. */
  kSquare = 1,
/** @brief Triangle generator: linear +/-amp ramps about offset. */
  kTriangle = 2,
/** @brief Sawtooth generator: -amp..+amp ramp about offset. */
  kSaw = 3,
/** @brief Constant generator: always offset; ignores amp and phase. */
  kDc = 4,
/** @brief Pulse generator: offset + amp while phase01 < duty, else offset. */
  kPulse = 5,
/** @brief Seeded white-noise generator: offset + amp * (2*u01 - 1). */
  kNoise = 6,
/** @brief Combinator: sums live operand sources; solver-only kind. */
  kSum = 7, // combinator: sums operand sources (SIG-5); solver-only kind
/** @brief Closed-form custom math (RPN program); solver-only evaluation. */
  kExpression = 8, // closed-form custom math (SIG-4a); solver-only evaluation
/** @brief CSV waveform replay with linear interpolation; solver-only evaluation. */
  kTable = 9, // CSV waveform replay (SIG-4a); solver-only evaluation
/** @brief Lua scripting source; evaluated by the Lua extra solver. */
  kLua = 10, // Lua scripting source (SIG-4b); evaluated by the Lua extra solver
};

// Canonical kind names for CLI/GUI/control-plane vocabularies (single
// source of truth; `parse_kind` accepts exactly these). APPEND-ONLY:
// serialized kind bytes must stay stable, so new kinds go last.
/**
 * @brief Canonical kind names for CLI, GUI, and control-plane vocabularies.
 * @note Single source of truth: parse_kind accepts exactly these spellings.
 */
inline constexpr std::array<std::string_view, 11> kKindNames{
    {"sine", "square", "triangle", "saw", "dc", "pulse", "noise", "sum",
     "expression", "table", "lua"}};

// Physical-quantity tag. Stored as `uint8` in pools/snapshots/wire, never
// as strings. U/I first; the mechanism is generic (see kQuantities).
/**
 * @brief Physical-quantity tag carried by every stamped signal sample.
 * @note Stored as uint8 in pools, snapshots, and the wire, never as
 * strings; U/I first, extended one kQuantities row at a time.
 * Values: kVoltage (suffix U, SI volts), kCurrent (suffix I, SI amperes).
 */
enum class QuantityId : std::uint8_t { kVoltage = 0, kCurrent = 1 };

// Single quantity table: every per-quantity fact lives in exactly one row
// (channel/CSV suffix, human name, accepted `--sig-quantity` spellings,
// units-owned suffix scales for parsing). Extension = append one row;
// the `static_assert`s below keep rows, codes, and `DimensionFor` in sync.
/**
 * @brief One row of the kQuantities single table.
 * @note Every per-quantity fact (channel/CSV suffix, human name, accepted
 * spellings, units-owned suffix scales) lives in exactly one row.
 */
struct QuantityDesc {
/** @brief Quantity tag this row describes; rows sit at index id. */
  QuantityId id;
/** @brief Channel and CSV suffix ("U", "I"). */
  std::string_view suffix; // "U" — channel (`sig.0.U`), CSV, console tag
/** @brief Human name for messages and future GUI/control display. */
  std::string_view name; // "voltage" — messages, future GUI/control display
/** @brief Units-owned suffix-to-scale parse table for this dimension. */
  std::span<const units::SuffixScale> scales; // units-owned parse table
/** @brief Accepted --sig-quantity spellings for parse_quantity. */
  std::array<std::string_view, 2> spellings; // accepted --sig-quantity text
};

/**
 * @brief The single quantity table: every per-quantity fact lives here.
 * @note Extension means appending one row; the static_asserts below keep
 * rows, codes, and DimensionFor in sync.
 */
inline constexpr std::array<QuantityDesc, 2> kQuantities{{
    {QuantityId::kVoltage, "U", "voltage", units::scales_for<units::ElectricPotential>::value,
     {"U", "V"}},
    {QuantityId::kCurrent, "I", "current", units::scales_for<units::ElectricCurrent>::value,
     {"I", "A"}},
}};

/** @brief Number of rows in kQuantities. */
inline constexpr std::size_t kQuantityCount = kQuantities.size();

namespace detail {
/**
 * @brief Look up a quantity row by tag.
 * @param[in] q quantity tag to resolve.
 * @return Pointer to the kQuantities row, or nullptr for unknown codes.
 */
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
/**
 * @brief Compile-time tag-to-Dimension binding for typed units work.
 * @note One specialization per quantity, beside the kQuantities table.
 */
struct DimensionFor;
template <>
/** @brief Voltage binds to units::ElectricPotential. */
struct DimensionFor<QuantityId::kVoltage> {
/** @brief Voltage dimension bound to the kVoltage tag. */
  using type = units::ElectricPotential;
};
template <>
/** @brief Current binds to units::ElectricCurrent. */
struct DimensionFor<QuantityId::kCurrent> {
/** @brief Current dimension bound to the kCurrent tag. */
  using type = units::ElectricCurrent;
};
template <QuantityId Q>
/** @brief Shorthand for the Dimension bound to tag Q. */
using DimensionForT = typename DimensionFor<Q>::type;

static_assert(std::is_same_v<DimensionForT<QuantityId::kVoltage>, units::ElectricPotential>);
static_assert(std::is_same_v<DimensionForT<QuantityId::kCurrent>, units::ElectricCurrent>);

// Dimensionally typed sample for boundary code (control plane, display):
// carries the dimension tag AND a matching `Value<Dim>`, so misreading
// volts as amps is a compile-time error wherever this type is used.
/**
 * @brief Dimensionally typed sample for control-plane and display edges.
 * @note Carries the dimension tag AND a matching Value<Dim>, so misreading
 * volts as amps is a compile-time error wherever this type is used.
 */
using StampedValue =
    std::variant<units::Value<units::ElectricPotential>, units::Value<units::ElectricCurrent>>;

/**
 * @brief Build a dimensionally typed sample from a hot-path SI value.
 * @param[in] q quantity tag carried by the source (must be valid).
 * @param[in] value_si sample value in SI base units (V or A per q).
 * @return StampedValue wrapping value_si in the Dimension bound to q.
 */
[[nodiscard]] inline StampedValue stamp(QuantityId q, double value_si) {
  if (q == QuantityId::kCurrent) {
    return StampedValue{units::Value<units::ElectricCurrent>{value_si}};
  }
  return StampedValue{units::Value<units::ElectricPotential>{value_si}};
}

/**
 * @brief Parametric source description: kind, quantity, and numerics.
 * @note Parsed values only (frontends parse text into this); inert fields
 * stay unchecked by validation (dc ignores amp/phase, expression/table/lua
 * carry programs/stores elsewhere). Sum operands are stable source
 * serials, resolved to slots per step by the solver.
 */
struct SignalParams {
/** @brief Waveform kind; solver-only kinds need pool/solver context. */
  SignalKind kind = SignalKind::kSine;
/** @brief Stamped quantity of every sample (SI volts or amperes). */
  QuantityId quantity = QuantityId::kVoltage;
/** @brief Frequency in hertz (periodic kinds only; must be finite and > 0). */
  double freq_hz = 1.0; // periodic kinds only; ignored by dc/noise/sum
/** @brief Peak amplitude in SI (V or A per quantity); scales noise output. */
  double amp = 1.0; // SI peak: V or A per `quantity`
/** @brief Offset in SI base units; stamps the initial sample of every kind. */
  double offset = 0.0; // SI base
/** @brief Phase offset in radians. */
  double phase_rad = 0.0;
/** @brief High fraction in [0,1]; read by square/pulse only. */
  double duty = 0.5; // square/pulse high fraction in [0,1]
/** @brief Noise seed; advanced once per step per live slot in slot order. */
  std::uint64_t seed = 1; // noise only
  // Sum operands: operand source serials (kSum only, 1..kMaxOps). Stored as
  // serials (stable across despawn/respawn), resolved to slots per step.
/** @brief Maximum sum operands per kSum source. */
  static constexpr std::size_t kMaxOps = 8;
/** @brief Operand source serials (kSum only, 1..kMaxOps entries used). */
  std::array<std::uint64_t, kMaxOps> ops{};
/** @brief Number of live operand serials in ops. */
  std::uint8_t op_count = 0;
  // Closed expression program (kExpression only; empty otherwise). Fixed
  // RPN nodes — no allocation, no strings — so per-step copies stay memcpy.
  // Table points (kTable) do NOT live here: variable-length data would
  // allocate on every per-step copy. Tables live in the pool's append-only
  // store (see world::Signals) and in SignalRunConfig::table for batches.
/** @brief Closed RPN program (kExpression only); fixed nodes, no allocation. */
  ExprProgram expr;

/**
 * @brief Field-wise equality.
 * @return true when every field compares equal.
 */
  bool operator==(const SignalParams &) const noexcept = default;
};

/** @brief 2*pi as an exact double literal. @return 6.283185307179586. */
[[nodiscard]] inline constexpr double two_pi() noexcept { return 6.283185307179586; }

// Fractional cycle position in [0,1). `floor` keeps this exact for
// negative phases as well.
/**
 * @brief Fractional cycle position in [0,1) for edges and peaks.
 * @param[in] freq_hz frequency in hertz.
 * @param[in] t_seconds source seconds since start.
 * @param[in] phase_rad phase offset in radians.
 * @return Cycle fraction; floor keeps it exact for negative phases too.
 * @note Integer cycle counting: no unbounded phase accumulator drifts.
 */
[[nodiscard]] inline double phase01(double freq_hz, double t_seconds,
                                    double phase_rad) noexcept {
  const double cycles = freq_hz * t_seconds + phase_rad / two_pi();
  return cycles - std::floor(cycles);
}

// Pure waveform evaluation. `u01` feeds noise only (uniform in [0,1),
// from `u01_from_bits`); periodic kinds ignore it.
/**
 * @brief Pure waveform evaluation at absolute source time t.
 * @param[in] p source parameters (kind selects the branch).
 * @param[in] t_seconds source seconds since start (t = sim_time + dt - t0).
 * @param[in] u01 uniform noise draw in [0,1) from u01_from_bits; ignored by periodic kinds.
 * @return Waveform value in SI (V or A per p.quantity); solver-only kinds return the offset identity.
 * @note Closed form over t: exactly reproducible for fixed inputs.
 */
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
/**
 * @brief Seeded splitmix64 step: takes the pre-increment state, returns the output.
 * @param[in] state RNG state before this step.
 * @return Output word; the caller stores it back as the new state.
 * @note Advanced once per step per live slot in slot order; constexpr with a pinned test vector.
 */
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
/**
 * @brief Map RNG bits to a uniform double in [0,1).
 * @param[in] z raw RNG output word.
 * @return Top 53 bits scaled exactly (inputs below 2^53 convert without rounding).
 */
[[nodiscard]] inline constexpr double u01_from_bits(std::uint64_t z) noexcept {
  return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

// Channel-name suffix for a stamped quantity (`sig.<slot>.U`). Unknown
// codes (corrupt input that slipped validation) render as "?".
/**
 * @brief Channel-name suffix for a stamped quantity.
 * @param[in] q quantity tag to render.
 * @return Suffix from kQuantities ("U"/"I"), or "?" for unknown codes that slipped validation.
 */
[[nodiscard]] inline constexpr std::string_view quantity_suffix(QuantityId q) noexcept {
  const QuantityDesc *d = detail::find_quantity(q);
  return d != nullptr ? d->suffix : "?";
}

// Human name for messages and future GUI/control display.
/**
 * @brief Human name for messages and future GUI/control display.
 * @param[in] q quantity tag to render.
 * @return Name from kQuantities ("voltage"/"current"), or "?" for unknown codes.
 */
[[nodiscard]] inline constexpr std::string_view quantity_name(QuantityId q) noexcept {
  const QuantityDesc *d = detail::find_quantity(q);
  return d != nullptr ? d->name : "?";
}

// Validity gate for QuantityId values arriving as raw bytes (pool stores,
// snapshots, wire) or casts: every stamped sample must resolve in
// kQuantities, so config edges reject the rest loudly instead of
// rendering "?" downstream.
/**
 * @brief Validity gate for QuantityId values arriving as raw bytes.
 * @param[in] q tag from pool stores, snapshots, wire data, or a cast.
 * @return true when q resolves in kQuantities; config edges reject the rest loudly.
 */
[[nodiscard]] inline constexpr bool is_valid_quantity(QuantityId q) noexcept {
  return detail::find_quantity(q) != nullptr;
}

// Dimensional rule for kSum (SIG-5): every operand must carry the same
// quantity as the sum output. Equality only — no per-quantity switch —
// so future kQuantities rows work untouched.
/**
 * @brief Dimensional rule for kSum: every operand must match the sum output.
 * @param[in] sum_q quantity of the sum source.
 * @param[in] operand_qs quantities of the operand sources.
 * @return true when sum_q is valid and every operand equals sum_q.
 * @note Equality only, no per-quantity switch: future kQuantities rows work untouched.
 */
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
/**
 * @brief Parse a kind name from CLI, GUI, or control-plane text.
 * @param[in] s kind spelling (exactly one of kKindNames).
 * @return Success with the kind, or kInvalidArgument ("unknown signal kind") for unknown text.
 */
[[nodiscard]] inline core::Result<SignalKind> parse_kind(std::string_view s) noexcept {
  for (std::size_t k = 0; k < kKindNames.size(); ++k) {
    if (s == kKindNames[k]) {
      return core::Result<SignalKind>::success(static_cast<SignalKind>(k));
    }
  }
  return core::Result<SignalKind>::failure("unknown signal kind");
}

[[nodiscard]] inline core::Result<QuantityId>
/**
 * @brief Parse a quantity tag from accepted spellings.
 * @param[in] s spelling from any kQuantities spellings row.
 * @return Success with the tag, or kInvalidArgument ("unknown quantity (want U|I)") for unknown text.
 * @note Cross-dimension text (e.g. "2mA" as volts) is parsed at display edges via
 * cli::parse_quantity_value and fails loudly; no voltage/current switch lives at call sites.
 */
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
/**
 * @brief Run-scoped section owned by the signal module, aggregated by core::RunConfig.
 * @note One member per solver (never flat per-solver fields): the driver honors
 * this section iff the signal solver is selected and rejects it otherwise.
 * table and lua ride along as bounded blobs (config-edge allocation, copied
 * into the pool store once per run, never on the hot path).
 */
struct SignalRunConfig {
/** @brief Parsed source parameters for the batch. */
  SignalParams params;
/** @brief Number of sources to spawn from params. */
  std::uint64_t count = 1;
/** @brief True when explicitly provided options selected this section. */
  bool set = false;
/** @brief CSV points for kTable batches (empty unless kind is kTable). */
  std::vector<TablePoint> table;
/** @brief Script text for kLua batches (empty unless kind is kLua). */
  std::string lua;
};

// Size cap for one Lua script (pool store, batch section, v5 blob alike):
// bounds memory and keeps corrupt blobs load-rejectable. Scripts are text,
// so 64 KiB is generous (typical sources are a few lines).
/**
 * @brief Size cap in bytes for one Lua script (pool store, batch section, v5 blob).
 * @note Bounds memory and keeps corrupt blobs load-rejectable; scripts are text, so 64 KiB is generous.
 */
inline constexpr std::size_t kMaxLuaScriptBytes = 65536;

// Config validation: periodic kinds need f > 0; duty is checked only for
// the kinds that read it (other kinds ignore it, lenient by design); sums
// need 1..kMaxOps operand serials (existence/kind checked at configure
// time against the live pool, not here); expressions need a structurally
// valid program; tables and Lua pass here structurally (points/scripts are
// validated where they are supplied: configure_table_source /
// lua::configure_lua_source, the batch driver, the GUI controller —
// SignalParams carries neither by design).
/**
 * @brief Config validation for source parameters.
 * @param[in] p parameters to gate (only values evaluation actually reads are checked).
 * @return kOk, or kInvalidArgument for non-positive/non-finite frequency, non-finite
 * active numerics or offset, invalid quantity, out-of-range duty, bad sum arity,
 * an invalid expression program, or an unknown kind enumerator.
 */
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
