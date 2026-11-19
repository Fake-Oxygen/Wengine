# CLI commands

Every command below was run exactly as shown while writing this guide;
usage text matches `./build/PhysicsSimApp help <cmd>`. Arguments use
`--key value` or `--key=value` forms (short flags such as `-h` exist for
help). A stray positional argument is never ignored — it is a usage
error (exit 2). Run any calculator or run command with `--help` for its
full option list.

## Closed-form calculators

Each takes named options with unit suffixes (`12V`, `0.54A`, `4Ohm`,
`200pF`, bare numbers read as SI units) and prints one result block.

### ohm — resistor (Ohm's law)

Give any two of voltage, current, resistance; the third plus power is
computed.

```console
$ ./build/PhysicsSimApp ohm 12V 3A
U = 12 V
I = 3 A
R = 4 Ohm
P = 36 W
```

Named forms work too (`ohm --u 12V --r 4Ohm` solves from U+R). Exit 0 on
success; providing anything other than exactly two of U/I/R is a usage
error (exit 2).

### s0 — S0 pulse output (energy meter)

Converts between electrical power/energy and meter pulse rates for a
meter constant `C` in `imp/kWh`:

```console
$ ./build/PhysicsSimApp s0 rate --power 2kW --const 1000imp/kWh
P = 2000 W
f = 0.555556 Hz (imp/s)
T = 1.8 s/imp
rate = 33.3333 imp/min
rate = 2000 imp/h
```

Modes are `convert`, `rate`, `power`, `energy`, `pulses`, and `full`;
`full` combines U/I (plus power factor) with the meter constant.

### i2c — I2C bus resistors

Sizes pull-up (`Rp`) windows and checks series resistors per NXP UM10204
section 7.1:

```console
$ ./build/PhysicsSimApp i2c pullup --vdd 3.3V --cb 200pF
limits: fast (tr 300 ns, tf 300 ns, VOL 0.4 V, IOL 3 mA, Cbmax 400 pF)
VDD = 3.3 V, Cb = 200 pF
Rp(min) = 966.667 Ohm
Rp(max) = 1770.33 Ohm
Rp window: FEASIBLE
Rp(rec) = 1308.18 Ohm
Rp(E12) = 1200 Ohm
Rp used = 1200 Ohm
tr @Rp = 203.351 ns (limit 300 ns) OK
I static = 2.41667 mA (limit 3 mA) OK
P static = 7.00833 mW per line
```

Modes are `pullup`, `series`, and `full` (the last adds a PASS/FAIL
verdict). Speed presets (`--speed std|fast|fmp`) set rise-time and
capacitance limits; `--vol/--iol/--tr` override them.

### ldo — LDO linear regulator

Generic operating-point check (part-specific numbers live under `ic`):

```console
$ ./build/PhysicsSimApp ldo --vin 5V --vout 3.3V --iout 500mA
VIN = 5 V, VOUT = 3.3 V, IOUT = 0.5 A
headroom = 1.7 V (min VIN 3.3 V for Vdo 0 V) OK
IIN = 0.5 A (Iq 0 A)
POUT = 1.65 W, PIN = 2.5 W
P pass = 0.85 W, P q = 0 W, P total = 0.85 W
eff = 66 %
TJ = 25 C at TA = 25 C (Rth 0 C/W) (no Tj limit)
summary: PASS
```

Optional `--iq/--vdo/--ta/--rth/--tjmax` add quiescent current, dropout,
and thermal checks. Exit 0 on `PASS`, 1 on dropout/thermal `FAIL`, 2 on
usage error.

### led — LED series resistor

You supply the LED's forward voltage and current; the tool picks (E12)
or evaluates (`--rs`) the series resistor:

```console
$ ./build/PhysicsSimApp led --vs 5V --vf 2V --if 20mA
VS = 5 V, VF = 2 V, IF target = 0.02 A
headroom = 3 V OK
Rs exact = 150 Ohm
Rs(E12) = 150 Ohm
Rs used = 150 Ohm (E12 pick)
IF actual = 0.02 A
P res = 0.06 W, P led = 0.04 W, P total = 0.1 W
eff = 40 %
(no power-rating limit)
summary: PASS
```

`--prating` adds a resistor dissipation PASS/FAIL. An impossible supply
(`--vs 5V --vf 9V`) prints `headroom = -4 V INFEASIBLE` and exits 1.

## sim — stepped simulation

Runs any registered stepped solver with defaults; the only
solver-scoped option here is the grandfathered `--gravity`
(projectile only). Unknown solver names list what is available:

```console
$ ./build/PhysicsSimApp sim run --solver nonsense --steps 5
Error: unknown solver 'nonsense'. Available: projectile signal logic
```

The standard invocation and its most useful flags:

```console
$ ./build/PhysicsSimApp sim run --steps 100 --particles 1
sim run: solver=projectile g=9.81
steps: 100 x 0.01s -> t=1s
particles: 1
p0: x=10 y=5.04595 vx=10 vy=0.19
```

