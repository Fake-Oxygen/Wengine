# Solvers

The closed-form slice of the suite: pure header-only calculators plus the
units machinery they share. Every golden is anchored to an independent
reference — a textbook identity, a standards document, or a datasheet
equation quoted in the solver header — never copied from the
implementation. The `test_*` binaries complement the CLI coverage in each
solver's `run`/`complete` surface.

## test_math_units

**What it checks**: `Value<Dim>` comparisons (including cross-magnitude
`kW` vs `W` and the `MHz`/`rpm`/`rps` literal spot-checks), `ostream`
formatting (raw SI base value, callers add context), the single-source
proof that every suffix in `units/scales.hpp` parses through the CLI
forwarders to exactly its table scale (bare numbers stay SI), and the
vendored glm basics (`dot`, `length`) used by the 3D viewport math.

**Purpose**: units are the one place unit-text knowledge lives; if the
suffix tables and the parser ever drift apart, every solver CLI breaks
silently.

**Guards against**: suffix/scale table drift, literal collisions, and
dimension confusion at parse/display edges.

**How to run**:

```sh
ctest --test-dir build -R math_units
./build/test_math_units
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover signal quantities (see `test_signal`) or
any solver's numerics.

## test_ohm_law

**What it checks**: `solve_from_ui(12V, 3A)` giving `R = 4Ω`, `P = 36W`;
` solve_from_ir`/`solve_from_ur` round-trips; and a `static_assert` that
the solver is genuinely `constexpr` (compile-time evaluation, part of the
hot-path performance story).

**Purpose**: pins the trivial-but-foundational `V = IR`, `P = UI`
identities with SI quantities across the boundary.

**Guards against**: unit-wrapping regressions in the typed solver API.

**How to run**:

```sh
ctest --test-dir build -R ohm_law
./build/test_ohm_law
```

**Prerequisites**: standard build only.

**Boundaries**: covers the solver only; CLI surface lives in `test_sim`
and the IC dispatch matrix in `test_ic`.

## test_s0

**What it checks**: S0 meter-constant conversions anchored to the DIN
43864 identities in the solver header (`1000 imp/kWh ↔ 1 imp/Wh`,
`3600 J/imp`, energy bridges, `Wh/s → W`), forward/inverse frequency,
power, pulse-count, and energy relations with the `2 kW → 0.555… Hz →
1.8 s` workpoint, the typed SI API (`2000 imp @ 1000 imp/kWh = 7.2 MJ`),
the `solve_full` aggregate, `constexpr` proofs, and the full `s0` CLI
surface (`convert`/`rate`/`power`/`energy`/`pulses`/`full`, named and
positional forms, stray positionals and missing `--const` as usage
errors).

**Purpose**: energy-meter pulse math must match the standard's identities
exactly; the CLI is the field-facing calculator.

**Guards against**: conversion-factor drift, forward/inverse asymmetry,
and silently ignored CLI arguments.

**How to run**:

```sh
ctest --test-dir build -R "^s0$"
./build/test_s0
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover the IC part calculators (see `test_ic`
and below) or pulse-train generation over time.

## test_i2c

**What it checks**: reference anchors straight from NXP UM10204 §7.1 and
TI SLVA689 (the `(5.5 − 0.4)/3 mA = 1.7 kΩ` minimum, the `300 ns/200 pF →
1.77 kΩ` maximum, NXP Fm+ training points), rise-time round-trips, static
load current/power, E12 snapping (including the next-decade rule),
geometric-mean recommendation, per-mode limits matching UM10204 Table 10,
defaults matching Table 9 style, the `[min, max]` pull-up window with
E12 pick and explicit-`Rp` evaluation, series-resistor LOW/rise budgets
with explicit-`Rs` evaluation, infeasible-window handling, the typed SI
API, and the full `pullup`/`series`/`full` CLI surface (Hs-mode refused
with an explanation, never sized; check failures exit 1, usage errors
exit 2).

**Purpose**: bus sizing must agree with the published reference documents
the header cites — the numbers come from NXP/TI, not from the code.

