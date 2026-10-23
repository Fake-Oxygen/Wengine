#pragma once

#include "wengine/cli/commands.hpp"
#include "wengine/tui/batches/types.hpp"

namespace wengine::tui::batches::definitions {

[[nodiscard]] inline BatchDef sim() {
  using cli::commands::SimSpec;

  const auto bit = [](unsigned index) { return ArgSet::single(index); };
  const auto argument = [&](std::size_t field, ArgSet requirements = ArgSet{}) {
    return BatchArgument{field, bit(static_cast<unsigned>(field)), requirements};
  };
  const auto profile = [](std::string_view kind, const ArgSet &active,
                          const ArgSet &required, const ArgSet &defaults,
                          bool supported = true, bool fallback = false) {
    return BatchProfile{kind, active, required, defaults, supported, fallback};
  };
  // Field indices must match the `fields` order below (bit N == field N).
  // --solver (field 0) is the profile selector and stays a plain field.
  // --gravity is projectile-only; UART + all -select options need the
  // signal solver; strides/modes/budgets need their output; --rate needs
  // --paced. ROS topics ride both solvers (particles for projectile,
  // signals + particles for signal); only --emit-ros-select is
  // signal-only. Nothing is strictly required: every option has a CLI default.
  const ArgSet kSteps = bit(1) | bit(2);
  const ArgSet kGravity = bit(3);
  const ArgSet kParticles = bit(4) | bit(5) | bit(6) | bit(7) | bit(8);
  const ArgSet kEmitEvery = bit(9);
  const ArgSet kCsv = bit(10);
  const ArgSet kCsvOpts = bit(11);
  const ArgSet kShm = bit(13);
  const ArgSet kShmOpts = bit(14);
  const ArgSet kTcp = bit(16);
  const ArgSet kTcpOpts = bit(17);
  const ArgSet kUdp = bit(19);
  const ArgSet kUdpOpts = bit(20) | bit(22);
  const ArgSet kRos = bit(30);
  const ArgSet kRosOpts = bit(31) | bit(33) | bit(34) | bit(35) | bit(36) |
                          bit(37) | bit(38) | bit(39);
  const ArgSet kPaced = bit(27);
  const ArgSet kHealth = bit(29);
  const ArgSet kProjectileActive =
      kSteps | kGravity | kParticles | kEmitEvery | kCsv | kCsvOpts | kShm |
      kShmOpts | kTcp | kTcpOpts | kUdp | kUdpOpts | kRos | kRosOpts | kPaced | bit(28) | kHealth;
  const ArgSet kSignalActive =
      kSteps | kParticles | kEmitEvery | bit(10) | bit(11) | bit(12) | bit(13) |
      bit(14) | bit(15) | bit(16) | bit(17) | bit(18) | bit(19) | bit(20) |
      bit(21) | bit(22) | bit(23) | bit(24) | bit(25) | bit(26) | bit(30) |
      bit(31) | bit(32) | bit(33) | bit(34) | bit(35) | bit(36) | bit(37) |
      bit(38) | bit(39) | kPaced |
      bit(28) | kHealth;
  const ArgSet kAllSimMask = ArgSet::below(40) - ArgSet::below(1);
  const ArgSet kDefaultSelected = bit(1) | bit(4);

  return {
      BatchKind::kSim,
      SimSpec::kName,
      "run",
      SimSpec::kDescription,
      SimSpec::kUsage,
      {
          {"--solver", "solver", "Space: choose registry solver"},
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
          {"--emit-uart-mode", "uart mode", "Space: choose ascii|cobs", false, {"ascii", "cobs"}},
          {"--emit-uart-stride", "uart stride", "0=muted, N>=1"},
          {"--emit-uart-select", "uart select", "serials e.g. 0 (signal only)"},
          {"--paced", "paced", "flag: 1 to enable wall-clock pacing", true},
          {"--rate", "rate", "wall steps/s, e.g. 1kHz (needs --paced)"},
          {"--emit-health", "health", "flag: 1 for live-output report", true},
          {"--emit-ros", "ros out", "flag: 1 to publish to ROS2 topics", true},
          {"--emit-ros-stride", "ros stride", "0=muted, N>=1"},
          {"--emit-ros-select", "ros select", "serials e.g. 0 (signal only)"},
          {"--ros-node", "ros node", "node name, e.g. wengine"},
          {"--ros-ns", "ros ns", "topic namespace, e.g. wengine"},
          {"--ros-signal-stem", "ros sig stem", "e.g. signals"},
          {"--ros-particle-stem", "ros pts stem", "e.g. particles"},
          {"--ros-qos", "ros qos", "Space: choose latest|reliable", false, {"latest", "reliable"}},
          {"--ros-grouped", "ros grouped", "flag: 1 (default layout)", true},
          {"--ros-per-serial", "ros per-serial", "flag: 1 (one topic per serial)", true},
      },
      {
          argument(1), argument(2), argument(3), argument(4), argument(5),
          argument(6), argument(7), argument(8), argument(9), argument(10),
          argument(11, bit(10)), argument(12, bit(10)), argument(13),
          argument(14, bit(13)), argument(15, bit(13)), argument(16),
          argument(17, bit(16)), argument(18, bit(16)), argument(19),
          argument(20, bit(19)), argument(21, bit(19)), argument(22, bit(19)),
          argument(23), argument(24, bit(23)), argument(25, bit(23)),
          argument(26, bit(23)), argument(27), argument(28, bit(27)),
          argument(29), argument(30), argument(31, bit(30)),
          argument(32, bit(30)), argument(33, bit(30)), argument(34, bit(30)),
          argument(35, bit(30)), argument(36, bit(30)), argument(37, bit(30)),
          argument(38, bit(30)), argument(39, bit(30)),
      },
      {
          profile("projectile", kProjectileActive, ArgSet{}, kDefaultSelected),
          profile("signal", kSignalActive, ArgSet{}, kDefaultSelected),
          profile("other", kAllSimMask, ArgSet{}, kDefaultSelected, true, true),
      },
  };
}

} // namespace wengine::tui::batches::definitions
