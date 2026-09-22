#pragma once

#include "wengine/cli/args.hpp"
#include "wengine/cli/command.hpp"

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

struct OhmSpec {
  static constexpr std::string_view kName = "ohm";
  static constexpr std::string_view kDescription = "Solve resistor (Ohm's law)";
  static constexpr std::string_view kUsage =
      "ohm [U] [I]\n"
      "  ohm 12.4V 0.54A\n"
      "  ohm --u 12.4V --i 0.54A\n"
      "  ohm --u 12V --r 4Ohm      (solve from U+R)\n"
      "  ohm --i 2A --r 5Ohm       (solve from I+R)\n"
      "  ohm --help";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct S0Spec {
  static constexpr std::string_view kName = "s0";
  static constexpr std::string_view kDescription = "S0 pulse output (energy meter imp/kWh)";
  static constexpr std::string_view kUsage =
      "s0 <mode> [options]   (C = meter constant in imp/kWh)\n"
      "  s0 convert --const 1000imp/kWh\n"
      "  s0 rate --power 2kW --const 1000imp/kWh\n"
      "  s0 power --freq 0.555Hz --const 1000imp/kWh\n"
      "  s0 energy --pulses 500 --const 1000imp/kWh\n"
      "  s0 pulses --power 2kW --time 1h --const 1000imp/kWh\n"
      "  s0 pulses --energy 2kWh --const 1000imp/kWh\n"
      "  s0 full --u 230V --i 10A [--pf 1.0] --const 1000imp/kWh [--time 1h]\n"
      "Modes can also be the first positional: s0 rate 2kW ...\n"
      "Units: W/kW, J/Ws/kJ/Wh/kWh, Hz/kHz, s/ms/min/h,\n"
      "  imp/kWh (also imp/Wh, imp/J == imp/Ws), pulses as bare/imp.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct I2cSpec {
  static constexpr std::string_view kName = "i2c";
  static constexpr std::string_view kDescription = "I2C bus resistors (pull-up Rp + series Rs)";
  static constexpr std::string_view kUsage =
      "i2c <mode> --vdd <V> --cb <F> [options]   (NXP UM10204 section 7.1)\n"
      "  i2c pullup --vdd 3.3V --cb 200pF [--speed fast] [--rp 1.5kOhm]\n"
      "  i2c series --vdd 3.3V --cb 200pF --rp 1.5kOhm [--rs 100Ohm]\n"
      "  i2c full   --vdd 3.3V --cb 200pF [--rp 1.5kOhm] [--rs 100Ohm]\n"
      "Speeds: std (100 kHz), fast (400 kHz, default), fmp (1 MHz).\n"
      "Overrides: --vol <V>, --iol <A>, --tr <s>. Hs-mode not sized\n"
      "(UM10204 section 5 uses current-source pull-ups, not resistors).\n"
      "Units: V, F/pF/nF/uF, s/ns/us, A/mA, Ohm/kOhm.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct SimSpec {
  static constexpr std::string_view kName = "sim";
  static constexpr std::string_view kDescription = "Run stepped simulation (engine vertical slice)";
  static constexpr std::string_view kUsage =
      "sim run [--steps N] [--dt <s>] [--gravity <m/s^2>] [--particles N]\n"
      "        [--x0 X] [--y0 Y] [--vx0 VX] [--vy0 VY]\n"
      "        [--emit-csv <path>] [--emit-every N]\n"
      "  sim run --steps 100 --particles 1   (projectile demo, all params named)\n"
      "Defaults: steps=100, dt=0.01s, gravity=9.81, particles=1,\n"
      "  x0=y0=0, vx0=vy0=10, emit-every=1. Time unit: s/ms/us/ns.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::commands
