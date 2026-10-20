#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/batches/types.hpp"

namespace wengine::tui::batches::definitions {

[[nodiscard]] inline BatchDef sim() {
  using cli::commands::SimSpec;

  return {
      BatchKind::kSim,
      SimSpec::kName,
      "run",
      SimSpec::kDescription,
      SimSpec::kUsage,
      {
          {"--solver", "solver", "registry: projectile|signal"},
          {"--steps", "steps", "e.g. 100"},
          {"--dt", "dt", "e.g. 0.01s / 10ms"},
          {"--gravity", "gravity", "m/s^2, projectile only"},
          {"--particles", "particles", "e.g. 1"},
          {"--x0", "x0", "e.g. 0"},
          {"--y0", "y0", "e.g. 0"},
          {"--vx0", "vx0", "e.g. 10"},
          {"--vy0", "vy0", "e.g. 10"},
          {"--emit-every", "emit-every", "global gate, e.g. 1"},
          {"--emit-csv", "csv file", "path, e.g. out.csv"},
          {"--emit-csv-stride", "csv stride", "0=muted, N>=1"},
          {"--emit-csv-select", "csv select", "serials e.g. 0 (signal only)"},
          {"--emit-shm", "shm seg", "e.g. /wengine-sim"},
          {"--emit-shm-stride", "shm stride", "0=muted, N>=1"},
          {"--emit-shm-select", "shm select", "serials e.g. 0 (signal only)"},
          {"--emit-tcp", "tcp out", "host:port"},
          {"--emit-tcp-stride", "tcp stride", "0=muted, N>=1"},
          {"--emit-tcp-select", "tcp select", "serials e.g. 0 (signal only)"},
          {"--emit-udp", "udp out", "host:port"},
          {"--emit-udp-stride", "udp stride", "0=muted, N>=1"},
          {"--emit-udp-select", "udp select", "serials e.g. 0 (signal only)"},
          {"--emit-udp-budget", "udp budget", "bytes 1..65507"},
          {"--emit-uart", "uart dev", "device[,baud] (signal only)"},
          {"--emit-uart-mode", "uart mode", "ascii|cobs"},
          {"--emit-uart-stride", "uart stride", "0=muted, N>=1"},
          {"--emit-uart-select", "uart select", "serials e.g. 0 (signal only)"},
          {"--paced", "paced", "flag: 1 to enable wall-clock pacing", true},
          {"--rate", "rate", "wall steps/s, e.g. 1kHz (needs --paced)"},
          {"--emit-health", "health", "flag: 1 for live-output report", true},
      },
      {},
      {},
  };
}

} // namespace wengine::tui::batches::definitions