**Guards against**: reference drift, E12 decade-boundary mis-snapping
(the known `966 Ω → 1 kΩ` lesson), and silently sized high-speed mode.

**How to run**:

```sh
ctest --test-dir build -R "^i2c$"
./build/test_i2c
```

**Prerequisites**: standard build only.

**Boundaries**: does not cover generic solver plumbing; part-specific
calculators live in the `test_ic` family below.

## test_ic

**What it checks**: the shared two-level `ic <chip> <op>` dispatch over
test-only chips (routing, per-chip `--help` forwarding, missing/unknown
chip/op errors, stray-token surfacing, completion delegation including
chips without a `complete` member staying silent), and the production
registry pins (`IcRegistry::all().size() == 3` with `ap63205`, `ina219`,
`tmc22xx`; `CliRegistry` `ic` entry with completion; end-to-end `ic --help`
and `ic nope` through the generic completion engine and `Shell`).

**Purpose**: hundreds of future parts must never become hundreds of
top-level commands; this pins the dispatch contract every chip inherits.

**Guards against**: registry-size drift, dispatch-duplication per chip,
completion crashes on chips without completion, and stray arguments
leaking past the chip layer.

**How to run**:

```sh
ctest --test-dir build -R "^ic$"
./build/test_ic
```

**Prerequisites**: standard build only.

**Boundaries**: covers dispatch only; per-chip equations are tested in
`test_ap63205`, `test_ina219`, and `test_tmc22xx`.

## test_ldo

**What it checks**: a hand-derived operating point (`5 V → 3.3 V @
0.5 A`, `Iq = 50 µA`: headroom `1.7 V`, `Ptot = 0.85025 W`,
`TJ = 110.025 °C`), primitive identities on exact integer points,
no-load efficiency defined as `1.0`, unphysical zero-input power as `+inf`
(never NaN), inclusive dropout boundary, thermal-limit gating only when
`tmax` is supplied, the typed SI API with `µA` parsing, and the full CLI
surface (PASS/FAIL summaries with exit 1 on check failure, stray/unknown
options and non-positive `VIN` as usage errors) plus completion of all
nine options.

**Purpose**: anchors the generic LDO calculator — textbook identities
requiring no datasheet, per the solver header — at a reference point
derived by hand.

**Guards against**: power-accounting drift (`eff × PIN = POUT`), edge-case
NaN/inf leaks, and silent CLI argument acceptance.

**How to run**:

```sh
ctest --test-dir build -R "^ldo$"
./build/test_ldo
```

**Prerequisites**: standard build only.

**Boundaries**: generic LDO only; part-specific regulators are `ic`
chips (see `test_ap63205`).

## test_led

**What it checks**: a hand-derived operating point (`5 V`, `VF = 2 V`,
`20 mA`: `Rs = 150 Ω` exactly E12, `IF' = 20 mA`, `Pres = 60 mW`,
`eff = 40 %`), primitive identities on exact points, zero-current as
`+inf` resistance (never NaN), E12 re-picking with recomputed current
(`60 Ω → E12 56`), explicit-`Rs` as-is evaluation, infeasible headroom
(`VS ≤ VF`) with zeroed picks, rating-limit gating only when supplied,
the typed SI API, and the full CLI surface (E12/explicit markings in
output, `P rating` and headroom FAIL paths, stray/zero-`Rs`/unknown
options as usage errors) plus completion.

**Purpose**: anchors the LED series-resistor calculator, including its
reuse of the tested E12 snap with the next-decade rule.

**Guards against**: E12 mis-snapping changing drive current silently,
NaN leaks at zero current, and unrated-vs-rated limit confusion.

**How to run**:

```sh
ctest --test-dir build -R "^led$"
./build/test_led
```

**Prerequisites**: standard build only.

**Boundaries**: generic LED only; no part-specific LED-driver chip exists
yet.

## test_ina219