- `--solver projectile|signal|logic` (default `projectile`).
- `--steps N`, `--dt 10ms`, `--particles N`, `--x0/--y0/--vx0/--vy0`.
- `--emit-csv <path>` writes the full snapshot log; `--emit-shm`,
  `--emit-tcp/--emit-udp` publish or stream the same frames live.
- `--emit-every N` plus per-output `--emit-<out>-stride` /
  `--emit-<out>-select` decimate or subscribe outputs (see
  [Outputs](outputs.md)); `--paced [--rate 1kHz]` spaces steps against
  wall-clock deadlines instead of running flat out.
- `--solver signal` runs one default 1 Hz sine with zero particles;
  `--solver logic --seq-file workspace.yaml` steps a saved logic
  workspace (the `logic save` verb writes that file).

## sig — signal generation (all signal vocabulary lives here)

Never on `sim`: every signal flag (`--sig-kind`, `--sig-freq`,
`--sig-amp`, `--sig-count`, `--sig-expr`, `--sig-table`, `--sig-lua`,
`--forever`, `--plot`, `--control-tcp`, …) belongs to `sig`:

```console
$ ./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 10 --dt 0.1ms --emit-csv /tmp/opencode/sig-example.csv
sig run: kind=sine
steps: 10 x 0.0001s -> t=0.001s
signals: 1
s0: sine value=2.12272e-15 U
csv: /tmp/opencode/sig-example.csv (10 rows)
```

Kinds are `sine square triangle saw dc pulse noise sum expression table
lua`; batch runs reject `sum` (configure sums with the live `sig add
--sig-kind sum --sig-operands` client instead). `--emit-csv` writes the
signals log (`step,time,sig_id,value,quantity`). The `sig
list/add/set/start/stop/drop --control-tcp <host:port>` family drives a
running `sig run --control-tcp` at step boundaries.

## ic — chip calculators (`ic <chip> <op>` dispatch)

One top-level command serves every part; new parts register as chips,
never as commands. Installed chips on this build:

```console
$ ./build/PhysicsSimApp ic --help
...
Available ICs:
  ap63205 - AP63205WU-7 buck design (inductor, COUT, UVLO, EN delay, thermal)
  ina219 - INA219 current/power monitor (calibration, shunt/bus codes, readout, trim)
  tmc22xx - TMC2202/2208/2224 stepper driver (current, chopper, StealthChop, standby)
```

Each chip has its own subcommands (`ic <chip> --help` lists them):

```console
$ ./build/PhysicsSimApp ic ap63205 spec
AP63205WU-7: fixed 5V, 1.1MHz, PWM/PFM, 2A max, TSOT26
VIN 3.8..32 V (internal UVLO 3.1 V), VFB 5.0 V
RDS(ON) HS 125 mOhm / LS 68 mOhm
HS peak limit 2.5..3.1 A, LS valley limit 2.5..3.9 A
tON min 80 ns, soft-start 4 ms, EN high 1.18 V / low 1.10 V
Table 3: L 4.7uH, CIN 10uF, COUT 2x22uF, BST 100nF
```

```console
$ ./build/PhysicsSimApp ic ina219 cal --imax 15A --rshunt 2mOhm
...
Cal = 44738 (0xAEC2) OK
...
calibration: PASS
```

```console
$ ./build/PhysicsSimApp ic tmc22xx current --rsense 220mOhm
...
current: PASS
```

## logic — logic-gate workspace (sub-shell session)

Bare `logic` enters a `[logic]> ` session (`back` leaves); with
arguments it runs one verb and exits. Verbs cover `add/list/wire/
unwire/remove/ignore/use/tie/untie/reset/solve/truth/optimize/draw/
save/load/export` plus sequential `step/poke/peek`:

```console
$ ./build/PhysicsSimApp logic add NAND --inputs 2
added NAND1 kind=NAND inputs=2
```

Note: one-shot invocations do not share state — a following `logic
list` in a new process shows an empty workspace. Use the session (or
`save`/`load` YAML files) for multi-step work.

## Completion and exit codes

The interactive shell Tab-completes command names, `help` arguments,
`s0`/`i2c` modes, `ohm --from` values, `ic` chips/ops, and option names
plus `--opt=` values. Exit codes are `0` success, `1` runtime/check
failure, `2` usage error, `-1` internal exit signal (REPL loop only):

```console
$ ./build/PhysicsSimApp ohm 12V 3A; echo $?
U = 12 V
...
0
$ ./build/PhysicsSimApp ohm 12V; echo $?
Error: provide exactly 2 of U/I/R (got 1)
...
2
```

Unknown commands exit 1 (`Unknown command: boguscmd`). `./build/PhysicsSimApp --tui`
opens the same command set over ncurses (byte-identical output; needs a
real terminal, takes no extra arguments) — see [Terminal UI](../tui.md).
