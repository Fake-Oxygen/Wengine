#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/batches/types.hpp"

namespace wengine::tui::batches::definitions {

[[nodiscard]] inline BatchDef sig() {
  using cli::commands::SigSpec;

  const auto bit = [](unsigned index) { return std::uint64_t{1} << index; };
  const auto argument = [&](std::size_t field,
                            std::uint64_t requirements = 0U) {
    return BatchArgument{field, bit(static_cast<unsigned>(field)), requirements};
  };
  const auto profile = [](std::string_view kind, std::uint64_t active,
                          std::uint64_t required, std::uint64_t defaults,
                          bool supported = true, bool fallback = false) {
    return SignalProfile{kind, active, required, defaults, supported, fallback};
  };
  // Runtime fields (steps/dt/emit/control/pacing) are shared by every kind;
  // signal fields (freq/amp/.../lua) vary per kind. Field indices must match
  // the `fields` order below (bit N == field N).
  const std::uint64_t kRuntimeMask = bit(38) - bit(12);
  const std::uint64_t kRuntimeDefault = bit(12) | bit(13);
  const std::uint64_t kPeriodicMask = bit(1) | bit(2) | bit(3) | bit(4) |
                                      bit(7) | bit(8);
  const std::uint64_t kAllSignalMask = bit(12) - bit(1);

  return {
      BatchKind::kSig,
      SigSpec::kName,
      "run",
      SigSpec::kDescription,
      SigSpec::kUsage,
      {
          {"--sig-kind", "kind", "sine|square|triangle|saw|dc|pulse|noise|sum|expression|table|lua"},
          {"--sig-freq", "freq", "e.g. 1kHz (periodic kinds)"},
          {"--sig-amp", "amp", "e.g. 3.3V / 20mA (per quantity)"},
          {"--sig-offset", "offset", "e.g. 0V"},
          {"--sig-phase", "phase", "radians, e.g. 0"},
          {"--sig-duty", "duty", "0..1 (square/pulse)"},
          {"--sig-seed", "seed", "e.g. 1 (noise)"},
          {"--sig-quantity", "quantity", "U|I"},
          {"--sig-count", "count", "identical sources, e.g. 1"},
          {"--sig-expr", "expr", "math over t, e.g. sin(2*pi*440*t)*0.5"},
          {"--sig-table", "table file", "CSV waveform (time,value)"},
          {"--sig-lua", "lua file", "script path (pure fn of t/seed/step/dt)"},
          {"--steps", "steps", "e.g. 1000 (ignored with --forever)"},
          {"--dt", "dt", "e.g. 0.1ms"},
          {"--emit-every", "emit-every", "global gate, e.g. 1"},
          {"--emit-csv", "csv file", "path, e.g. sig.csv"},
          {"--emit-csv-stride", "csv stride", "0=muted, N>=1"},
          {"--emit-csv-select", "csv select", "serials e.g. 0,2"},
          {"--emit-shm", "shm seg", "e.g. /wengine-sig"},
          {"--emit-shm-stride", "shm stride", "0=muted, N>=1"},
          {"--emit-shm-select", "shm select", "serials e.g. 0,2"},
          {"--emit-tcp", "tcp out", "host:port"},
          {"--emit-tcp-stride", "tcp stride", "0=muted, N>=1"},
          {"--emit-tcp-select", "tcp select", "serials e.g. 0,2"},
          {"--emit-udp", "udp out", "host:port"},
          {"--emit-udp-stride", "udp stride", "0=muted, N>=1"},
          {"--emit-udp-select", "udp select", "serials e.g. 0,2"},
          {"--emit-udp-budget", "udp budget", "bytes 1..65507"},
          {"--emit-uart", "uart dev", "device[,baud]"},
          {"--emit-uart-mode", "uart mode", "ascii|cobs"},
          {"--emit-uart-stride", "uart stride", "0=muted, N>=1"},
          {"--emit-uart-select", "uart select", "serials e.g. 0,2"},
          {"--control-tcp", "control", "host:port (step-sync server)"},
          {"--control-uart", "ctl-uart", "flag: 1 (needs --emit-uart)", true},
          {"--paced", "paced", "flag: 1 to enable wall-clock pacing", true},
          {"--rate", "rate", "wall steps/s, e.g. 1kHz (needs --paced)"},
          {"--forever", "forever", "flag: 1 runs until Ctrl-C (async, TUI-5)", true},
          {"--emit-health", "health", "flag: 1 for live-output report", true},
      },
      {
          argument(1), argument(2), argument(3), argument(4), argument(5),
          argument(6), argument(7), argument(8), argument(9), argument(10),
          argument(11), argument(12), argument(13), argument(14), argument(15),
          argument(16, bit(15)), argument(17, bit(15)), argument(18),
          argument(19, bit(18)), argument(20, bit(18)), argument(21),
          argument(22, bit(21)), argument(23, bit(21)), argument(24),
          argument(25, bit(24)), argument(26, bit(24)), argument(27, bit(24)),
          argument(28), argument(29, bit(28)), argument(30, bit(28)),
          argument(31, bit(28)), argument(32), argument(33, bit(28)),
          argument(34), argument(35, bit(34)), argument(36), argument(37),
      },
      {
          profile("sine", kRuntimeMask | kPeriodicMask, 0U,
                  kRuntimeDefault | bit(1) | bit(2)),
          profile("square", kRuntimeMask | kPeriodicMask | bit(5), 0U,
                  kRuntimeDefault | bit(1) | bit(2)),
          profile("triangle", kRuntimeMask | kPeriodicMask, 0U,
                  kRuntimeDefault | bit(1) | bit(2)),
          profile("saw", kRuntimeMask | kPeriodicMask, 0U,
                  kRuntimeDefault | bit(1) | bit(2)),
          profile("pulse", kRuntimeMask | kPeriodicMask | bit(5), 0U,
                  kRuntimeDefault | bit(1) | bit(2)),
          profile("noise", kRuntimeMask | bit(2) | bit(3) | bit(6) | bit(7) | bit(8),
                  0U, kRuntimeDefault | bit(2)),
          profile("dc", kRuntimeMask | bit(3) | bit(7) | bit(8), 0U,
                  kRuntimeDefault),
          profile("expression", kRuntimeMask | bit(3) | bit(7) | bit(8) | bit(9),
                  bit(9), kRuntimeDefault | bit(9)),
          profile("table", kRuntimeMask | bit(3) | bit(7) | bit(8) | bit(10),
                  bit(10), kRuntimeDefault | bit(10)),
          profile("lua", kRuntimeMask | bit(3) | bit(6) | bit(7) | bit(8) | bit(11),
                  bit(11), kRuntimeDefault | bit(11)),
          profile("sum", kRuntimeMask, 0U, kRuntimeDefault, false),
          profile("other", kRuntimeMask | kAllSignalMask, 0U,
                  kRuntimeDefault, true, true),
      },
  };
}

} // namespace wengine::tui::batches::definitions
