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
      "Signal sources: sine/square/triangle/saw/dc/pulse/noise (stamped U/I)";

  static std::unique_ptr<core::ISolver> create() {
    return std::make_unique<SignalSolver>();
  }

  [[nodiscard]] std::string_view name() const noexcept override { return kName; }

  core::Status step(world::State &s, double dt) noexcept override {
    if (!(dt > 0.0)) {
      return {core::StatusCode::kInvalidArgument};
    }
    const double t_end = s.sim_time + dt;
    for (std::uint32_t i = 0; i < s.signals.capacity(); ++i) {
      if (!s.signals.has(i) || s.signals.running[i] == 0) {
        continue;
      }
      signal::SignalParams p;
      p.kind = static_cast<signal::SignalKind>(s.signals.kind[i]);
      p.quantity = static_cast<signal::QuantityId>(s.signals.quantity[i]);
      p.freq_hz = s.signals.freq[i];
      p.amp = s.signals.amp[i];
      p.offset = s.signals.offset[i];
      p.phase_rad = s.signals.phase[i];
      p.duty = s.signals.duty[i];
      p.seed = s.signals.seed[i];
      double u01 = 0.0;
      if (p.kind == signal::SignalKind::kNoise) {
        s.signals.rng[i] = signal::rng_next(s.signals.rng[i]);
        u01 = signal::u01_from_bits(s.signals.rng[i]);
      }
      s.signals.value[i] = signal::eval_signal(p, t_end - s.signals.t0[i], u01);
    }
    return core::Status::success();
  }
};

} // namespace wengine::solvers