**What it checks**: datasheet-anchored constants from TI SBOS448G
(`kShuntLsbV = 10 µV`, `kBusLsbV = 4 mV`, reset word `0x399F`), the §9.2.2
calibration goldens (`15 A → 457.76 µA` LSB, `Cal = 20480`, power LSB,
max current/power), PGA/BRNG selection with over-range and 26 V
absolute-maximum failures, shunt/bus register goldens (`20 mV → 2000`,
`11.98 V → 0x5D98`), readout goldens (`10.0 A`, `119.8 W` from the §9.2.2
register image), Eq.6 calibration correction, Table 5 ADC timing spot
checks, and the eight-op CLI surface (`cal`/`shunt`/`bus`/`decode`/
`correct`/`adc`/`config`/`spec`) with check-failure vs usage-error exit
codes, completion lists, and a `Shell` end-to-end.

**Purpose**: proves the INA219 calculator reproduces the datasheet's own
worked examples bit-for-bit through both the solver and CLI layers.

**Guards against**: LSB/truncation drift, PGA-range mis-selection,
absolute-maximum blindness, and CLI/decode divergence from the datasheet.

**How to run**:

```sh
ctest --test-dir build -R ina219
./build/test_ina219
```

**Prerequisites**: standard build only.

**Boundaries**: pure calculation and register images; no live I²C
hardware is touched.

## test_ap63205

**What it checks**: datasheet-anchored constants from Diodes DS41326
(`VOUT = 5 V`, `fsw = 1.1 MHz`, `2 A` max), Eq.7/8 inductor goldens
(`12 V → 3.314 µH`, `2.4 A` peak vs the `2.5 A` HS limit), Eq.9/10
output-capacitor goldens (`4 mV` ripple, `10.57 µF` minimum), Eq.1/2 UVLO
resistor goldens (`R5 = 111.22 k`, `R6 = 18.79 k`), Eq.3 enable-delay
goldens (`10 ms ↔ 12.7 nF`), thermal goldens (`0.5 W @ 85 °C → 129.5 °C`
over target; conduction estimate `0.367 W → 117.66 °C`), and the six-op
CLI surface (`inductor`/`cout`/`uvlo`/`delay`/`thermal`/`spec`) with
check-failure vs usage-error codes, completion, and a `Shell`
end-to-end.

**Purpose**: proves the AP63205 buck calculator reproduces the datasheet
equations through both the solver and CLI layers.

**Guards against**: equation-coefficient drift, limit-check blindness
(peak current, thermal), and mutually exclusive option pairs being
accepted together.

**How to run**:

```sh
ctest --test-dir build -R ap63205
./build/test_ap63205
```

**Prerequisites**: standard build only.

**Boundaries**: pure calculation; no live converter hardware is touched.

## test_tmc22xx

**What it checks**: Trinamic-manual-anchored goldens across all
families — Ch.9 current equations (`0.9192 A` standalone, CS scaling,
`Rsense` inverse, CS picker, sense dissipation), Ch.10 RDSon model and
lookup rows, Ch.7 chopper equations (TOFF, blanking, PWM frequency,
hysteresis truth table, MRES codes, standalone pin decodes), velocity
equations (VACTUAL/TSTEP/fullstep/rpm chain), stealthChop seeds and OTP
nearest picks, standby/thermal/UART timing, and the Eq.17 CRC8-ATM vectors
(`{05 00} → 0x18`, 7-byte write frame `→ 0xFE`) — plus the sixteen-op CLI
surface with check-failure vs usage-error codes, completion, and a
`Shell` end-to-end.

**Purpose**: proves the TMC22xx stepper-driver calculator reproduces the
manual's equations and CRC reference through both layers.

**Guards against**: current-limit miscalculation (motor damage class),
chopper mistiming, CRC divergence from the datasheet C reference, and
mode/pin-decode errors.

**How to run**:

```sh
ctest --test-dir build -R tmc22xx
./build/test_tmc22xx
```

**Prerequisites**: standard build only.

**Boundaries**: pure calculation and frame bytes; no live stepper
hardware is driven.
