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
      "sim run [--solver <name>] [--steps N] [--dt <s>] [--gravity <m/s^2>]\n"
      "        [--particles N] [--x0 X] [--y0 Y] [--vx0 VX] [--vy0 VY]\n"
      "        [--emit-csv <path>] [--emit-shm <name>] [--emit-tcp <host:port>]\n"
      "        [--emit-udp <host:port>] [--emit-every N]\n"
      "        [--emit-csv-stride N] [--emit-shm-stride N]\n"
      "        [--emit-tcp-stride N] [--emit-udp-stride N]\n"
      "        [--emit-csv-select <s0,s1>] [--emit-shm-select <s0,s1>]\n"
      "        [--emit-tcp-select <s0,s1>] [--emit-udp-select <s0,s1>]\n"
      "        [--emit-udp-budget BYTES]\n"
      "        [--emit-uart <device[,baud]>] [--emit-uart-mode ascii|cobs]\n"
      "        [--emit-uart-stride N] [--emit-uart-select <s0,s1>]\n"
      "        [--emit-health]\n"
      "        [--paced] [--rate <Hz>]\n"
      "  sim run --steps 100 --particles 1   (projectile demo, all params named)\n"
      "  sim run --steps 100 --paced         (real-time wall-clock pacing)\n"
      "  sim run --steps 1000 --paced --rate 1kHz   (10x real-time at dt=10ms)\n"
      "Solvers come from the compile-time registry (default projectile;\n"
      "  unknown names list the available ones). --gravity applies to the\n"
      "  projectile solver only; particle population is generic setup.\n"
      "  No solver-scoped options beyond --gravity live here: signal sources\n"
      "  are tuned in the sig command (sim run --solver signal runs one\n"
      "  default 1 Hz sine with zero particles). With solver signal,\n"
      "  --emit-csv writes the signals log and --emit-shm publishes signal\n"
      "  channels (sig.<slot>.U). --emit-tcp/--emit-udp stream the same\n"
      "  frames over sockets (TCP: length-prefixed LinkFrames after a\n"
      "  version/domain hello; UDP: compact datagrams); drops are counted,\n"
      "  never blocking (see docs/signals.md sec. 5).\n"
      "Per-output sampling (independent strides + signal subscriptions):\n"
      "  --emit-<csv|shm|tcp|udp>-stride N selects a step-aligned stride\n"
      "  for that output only (N>=1: forward step%N==0; N=0: muted;\n"
      "  default 1 = every emitted tick). --emit-every stays the global\n"
      "  Runner gate, so an output observes steps divisible by BOTH gates\n"
      "  (intersection, not product). Decimation changes sampled bandwidth\n"
      "  and may alias a waveform (see docs/delivery.md sec. 2).\n"
      "  --emit-<out>-select <s0,s1> restricts a signal output to stable\n"
      "  source serials (signal solver only; requires its --emit-<out>).\n"
      "  Subscriptions filter rows, never ticks, so reader LagStats stay\n"
      "  exact; empty/muted selections drop loudly via counters. Outputs\n"
      "  needing >64 channels (>63 signal sources + sig.count) are\n"
      "  rejected before running — subscribe to a fitting subset instead.\n"
      "  --emit-udp-budget BYTES caps each UDP datagram, headers included\n"
      "  (default 1200: MTU-safe on 1280-byte IPv6-minimum and 1500-byte\n"
      "  Ethernet paths — one lost IP fragment loses the whole datagram).\n"
      "  An indivisible over-budget record is refused loudly (over-bound\n"
      "  counter + sticky status; summaries report need= required bytes).\n"
      "  Shrink records with --emit-udp-select (fewer channels); strides\n"
      "  only lower the datagram rate, never the size. Splitting one tick\n"
      "  across several budgeted datagrams is the sample-batch packetizer's\n"
      "  extension point.\n"
      "  --emit-uart <device[,baud]> streams signal rows over a serial line\n"
      "  (SIG-8 Zephyr HIL track, solver 'signal' only): one CSV-shaped row\n"
      "  per source per emission (`step,time,sig_id,value,quantity`), ascii\n"
      "  lines by default or COBS frames with --emit-uart-mode cobs (baud\n"
      "  9600..921600, default 115200 8N1). UART never back-pressures the\n"
      "  sim: overdriven lines drop loudly (see docs/zephyr-hil.md for the\n"
      "  baud budget table — decimate fast runs). Per-output stride/select\n"
      "  work as on every transport; lines are unbounded (no 64-channel cap).\n"
      "  --emit-health prints the per-cause live-output health report\n"
      "  (attempted/published/over-bound/backpressure/pending/disabled +\n"
      "  periodic stderr updates on long runs); default output stays concise.\n"
      "  Publish failures are always loud, with or without the flag.\n"
      "Pacing (optional wall-clock mode, default off = fast batch): --paced\n"
      "  spaces fixed-dt steps against absolute steady-clock deadlines\n"
      "  (no drift, no skipped steps, dt unchanged); --rate sets wall steps\n"
      "  per second (Hz/kHz, default 1/dt = real-time). Late steps run\n"
      "  immediately and are counted, never skipped (see docs/delivery.md).\n"
      "  Ctrl-C stops at a step boundary. Waveform Hz (--sig-freq) is the\n"
      "  physics being simulated; sample rate is 1/(dt*emit_every) and the\n"
      "  paced packet rate is --rate/decimated emissions — three clocks.\n"
      "Defaults: steps=100, dt=0.01s, gravity=9.81, particles=1,\n"
      "  x0=y0=0, vx0=vy0=10, emit-every=1. Time unit: s/ms/us/ns.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct SigSpec {
  static constexpr std::string_view kName = "sig";
  static constexpr std::string_view kDescription =
      "Generate signals (sine/square/triangle/saw/dc/pulse/noise/sum/expression/table/lua)";
  static constexpr std::string_view kUsage =
      "sig run [--sig-kind <kind>] [--sig-freq <Hz>] [--sig-amp <V|A>]\n"
      "        [--sig-offset <V|A>] [--sig-phase <rad>] [--sig-duty 0..1]\n"
      "        [--sig-seed N] [--sig-quantity U|I] [--sig-count N]\n"
      "        [--sig-expr '<math>'] [--sig-table <csv>] [--sig-lua <file>]\n"
      "        [--steps N] [--dt <s>] [--emit-csv <path>] [--emit-shm <name>]\n"
      "        [--emit-tcp <host:port>] [--emit-udp <host:port>] [--emit-every N]\n"
      "        [--emit-csv-stride N] [--emit-shm-stride N]\n"
      "        [--emit-tcp-stride N] [--emit-udp-stride N]\n"
      "        [--emit-csv-select <s0,s1>] [--emit-shm-select <s0,s1>]\n"
      "        [--emit-tcp-select <s0,s1>] [--emit-udp-select <s0,s1>]\n"
      "        [--emit-udp-budget BYTES]\n"
      "        [--emit-uart <device[,baud]>] [--emit-uart-mode ascii|cobs]\n"
      "        [--emit-uart-stride N] [--emit-uart-select <s0,s1>]\n"
      "        [--emit-health]\n"
      "        [--control-tcp <host:port>] [--control-uart]\n"
      "        [--forever] [--paced] [--rate <Hz>]\n"
      "        (run until Ctrl-C with --forever; --steps is ignored)\n"
      "  sig list --control-tcp <host:port>\n"
      "  sig add --control-tcp <host:port> --sig-kind <kind>\n"
      "        [--sig-quantity U|I] [--sig-freq <Hz>] [--sig-amp <V|A>]\n"
      "        [--sig-offset <V|A>] [--sig-phase <rad>] [--sig-duty 0..1]\n"
      "        [--sig-seed N] [--sig-operands <s0,s1>] [--sig-expr '<math>']\n"
      "  sig set <serial> --control-tcp <host:port>\n"
      "        [--sig-freq <Hz>] [--sig-amp <V|A>] [--sig-offset <V|A>]\n"
      "        [--sig-phase <rad>] [--sig-duty 0..1] [--sig-seed N]\n"
      "        [--sig-quantity U|I]\n"
      "  sig start|stop|drop <serial> --control-tcp <host:port>\n"
      "  sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 1000 --dt 0.1ms\n"
      "  sig run --sig-kind sine --sig-freq 1kHz --dt 0.1ms --paced --rate 1kHz\n"
      "  sig run --sig-kind expression --sig-expr 'sin(2*pi*440*t)*0.5'\n"
      "  sig run --sig-kind table --sig-table wave.csv --steps 1000\n"
      "  sig run --sig-kind lua --sig-lua wah.lua --steps 1000\n"
      "Kinds: sine square triangle saw dc pulse noise sum expression table lua\n"
      "  (--sig-kind, Tab-completed; --sig-count spawns that many identical\n"
      "  sources; noise seeds derive as seed+k; table batches share one file).\n"
      "  sum adds operand sources' values (batch runs reject it — configure\n"
      "  sums with `sig add --sig-kind sum --sig-operands <s0,s1>`).\n"
      "  expression compiles a closed math language over t (source seconds):\n"
      "  literals, t, pi, e, + - * / ^, unary -, sin cos exp log sqrt abs,\n"
      "  min(a,b) max(a,b), parentheses (from_chars parsing, fixed eval order;\n"
      "  div-by-zero and out-of-domain fail the step loudly, never NaN).\n"
      "  table replays a CSV waveform (header 'time,value', SI seconds + SI\n"
      "  value, linear interp, clamped ends, bit-exact on exact time hits).\n"
      "  lua runs a sandboxed Lua 5.4 script per step (pure function of\n"
      "  t/seed/step/dt, must return a number; math+string+small pure base\n"
      "  only — no io/os/package/debug, instruction budget enforced, every\n"
      "  failure stops the step loudly; see docs/signals.md sec. 7).\n"
      "  --sig-freq takes Hz/kHz; --sig-amp/--sig-offset take V/mV/kV or\n"
      "  A/mA per --sig-quantity U|I. --emit-csv writes the signals log\n"
      "  (step,time,sig_id,value,quantity); --emit-shm publishes signal\n"
      "  channels (sig.<slot>.U); --emit-tcp/--emit-udp stream the same\n"
      "  channels over sockets (see sim run above).\n"
      "Per-source family (live control clients, SIG-6): list/add/set/\n"
      "  start/stop/drop speak the SIG-10 verbs to a running\n"
      "  `sig run --control-tcp <host:port>` (sources address by stable\n"
      "  serial, never reused). `sig add` configures one source at a time\n"
      "  — including sums (`--sig-operands <s0,s1>`, rejected in batch\n"
      "  runs) and expressions (`--sig-expr`); table/lua need bulk\n"
      "  payloads and stay batch-only. Values parse like the batch run\n"
      "  (quantity-aware volts/amps, Hz/kHz); the wire carries SI doubles.\n"
      "  Every server verdict prints (`OK ...` -> exit 0, `ERR ...` ->\n"
      "  exit 1); bad flags/endpoints are usage errors (exit 2) and a\n"
      "  refused connection is a runtime error (exit 1).\n"
      "Per-output sampling: --emit-<csv|shm|tcp|udp|uart>-stride N (step-aligned\n"
      "  per-output decimation, 0=muted, default 1) intersects with the\n"
      "  global --emit-every gate (an output sees steps divisible by both;\n"
      "  decimation may alias — see docs/delivery.md sec. 2), and\n"
      "  --emit-<out>-select <s0,s1> restricts that output to stable\n"
      "  source serials (requires its --emit-<out>; filters rows, never\n"
      "  ticks). Full-rate CSV alongside decimated/subset live output is\n"
      "  the intended split (recorder stays complete, live stays cheap).\n"
      "  Outputs needing >64 channels (>63 sources + sig.count) are\n"
      "  rejected before running — subscribe to fit.\n"
      "  --emit-udp-budget BYTES caps each UDP datagram, headers included\n"
      "  (default 1200: MTU-safe on 1280-byte IPv6-minimum and 1500-byte\n"
      "  Ethernet paths — one lost IP fragment loses the whole datagram).\n"
      "  An indivisible over-budget record is refused loudly (over-bound\n"
      "  counter + sticky status; summaries report need= required bytes).\n"
      "  Shrink records with --emit-udp-select (fewer channels); strides\n"
      "  only lower the datagram rate, never the size. Splitting one tick\n"
      "  across several budgeted datagrams is the sample-batch packetizer's\n"
      "  extension point.\n"
      "  --emit-uart <device[,baud]> streams signal rows over a serial line\n"
      "  (SIG-8 Zephyr HIL track): one CSV-shaped row per source per emission\n"
      "  (`step,time,sig_id,value,quantity`), ascii lines by default or COBS\n"
      "  frames with --emit-uart-mode cobs (baud 9600..921600, default 115200\n"
      "  8N1). UART never back-pressures the sim: overdriven lines drop\n"
      "  loudly — decimate fast runs (see docs/zephyr-hil.md for the baud\n"
      "  budget table). Stride/select work as on every transport; lines are\n"
      "  unbounded (no 64-channel cap).\n"
      "  --emit-health prints the per-cause live-output health report\n"
      "  (attempted/published/over-bound/backpressure/pending/disabled +\n"
      "  periodic stderr updates on long/--forever runs); default output\n"
      "  stays concise. Publish failures are always loud, with or without\n"
      "  the flag.\n"
      "Control plane (--control-tcp <host:port>, SIG-10): listens for\n"
      "  text-line commands (HELLO/PING/LIST/ADD/SET/START/STOP/DROP;\n"
      "  SELECT is reserved for the future mux) from any number of TCP\n"
      "  clients (Zephyr/PC). Commands apply at exact step boundaries in\n"
      "  fixed channel order with last-writer-wins per (source, param)\n"
      "  and log for replay; the run steps singly while control is\n"
      "  attached. Verbs, handshake, and ordering are specified in\n"
      "  docs/signals.md sec. 6.\n"
      "  --control-uart (SIG-8, D4b-A) interleaves the same verbs on the\n"
      "  --emit-uart wire as control frames (`!line` in ascii mode,\n"
      "  COBS 'Q'/'R' frames in cobs mode), drained after the TCP slots.\n"
      "Pacing (optional wall-clock mode, default off = fast batch): --paced\n"
      "  spaces fixed-dt steps against absolute steady-clock deadlines\n"
      "  (no drift, no skipped steps, dt unchanged); --rate sets wall steps\n"
      "  per second (Hz/kHz, default 1/dt = real-time). Late steps run\n"
      "  immediately and are counted, never skipped (see docs/delivery.md).\n"
      "  Works for finite runs and with --forever; Ctrl-C stops at a step\n"
      "  boundary. Waveform Hz (--sig-freq) is the physics being simulated;\n"
      "  sample rate is 1/(dt*emit_every) and the paced packet rate is\n"
      "  --rate/decimated emissions — three separate clocks.\n"
      "Defaults: kind=sine, freq=1Hz, amp=1, offset=0, phase=0, duty=0.5,\n"
      "  seed=1, quantity=U, count=1, steps=100, dt=0.01s, emit-every=1;\n"
      "  --forever runs continuously until Ctrl-C.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct LdoSpec {
  static constexpr std::string_view kName = "ldo";
  static constexpr std::string_view kDescription = "LDO linear regulator operating point";
  static constexpr std::string_view kUsage =
      "ldo --vin <V> --vout <V> --iout <A> [options]   (generic linear regulator)\n"
      "  ldo --vin 5V --vout 3.3V --iout 500mA\n"
      "  ldo --vin 5V --vout 3.3V --iout 500mA --iq 50uA --vdo 0.3V\n"
      "  ldo --vin 5V --vout 3.3V --iout 500mA --ta 85 --rth 100 --tjmax 125\n"
      "Options: --iq <A> (quiescent/ground current, default 0),\n"
      "  --vdo <V> (dropout voltage at load, default 0 = no dropout check),\n"
      "  --ta <C> (ambient temperature, default 25),\n"
      "  --rth <C/W> (junction-to-ambient thermal resistance, default 0 =\n"
      "  no thermal rise), --tjmax <C> (thermal limit for PASS/FAIL;\n"
      "  default none). Units: V/mV/kV, A/mA/uA; --ta/--rth/--tjmax are\n"
      "  plain numbers (C, C/W, C).\n"
      "Core identities: headroom = VIN - VOUT (>= Vdo), IIN = IOUT + Iq,\n"
      "  Ppass = (VIN - VOUT) * IOUT, Pq = VIN * Iq, Ptot = Ppass + Pq,\n"
      "  eff = VOUT*IOUT / (VIN*IIN), TJ = TA + Ptot * RthJA.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct LedSpec {
  static constexpr std::string_view kName = "led";
  static constexpr std::string_view kDescription = "LED series resistor (supply Vf/If)";
  static constexpr std::string_view kUsage =
      "led --vs <V> --vf <V> --if <A> [options]   (generic LED resistor)\n"
      "  led --vs 5V --vf 2V --if 20mA\n"
      "  led --vs 5V --vf 3.2V --if 20mA --rs 100Ohm   (evaluate Rs at hand)\n"
      "  led --vs 5V --vf 2V --if 20mA --prating 0.25W (resistor power check)\n"
      "Options: --rs <Ohm> (explicit resistor, default: nearest E12 pick),\n"
      "  --prating <W> (resistor power rating for PASS/FAIL; default none).\n"
      "  Units: V/mV/kV, A/mA/uA, Ohm/kOhm, W/mW.\n"
      "Core identities: headroom = VS - VF (> 0 required),\n"
      "  Rs = headroom / IF, IF' = headroom / Rs_used,\n"
      "  Pres = headroom * IF', Pled = VF * IF', eff = VF / VS.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

struct IcSpec {
  static constexpr std::string_view kName = "ic";
  static constexpr std::string_view kDescription = "IC-specific calculators (datasheet formulas)";
  static constexpr std::string_view kUsage =
      "ic <chip> <op> [options]   (one command for all parts — chips never\n"
      "        become top-level commands; see `ic --help` for the list)\n"
      "  ic --help\n"
      "  ic <chip> --help     (per-chip subcommands, e.g. ic ne555 astable ...)\n"
      "Solvers live in solvers/ic/ (one part per header, formulas quoted\n"
      "  from the datasheet); each chip dispatches its own subcommands.";

  static int run(ArgCursor &args, std::ostream &out);
  static void complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out);
};

} // namespace wengine::cli::commands
