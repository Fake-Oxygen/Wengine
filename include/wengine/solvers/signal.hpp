#pragma once

// Signal-source stepped solver: evaluates every live `Signals` pool slot
// in slot order (SIG-2 engine core, see docs/signals.md).
//
// End-of-step convention: the value written during a step is value(T)
// where T is the snapshot time the step is about to stamp
// (`state.sim_time + dt`), matching the integrated particle positions.
// Stopped slots are skipped (frozen sample, frozen RNG). Noise slots
// advance their splitmix64 state once per step before sampling, so the
// sequence is a pure function of (seed, step count) — independent of `dt`.
//
// TWO PASSES (SIG-5): generators first, `kSum` combinators second, so sums
// always read fresh operand values regardless of slot order (operands are
// guaranteed non-sum by configure_source). A dangling operand (despawned
// after the sum was configured) fails the step with kInvalidArgument —
// loud and deterministic — instead of reading a stale slot. The failed
// step advances neither time nor index (Runner contract) and emits nothing.

#include "wengine/core/solver.hpp"
#include "wengine/signal/types.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace wengine::solvers {

class SignalSolver : public core::ISolver {
public:
  static constexpr std::string_view kName = "signal";
  static constexpr std::string_view kDescription =
      "Signal sources: sine/square/triangle/saw/dc/pulse/noise/sum (stamped U/I)";

  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<SignalSolver>();
  }

  [[nodiscard]] std::string_view name() const noexcept override { return kName; }

  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0)) {
      return {core::StatusCode::kInvalidArgument};
    }
    const double t_end = s.sim_time + dt;
    // Pass 1: generators (sums read these fresh in pass 2).
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      if (!s.signals.has(i) || s.signals.running[i] == 0) {
        continue;
      }
      if (s.signals.kind[i] == static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
        continue;
      }
      signal::SignalParams p = params_at(s, i);
      double u01 = 0.0;
      if (p.kind == signal::SignalKind::kNoise) {
        s.signals.rng[i] = signal::rng_next(s.signals.rng[i]);
        u01 = signal::u01_from_bits(s.signals.rng[i]);
      }
      s.signals.value[i] = signal::eval_signal(p, t_end - s.signals.t0[i], u01);
    }
    // Pass 2: sums over pass-1-fresh operand values.
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      if (!s.signals.has(i) || s.signals.running[i] == 0) {
        continue;
      }
      if (s.signals.kind[i] != static_cast<std::uint8_t>(signal::SignalKind::kSum)) {
        continue;
      }
      double acc = s.signals.offset[i];
      for (std::uint8_t k = 0; k < s.signals.op_count[i]; ++k) {
        const std::uint64_t want =
            s.signals.ops[i * signal::SignalParams::kMaxOps + k];
        const auto slot = world::find_serial(s.signals, want);
        if (!slot.has_value()) {
          return {core::StatusCode::kInvalidArgument}; // dangling operand
        }
        acc += s.signals.value[*slot];
      }
      s.signals.value[i] = acc;
    }
    return core::Status::success();
  }

private:
  static signal::SignalParams params_at(const world::State &s, std::uint32_t i) noexcept {
    signal::SignalParams p;
    p.kind = static_cast<signal::SignalKind>(s.signals.kind[i]);
    p.quantity = static_cast<signal::QuantityId>(s.signals.quantity[i]);
    p.freq_hz = s.signals.freq[i];
    p.amp = s.signals.amp[i];
    p.offset = s.signals.offset[i];
    p.phase_rad = s.signals.phase[i];
    p.duty = s.signals.duty[i];
    p.seed = s.signals.seed[i];
    return p;
  }
};

} // namespace wengine::solvers
