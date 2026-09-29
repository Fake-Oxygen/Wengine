# WEngine

High-performance C++20 physics simulation engine. Currently CLI-driven;
a GUI and external-app streaming layer are planned. The core is a set of
header-only, allocation-free modules (math, units, solvers) with a thin
CLI adapter on top.

## Requirements

- CMake >= 3.20
- C++20 compiler (developed/tested with GCC 13.3)
- GLM via the `external/glm` submodule (pinned 1.0.1) — clone with
  `git clone --recursive` or run `git submodule update --init` after cloning.
  Otherwise dependency-free (tests use plain `assert`, no gtest fetch)
- GUI only (`-DBUILD_GUI=ON`): Dear ImGui via the `external/imgui`
  submodule (pinned v1.91.9b) plus system `libsdl2-dev` and
  `libvulkan-dev` (SDL2 windowing + Vulkan rendering, see policy below).

## Build

Out-of-source builds only (in-source is rejected by `CMakeLists.txt`):

```sh
cmake -S . -B build
cmake --build build
```

Options (CLI/tests/benchmarks default `ON`, GUI defaults `OFF`):

```sh
cmake -S . -B build -DBUILD_CLI=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON -DBUILD_GUI=OFF
# GUI blank app: cmake -S . -B build-gui -DBUILD_GUI=ON && cmake --build build-gui
```

Run tests and benchmarks:

```sh
ctest --test-dir build --output-on-failure
./build/bench_ohm
./build/bench_s0
./build/bench_i2c
./build/bench_projectile
./build/bench_signal
```

Benchmarks use runtime-generated (opaque) inputs so the `constexpr`
solvers cannot be constant-folded away; each prints `ns/iter` (or
`ns/particle-step` for the stepped path) plus a checksum sink.

`compile_commands.json` is exported to `build/` for clangd/clang-tidy.

## Run (CLI)

```sh
./build/PhysicsSimApp                  # interactive REPL
./build/PhysicsSimApp ohm 12V 3A       # headless: run once and exit
./build/PhysicsSimApp s0 rate --power 2kW --const 1000imp/kWh
./build/PhysicsSimApp --help
```

Headless mode passes `argv` straight to the command — no shell startup, no
re-tokenizing (an argument containing spaces stays one argument), exit code
is the command result (`0` ok, `1` runtime/check failure, `2` usage error;
`exit`/`quit` map to `0`). With no arguments the interactive REPL starts.

REPL commands: `help [cmd]`, `exit`/`quit`, plus registered commands
(`ohm`, `s0`, `i2c`, `sim`, `sig`).
`--help`/`-h` also print help. Return codes: `0` ok, `1`
runtime/unknown-command error, `2` argument/usage error, `-1` internal exit
signal (REPL loop only).

Interactive input (TTY) has shell-style line editing with persistent
history: `Up`/`Down` recall previous commands (current draft is kept while
browsing), `Left`/`Right` move the cursor, `Backspace`/`Del` delete,
`Home`/`End` (or `Ctrl-A`/`Ctrl-E`) jump, `Ctrl-U`/`Ctrl-K`/`Ctrl-W` kill
text, `Ctrl-L` clears the screen, `Ctrl-C` cancels the line, `Ctrl-D` on an
empty line exits. `Tab` autocompletes: command names (`o` → `ohm `),
`help` arguments, modes (`s0 r` → `s0 rate `), option names
(`s0 --` lists `--power --const …`) and option values (`ohm --from=`
lists `ui ur ir …`). One match completes plus a space; several complete the
common prefix or list candidates in columns. Each command provides its
vocabulary via an optional `complete` spec member (checked into the
compile-time registry). History (cap 500, skips blanks and consecutive duplicates)
is stored at `$XDG_CACHE_HOME/wengine/history` (fallback
`~/.cache/wengine/history`). Piped input keeps plain `getline` behavior, so
`printf 'ohm 12V 3A\nexit\n' | ./build/PhysicsSimApp` still works.

```text
> ohm 12.4V 0.54A
U = 12.4 V
I = 0.54 A
R = 22.963 Ohm
P = 6.696 W

> ohm --u 12V --r 4Ohm      # solve from U+R
> ohm --i 2A --r 5Ohm       # solve from I+R
> ohm --u=1kV --i=2A        # SI prefixes: mV/kV/MV, mA/kA, mOhm/kOhm/MOhm
> ohm --help
```

S0 pulse output (energy meter, constant `C` in `imp/kWh`):

```text
> s0 convert --const 1000imp/kWh     # imp/Wh, imp/J (= imp/Ws), J/Wh/kWh per imp
> s0 rate --power 2kW --const 1000imp/kWh        # f [Hz], T [s], imp/min, imp/h
> s0 power --freq 0.5556Hz --const 1000imp/kWh   # inverse: pulse rate -> W/kW
> s0 energy --pulses 500 --const 1000imp/kWh     # inverse: pulses -> kWh/Wh/J
> s0 pulses --power 2kW --time 1h --const 1000imp/kWh
> s0 pulses --energy 2kWh --const 1000imp/kWh    # energy batch -> pulse count
> s0 full --u 230V --i 10A [--pf 1.0] --const 1000imp/kWh [--time 1h]
```

Core identity: `f = P * C / 3_600_000` (`P` in W, `f` in Hz == imp/s);
`N = E_kWh * C`. Meter-constant forms accepted: `imp/kWh`, `imp/Wh`,
`imp/J` (== `imp/Ws`), `imp/kJ`, `imp/MWh`. Energy units: `J`/`Ws`/`kJ`/
`Wh`/`kWh`; power `W`/`kW`; time `s`/`ms`/`us`/`ns`/`min`/`h`.

I2C bus resistors (NXP UM10204 §7.1 pull-up sizing + series checks):

```text
> i2c pullup --vdd 3.3V --cb 200pF            # Rp window, rec, E12, tr/I/P
> i2c series --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm
> i2c full --vdd 3.3V --cb 200pF --rp 1.2kOhm --rs 100Ohm   # + PASS/FAIL
```

Core identities: `Rp(min) = (VDD − VOL) / IOL`,
`Rp(max) = tr / (0.8473 · Cb)`; `Rs ≤ min((0.3·VDD − VOL)/IOL, Rp(max) − Rp)`.
Speed presets (`--speed std|fast|fmp`, default `fast`): tr 1000/300/120 ns,
Cb max 400/400/550 pF, IOL 3/3/30 mA. `--vol/--iol/--tr` override any preset.
Hs-mode is rejected (current-source pull-ups, UM10204 §5). Capacitance units:
`F`/`mF`/`uF`/`nF`/`pF`; resistance `Ohm`/`kOhm`; time `s`/`ms`/`us`/`ns`.

Stepped simulation (engine vertical slice — runner + state + sinks):

```text
> sim run --steps 100 --particles 1
sim run: solver=projectile g=9.81
steps: 100 x 0.01s -> t=1s
particles: 1
p0: x=10 y=5.04595 vx=10 vy=0.19
> sim run --steps 20 --dt 10ms --particles 2 --emit-csv run.csv --emit-every 10
> sim run --steps 100 --emit-shm /wengine-demo &
> link_sample /wengine-demo --frames 5   (live SHM read in another terminal)
> link_bridge record /wengine-demo rec.csv --frames 5 &
> link_bridge play run.csv /wengine-demo --rate 10   (replay a CSV log onto the bus)
> sim run --steps 1000 --emit-tcp 192.0.2.20:9000   (TCP LinkFrame stream)
> sig run --steps 1000 --emit-udp 192.0.2.20:9001   (compact UDP frames)
> sim run --steps 100 --paced                       (real-time wall-clock pacing)
> sig run --sig-kind sine --sig-freq 1kHz --dt 0.1ms --paced --rate 1kHz --emit-shm /wengine-sig
```

All `sim` parameters are named (`--solver`, `--steps`, `--dt`, `--gravity`,
`--particles`, `--x0/--y0/--vx0/--vy0`, `--emit-csv`, `--emit-shm`,
`--emit-tcp`, `--emit-udp`,
`--emit-every`, `--emit-<csv|shm|tcp|udp>-stride`,
`--emit-<csv|shm|tcp|udp>-select`, `--emit-udp-budget BYTES`,
`--paced`, `--rate`); the CSV log carries the full snapshot stream for replay,
while `--emit-shm <name>` publishes every emitted snapshot to a
wengine-link SHM segment for realtime local consumers (`examples/
link_sample` prints them live; the segment lives while `sim` runs). `--solver` selects from the
compile-time solver registry (default `projectile`; unknown names list what's
available), so new stepped solvers become runnable without touching the CLI —
`--gravity` applies to `projectile` only, particle population is generic setup.
TCP actively connects to the listening reader at `host:port` and sends a
version/domain hello followed by length-prefixed full frames. UDP sends
versioned compact datagrams containing only used entries, each fitting the
`--emit-udp-budget` payload budget (default 1200 bytes: MTU-safe on
1280-byte IPv6-minimum and 1500-byte Ethernet paths); an indivisible
over-budget record is refused loudly with a `need=` diagnostic instead of
fragmenting. Both transports are
non-blocking; use `--emit-every` (global Runner gate) plus per-output
`--emit-<out>-stride N` (step-aligned, `0` = muted) and, for signal outputs,
`--emit-<out>-select <s0,s1>` (stable serials) to decimate high-rate
simulations — an output sees steps divisible by both gates, and decimation
may alias a waveform (see `docs/delivery.md` sec. 2). Keep the recorder at
full rate while live outputs decimate/subscribe; outputs needing >64 channels
are rejected before running. Use
reader-side `LagStats` to observe network loss. Both commands run a fast
batch by default; `--paced` spaces fixed-dt steps against absolute
steady-clock deadlines instead (no drift, no skipped steps, `dt`
unchanged), with `--rate <Hz>` setting wall steps per second (default
`1/dt` = real-time). Late steps run immediately and are counted in the
`paced:` summary line, never skipped; Ctrl-C stops at a step boundary.
Three clocks stay separate: waveform Hz (`--sig-freq`, the physics),
sample rate `1/(dt*emit_every)`, and paced packet rate. Wire details and measured
rate budgets are in [docs/signals.md, section 5](docs/signals.md#5-transports-stream-out).

Signal generation (`sig run` — its own command, so solver vocabularies never
bloat `sim run`): unlimited sources, stamped U/I:

```text
> sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 1000 --dt 0.1ms
> sig run --sig-kind square --sig-freq 50Hz --sig-amp 5V --sig-duty 0.25
> sig run --sig-kind noise --sig-amp 1V --sig-seed 7 --sig-count 2
> sig run --sig-kind dc --sig-offset 2.5V --emit-csv sig.csv --emit-shm /wengine-sig
> sig run --sig-kind expression --sig-expr 'sin(2*pi*440*t)*0.5'
> sig run --sig-kind table --sig-table wave.csv --steps 1000
> sig run --sig-kind lua --sig-lua wah.lua --steps 1000
> sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --forever
> sig run --sig-kind sine --sig-freq 1kHz --dt 0.1ms --emit-uart /dev/ttyUSB0 --emit-every 4
```

Python signal receivers are under `examples/` (stdlib only). Start one in
the listening terminal, then run the publisher against that host/port:

```sh
python3 examples/link_tcp_signal_receiver.py --bind 0.0.0.0 --port 9000
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 100 --dt 0.1ms --emit-tcp 127.0.0.1:9000

python3 examples/link_udp_signal_receiver.py --bind 0.0.0.0 --port 9001
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 100 --dt 0.1ms --emit-udp 127.0.0.1:9001
```

The TCP sample validates the hello/version/domain and length-prefixed frames;
the UDP sample validates compact datagrams and prints sequence-gap counts.
`link_socket_protocol.py` contains their shared standard-library decoder.
The Zephyr HIL samples live in `examples/zephyr/` (UART stream + control
app, TCP socket app, shared C99 parsers): wiring, baud budgets, and build
steps are in `docs/zephyr-hil.md`.

For a live graph instead of terminal output, install Matplotlib and use the
TCP plotter:

```sh
python3 -m pip install matplotlib
python3 examples/link_tcp_signal_plot.py --port 9000 --history 1000
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 1000 --dt 0.1ms --emit-tcp 127.0.0.1:9000
```

The equivalent UDP plotter listens on port `9001` and exits after three idle
seconds by default:

```sh
python3 examples/link_udp_signal_plot.py --port 9001 --history 1000
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 1000 --dt 0.1ms --emit-udp 127.0.0.1:9001
```

Kinds: `sine square triangle saw dc pulse noise sum expression table lua`
(`--sig-kind`, Tab-completed).
`--sig-freq` (`Hz`/`kHz`), `--sig-amp`/`--sig-offset` (`V`/`mV`/`kV` or
`A`/`mA` per `--sig-quantity U|I`), `--sig-phase` (rad), `--sig-duty`
(square/pulse high fraction), `--sig-seed` (noise; source `k` uses
`seed+k`), `--sig-count` (identical sources, default 1).
`--sig-expr '<math>'` compiles a closed expression over `t` (source
seconds): literals, `t`, `pi`, `e`, `+ - * / ^`, unary `-`, `sin cos exp
log sqrt abs`, `min(a,b)` `max(a,b)`, parentheses — div-by-zero and
out-of-domain fail the step loudly, never NaN. `--sig-table <csv>`
replays a waveform file (header `time,value`, SI seconds + SI value,
linear interp, clamped ends, bit-exact on exact time hits).
`--sig-lua <file>` runs a sandboxed Lua 5.4 script per step (pure
function of `t/seed/step/dt`, must return a number; `math`+`string`+small
pure base only — no `io/os/package/debug`, instruction budget enforced,
every failure stops the step loudly).
Runnable examples live in `examples/lua/` (`sine`, `chirp`, `burst`,
`seed_detune`, `softclip` — each header shows its run command).
`--emit-csv` writes the signals log (`step,time,sig_id,value,quantity`,
exact round-trip); `--emit-shm` publishes signal channels
(`sig.<slot>.U`). `sim run --solver signal` runs one default 1 Hz sine
(zero particles); mixed particle+signal runs pass `--particles N` there.
`--forever` ignores `--steps` and keeps one continuous fixed-step run alive
until Ctrl-C; interruption is handled at a step boundary and then sinks are
flushed and closed normally.
`--control-tcp <host:port>` (SIG-10) listens for text-line commands
(`HELLO/PING/LIST/ADD/SET/START/STOP/DROP`; `SELECT` reserved for the
future mux) from any number of TCP clients: commands apply at exact step
boundaries in fixed channel order with last-writer-wins per
(source, param) and log for replay, e.g. `SET 0 FREQ 50` retunes a live
sine without restarting the run. The per-source family (SIG-6) speaks
those same verbs as ready-made clients against a running
`sig run --control-tcp` (sources address by stable serial, values parse
like the batch run — the wire carries SI doubles):

```text
> sig list --control-tcp 127.0.0.1:9002
> sig add --control-tcp 127.0.0.1:9002 --sig-kind square --sig-freq 50Hz --sig-amp 5V
> sig add --control-tcp 127.0.0.1:9002 --sig-kind sum --sig-operands 0,1
> sig set 1 --control-tcp 127.0.0.1:9002 --sig-freq 60Hz --sig-amp 2.5V
> sig stop 1 --control-tcp 127.0.0.1:9002
> sig start 1 --control-tcp 127.0.0.1:9002
> sig drop 1 --control-tcp 127.0.0.1:9002
```

See `docs/signals.md` for the full design (routing, control plane, Lua,
Zephyr transports).

Argument forms (see `wengine/cli/args.hpp`): positional values with unit
suffixes (`12V`, `0.5A`, `4Ohm`, bare numbers = SI units), named options
`--key value` / `--key=value` / `-k value` (`--u/-U/--voltage`,
`--i/-I/--current`, `--r/-R/--resistance`, `--from ui|ur|ir`), flags
`--help/-h`, single/double-quoted tokens. Max 64 tokens per line
(`kMaxArgs`).

## Architecture

```
include/wengine/<module>/*.hpp   public API (header-only where hot)
src/<module>/*.cpp               implementation 1:1 with headers
apps/cli/main.cpp                 CLI entry (thin)
apps/gui/                         reserved for future GUI
src/cli/                          CLI adapter only (no physics)
tests/  benchmarks/  examples/
```

| Module | Path | Role |
|---|---|---|
| `units` | `wengine/units/units.hpp`, `scales.hpp` | `Dimension<...>`, `Value<Dim>` with dimensional `*`/`/` operators, exact comparisons, `ostream<<` (SI base), `_V`/`_A`/`_mA`/`_Ohm`/`_kOhm`/`_F`/`_pF`/`_nF`/`_uF`/`_s`/`_ms`/`_us`/`_ns`/`_W`/`_J`/`_Wh`/`_kWh`/`_Hz` literals, `scales_for<Dim>` suffix tables (single source of truth for CLI parsing) |
| `solvers` | `wengine/solvers/ohm_law.hpp`, `s0_pulse.hpp`, `i2c.hpp`, `projectile.hpp`, `signal.hpp`, `registry.hpp`, `solver_registry.hpp`, `run.hpp`, `signal_control.hpp` | Pure stepping/calculation functions, e.g. `OhmLawSolver::solve_from_ui/ir/ur`, `S0PulseSolver::frequency_hz/power_w_from_frequency/pulses_from_power_time`, `I2cBusSolver::solve_pullup/solve_series`, `ProjectileSolver::step`, `SignalSolver::step` — no I/O; compile-time `SolverRegistry` catalog (mirrors the CLI registry); `ControlBus` step-boundary command drain with fixed channel order, last-writer-wins, and replay log (SIG-10) |
| `signal` | `wengine/signal/types.hpp`, `expr.hpp`, `table.hpp`, `lua.hpp`, `control.hpp` | Signal kinds/params/quantity tags, pure `eval_signal` closed forms, seeded splitmix64 noise, kind/quantity parsing + validation, single quantity table + `DimensionFor`/`stamp` typed binding, sum combinator (operands by stable serial), closed expression programs (RPN, from_chars parsing, div-by-zero/out-of-domain to Status), table CSV replay (linear interp, clamped, bit-exact hits), Lua scripting source (sandboxed 5.4 runtime in an isolated TU — this header is Lua-free), control-plane line protocol vocabulary (strict verb parsing + response builders, no state, no I/O) — no I/O; pool lives in `world` |
| `core` | `wengine/core/status.hpp`, `solver.hpp`, `runner.hpp`, `run_config.hpp` | Unified errors: `Status`/`StatusCode` + `Result<T>` (no exceptions/heap strings), `to_exit_code`/`kExitOk/Runtime/Usage` mapping, `ISolver::step(State&, dt)` interface, `Runner` fixed-step loop (`run`/`advance`) |
| `world` | `wengine/world/entity.hpp`, `particles.hpp`, `signals.hpp`, `state.hpp`, `serialize.hpp` | `Entity` registry (freelist + generations), `Particles` SoA pool, `Signals` SoA pool (stable serials, sum operands, expression programs, append-only table/script stores, entity-attached), `State` (clock + entities + pools), deep-copy `Snapshot` (+ serial-carrying signal arrays, `select_signals` subset filter), versioned binary save/load (v5; v4 loads with empty script ranges, v3 with zeroed custom columns, v2 with synthesized serials, v1 with empty signals) |
| `link` | `wengine/link/frame.hpp`, `protocol.hpp`, `socket_publisher.hpp`, `shm_publisher.hpp`, `control_server.hpp`, `uart_codec.hpp`, `uart_publisher.hpp` | Solver-agnostic `Frame` adapters + versioned wire codec; app-edge SHM publisher and POSIX TCP/UDP stream publishers (`wengine_link_net`), exact version/domain gates, compact UDP frames, and lag/loss accounting; domain-free TCP line server for the control plane (same package); UART serial publisher + control pump with COBS framing (same package) |
| `emit` | `wengine/emit/sink.hpp`, `console_sink.hpp`, `csv_sink.hpp`, `signal_sink.hpp`, `routing.hpp` | `ISink::on_snapshot()` observers: human-readable console lines, replay-grade CSV log (`step,time,slot,x,y,vx,vy`, full double precision), signals CSV log (`step,time,sig_id,value,quantity`, exact round-trip + parser), routing wrappers (`SignalSubsetSink` serial subscriptions, `StrideSink` step-aligned decimation, drop counters) |
| `physics` | `wengine/physics/components.hpp` | Component declarations solvers operate on |
| `cli` | `wengine/cli/args.hpp`, `command.hpp`, `command_registry.hpp`, `commands.hpp`, `complete.hpp`, `shell.hpp`, `line_reader.hpp` | Zero-copy parser + compile-time registry + `Shell` REPL with history and Tab completion |

Dependency direction (never reversed):

```text
solvers -> units (+ glm directly where needed, no wrapper)
world   -> (standalone state)
emit    -> world
cli     -> solvers, core   (thin adapter, owns all iostream use)
apps    -> cli + core
```

### Math core and dependency policy (revised 2026-09-22)

**Decision: GLM (`external/glm` git submodule, pinned 1.0.1, MIT,
header-only) as the math library — the engine's only third-party
dependency.** This supersedes the same-day header-only decision: the
hand-rolled `Vector2D/3D` core is scratched in favor of battle-proven
library code. Rationale: GLM is C++-native with first-class `double`
types (`dvec`/`dmat` match the engine's all-`double` convention),
allocation-free, SIMD-friendly, and covers the roadmap needs (vectors,
matrices, quaternions) without speculative home-grown algebra. GLM is
consumed directly as `glm::` types — there is no `wengine/math` wrapper
and no alias layer. Rules:
the pin moves only by explicit decision; golden tests are implicitly
pinned to the submodule commit (re-derive goldens on bumps, never widen
tolerances); no second third-party dep without its own decision record.

GUI backend decision (PHYSI-13): Dear ImGui (`external/imgui` submodule,
pinned v1.91.9b, MIT) + SDL2 windowing + Vulkan rendering — the authorized
second third-party dep. Chosen over SDL2/OpenGL and GLFW variants for
explicit GPU control and the highest perf ceiling (validation-layer
support, no hidden driver sync, compute-shader future for fields/particles)
at the cost of Vulkan bootstrap code owned by `apps/gui`. SDL2 and the
Vulkan loader are system packages, not vendored. CMake: `wengine_imgui`
(`STATIC`, `SYSTEM` includes so third-party headers stay warning-clean);
`WEngineGui` links `wengine_core` directly, never `wengine_cli`, and owns
all I/O. No engine headers change for GUI work.

Lua scripting decision (SIG-4b, 2026-09-24): PUC Lua (`external/lua`
submodule, pinned v5.4.9, MIT) — the authorized third third-party dep.
Rationale: Lua covers arbitrarily complex custom waveforms without growing
the expression language; the 5.4 interpreter line is auditable and
deterministic where LuaJIT's codegen/warmup is not, and 5.5.x is too fresh
to pin. Vendored + built from source (no system headers needed):
`wengine_lua` (`STATIC`, interpreter core + base/math/string libs only,
compiled as C++, `SYSTEM` includes) plus `wengine_signal_lua` (`STATIC`:
`src/signal/lua_solver.cpp`, the single TU allowed to know Lua exists,
strict warnings) — engine headers stay Lua-free. Sandbox (math−random,
string−dump/format behind read-only proxies, small pure base; fresh
`_ENV` per call so scripts are pure functions of `t/seed/step/dt`;
instruction-count budget; every failure is a loud step Status) and the
5.4.9 pin (asserted in code and test) are locked in `docs/signals.md`
sec. 7. No fourth third-party dep without its own decision record.

CMake targets mirror this: `wengine_glm` (third-party import), `wengine_units`,
`wengine_solvers`, `wengine_world`, `wengine_emit`, `wengine_link` (link wire
protocol, header-only, OS-free) (all `INTERFACE`) combine
 into `wengine_core` (`wengine::core` alias). `wengine_link_shm` (`STATIC`:
  `src/link/shm_publisher.cpp`, the POSIX endpoint TU) and `wengine_link_net`
  (`STATIC`: `src/link/socket_publisher.cpp`, POSIX TCP/UDP endpoints, plus
  `src/link/control_server.cpp`, the POSIX TCP control-plane line server,
  plus `src/link/uart_publisher.cpp`, the POSIX UART serial publisher +
  control pump) link
  `wengine_core`.
 `wengine_signal_lua` (`STATIC`: `src/signal/lua_solver.cpp`, the Lua
 runtime TU) links `wengine_core` + `wengine_lua` (vendored interpreter).
 `wengine_cli` (`STATIC`: `src/cli/shell.cpp`,
 `src/cli/commands/ohm.cpp`, `src/cli/commands/s0.cpp`,
 `src/cli/commands/i2c.cpp`, `src/cli/commands/sim.cpp`,
  `src/cli/commands/sig.cpp`) links `wengine_core`, `wengine_link_shm`,
  `wengine_link_net`,
 and `wengine_signal_lua` (for `sig run --sig-lua`);
`PhysicsSimApp` links `wengine_cli`. Tests: `test_ohm_law`, `test_math_units`
(link core), `test_cli_args`, `test_s0`, `test_i2c`, `test_sim` (link cli),
`test_solver_registry`, `test_runner`, `test_signal` (waveform goldens,
seeded-noise sequence, solver end-of-step convention, headless signal
config + rejections, signal Frame adapter + wire round-trip, signals CSV
round-trip, expression goldens + div-by-zero step freeze, table CSV replay
+ byte-exact hits, custom headless batches + rejections, v5/v4/v1
serialization), `test_lua` (5.4.9 pin, sandbox rejections incl. read-only
proxies, instruction budget, Lua sine golden vs parametric sine, inputs,
determinism + save/load/continue purity, headless extras + rejections, sum
over Lua, shipped `examples/lua/` goldens, v5 round-trip + v4 fallback), `test_routing` (serial stability,
1:N/N:1 subscriptions, mid-run reconfig, stride alignment, sum golden +
operand rejections, dangling-operand failure, sink-failure isolation, v3
round-trip + v2 fallback), `test_world`, `test_emit`,
`test_link_protocol` (solver-agnostic items/channels wire frame, particle
adapter, encode/decode exactness, seqlock states incl. threaded hammer,
version gate, lag accounting), `test_link_shm` (segment create/publish/
 read-back, headless fan-out exactness, drop-oldest, failure paths, reader
 attach/poll), `test_link_bridge` (CSV parse fidelity + rejection, inverse
    adapter, CSV->bus->CSV byte-identity),
    `test_link_receiver` (reorder window + duplicates, session resets,
    pre-attach/skipped/confirmed separation, trailing-loss gating),
    `test_link_sockets` (TCP hello/version
   gate, length-prefixed loopback exactness, UDP compact codec/datagrams,
   UDP payload budget (exact fit/one-over, endpoint forms, representative
   sizes),
    transient backpressure, partial-TCP-frame preservation, sub-Hz/100-kHz soaks
     and lag accounting),     `test_link_sample_block` (batched consecutive ticks
    without decimation: golden wire bytes, schema-change seals, empty
    selections, partial blocks, flatten(batch(stream)) equality, >63
    channels, over-bound/malformed refusal, cross-kind frame/block refusal),
    `test_output_queue` (bounded owning SPSC block queue: FIFO/wraparound,
    slot + byte-budget refusal, shutdown drain, post-enqueue immutability,
    reusable slot storage, threaded producer/consumer cover),
    `test_output_health` (per-cause live-output
   accounting, shared health report, over-bound/disconnect fault injection,
   telemetry-failure numerical isolation), `test_sim` also validates `--emit-shm` wiring,
    TCP/UDP endpoint syntax, per-output strides/selects (global+per-output
    intersection, full-rate CSV + decimated live output, capacity refusal),
    UDP budget wiring/diagnostics +
   `sig run` expression/table/lua goldens and usage errors (incl. sandbox
   failure surfacing), `--control-tcp` flag/completion/shape errors,
   step-synchronous control drive + summary,
`test_signal_control` (control-plane verb parsing, add/set/start/stop/
  drop/list/hello/ping/select + table/lua refusals, idempotent
  start/stop, phase-exact pause/resume incl. stopped save/load/resume,
  pool-untouched refusals, sum add + dangling-operand step failure,
  fixed channel order with last-writer-wins, two-channel replay-log
  exactness), `test_control_server` (loopback handshake/version gate,
  mid-run set/start/stop over TCP, malformed + overlong rejection,
  two-client conflict order, LIST census, disconnect survival, `sig run
  --control-tcp` CLI smoke, control-client transport classes, full
  `sig list/add/set/start/stop/drop` family live incl. sum + expression
  adds, census proof of SET values, unknown-serial and refused-connection
  exits), `test_uart_codec` (COBS vectors, endpoint/
  mode parsing, stream line goldens, ascii/cobs demux + resync, Zephyr C
  parser parity incl. compact v2 decode of real encoder bytes),
  `test_uart_pub` (pty-loopback ASCII/COBS exactness, UART control
  round-trips through the bus, garbage resync, flood drops without
  stalls, decimated soak + `sig run`/`sim run` UART CLI smoke and usage
  errors),
`test_determinism` (fixed-dt goldens, exact repeat, CSV replay),
`test_gui_sim_control` (GUI controller bit-parity with headless + goldens),
`test_signal_panel` (signal panel model validation/config + controller
parity for signal runs, incl. expression/table/lua batch, verb-session
parity vs headless bus, verb rejections), `test_signal_panel_lua`
(controller Lua runtime wiring: batch parity, failure text, rejection
matrix),
`test_closed_form_panels` (GUI panel models match CLI text + goldens),
`test_viewport3d` (orbit camera projection, culling, grid, guards),
`test_frame_source` (source registry, channels, ring,
particle + signal CSV-exactness, sink re-attach, plot bounds),
`test_gui_stream_source`
(SHM subscribe parity + lag isolation, CSV replay transport/pacing,
CLI/frame/CSV exactness, signal-log replay), `test_canvas2d` (2D mapping,
fit, zoom/pan, grid steps). Benchmarks:
`bench_ohm`, `bench_s0`, `bench_i2c`, `bench_projectile`, `bench_signal`
(ns/source-step over a live Runner + SignalSolver, opaque params), `bench_link`
(publish ns/frame + frames/s through a live SHM segment, slow-reader
published/received/missed/max_gap), `bench_link_sockets` (TCP/UDP loopback
frames/s, published-local/dropped/received bytes and reader lag; args select mode, frames,
 and particle count).
 `bench_sample_block` (batched-tick bytes/sample vs per-step frames,
 builder + encode cost; args select ticks, channels, samples-per-block).

## Flow

CLI path today:

```text
main -> Shell::run
  -> getline into owned std::string (1 alloc/line, unavoidable)
  -> tokenize_into() to string_view[64] (zero-copy, quotes aware)
  -> builtin? (help/exit)
  -> CliRegistry::find(name) (constexpr linear search)
  -> Spec::run(ArgCursor&, ostream&)
     -> take_option/take_flag/next_positional (consumed-bitmask, no alloc)
     -> parse_* via std::from_chars + suffix tables
     -> OhmLawSolver::solve_from_* (constexpr, noexcept)
     -> print to ostream
```

Simulation path: `core::Runner` owns `world::State` and advances it in
fixed-`dt` steps — `run(N)` for exact batch/headless runs, `advance(frame_dt)`
with an accumulator for real-time loops. Each step calls registered
`ISolver::step(state, dt)` implementations in order, then `take_snapshot()`
hands an immutable `Snapshot` to attached `emit::ISink` observers
(decimated via `emit_every`). Engine code never knows its consumers.
Headless runs go through the `core::RunConfig` seam: frontends (CLI today,
GUI tomorrow) build the plain-data config and call
`solvers::run_headless()` with caller-owned sinks — the engine never sees
frontend types.

### Emit transport decision (recorded, see `docs/delivery.md`)

Evaluated for getting snapshots to external apps / GUI:
file+CSV replay, stdout piping, TCP/UDP sockets, POSIX shared-memory
seqlock ring, ZeroMQ, gRPC. **Decision: shared-memory seqlock snapshot bus
(`wengine-link`) for realtime local consumers; CSV/file as the durable
log + replay format; TCP/UDP socket publishers for the networked
consumer (Zephyr over IP, now shipped).** Rationale: the GUI is local,
performance is the project's identity, and the no-dependency policy holds
— SHM gives sub-µs zero-copy publish with drop-oldest semantics fitting
visualization, while the file log covers determinism, replay, and
debugging for free. Tracked as a separate Plane epic (link protocol,
writer, reader, replay bridge, versioning). Wire versions exact-match at
attach: a reader refuses a foreign segment immediately with the found
version reported
(`segment 'X' has version 1, this reader supports version 2`); version 0
is reserved as the mid-construction marker and never assigned.

Delivery promises and clocks are defined in `docs/delivery.md`:
latest-state viewing (drop-oldest/take-latest), ordered bounded sample
streaming (bounded, counted drops), and complete recording
(driver-paced/backpressured or loud failure) — over the independent
simulation clock (`dt`), emitted sample rate, wall-clock packet rate,
and UI refresh rate (waveform frequency is none of those).

## Conventions

- Files: `snake_case.hpp`/`.cpp`; types: `PascalCase` (`OhmLawSolver`,
  `ArgCursor`); functions/variables: `snake_case` (`solve_from_ui`,
  `take_option`); constants: `kName` style; namespaces:
  `wengine::<module>` (`wengine::cli`, `wengine::solvers`, ...).
- Headers: `#pragma once`, one public header per concept under
  `include/wengine/`, matching `src/` implementation file. No
  `using namespace` in headers.
- Hot path (math/solvers/parser): header-only, `constexpr`/`noexcept`
  where possible, `[[nodiscard]]` on results; no virtuals, no
  `<sstream>`/`<stoi>`/regex/exceptions — `std::from_chars`,
  `string_view`, `span`, `core::Result<T>`/`Status` instead. One
  defensive `try/catch` lives only at the `Shell::execute` boundary and
  maps to `kInternal`.
- Solvers: pure functions over `units::Value<Dim>` (SI base internally);
  never touch `iostream`, CLI types, or `world` I/O directly.
- Errors (unified): `core::Status` (void ops) + `core::Result<T>`
  (value ops) everywhere fallible;
  `std::optional` only for absence, `bool` only for predicates/state
  queries, solver `inf`/`feasible` are domain values. CLI handlers keep
  signature `int run(ArgCursor&, std::ostream&)` but produce the `int`
  only via `core::kExitOk/kExitRuntime/kExitUsage` and the shared
  `cli/status.hpp` helpers (`usage_error`/`runtime_error`/
  `unexpected_error`); `-1` is the REPL exit signal.
- Commands: spec struct (`kName`, `kDescription`, `kUsage`, `run`) +
  registration in `command_registry.hpp` as
  `using CliRegistry = Registry<...>`; duplicates/empty names fail at
  compile time via `consteval` validation.

## Adding a command / solver

1. New solver: add `include/wengine/solvers/<name>.hpp` with
   `static constexpr` solve functions on `Value<Dim>` types.
2. New command: declare spec in `include/wengine/cli/commands.hpp`,
   implement `run` in `src/cli/commands/<name>.cpp`, append the spec to
   `CliRegistry`, add the `.cpp` to `wengine_cli` sources in
   `CMakeLists.txt`.
3. Add `tests/test_<name>.cpp` + `add_executable`/`add_test` entry.

## Layout

```text
apps/cli/main.cpp  apps/gui/main.cpp (thin assembly: args, backend
  lifetime, frame loop, window layout)
  apps/gui/backend/vk_context.hpp/.cpp (SDL2 + Vulkan bootstrap, frames)
  apps/gui/controllers/sim_controller.hpp (engine-only RunConfig driver)
  + external_source.hpp (SHM subscriber + CSV replay, engine-only)
  apps/gui/models/closed_form_models.hpp (ohm/s0/i2c evaluate logic)
  + signal_model.hpp (batch signal config: CLI-parity validation + RunConfig,
  incl. expression/table/lua payloads)
  apps/gui/panels/sim_panel.hpp + source_panel.hpp (frame-source selector)
  + signal_panel.hpp (batch Apply + live source verbs over the SIG-10 bus)
  + closed_form_panels.hpp/.cpp (ImGui UI)
  apps/gui/views/ frame_source.hpp (solver-name adapter registry over the
  shared link::Frame view-model, frame history + recorder sink), camera3d.hpp
  (orbit/project math) + viewport3d_panel.hpp (ImGui 3D rasterization) +
  graphs_panel.hpp (channel selector + time-series plots over frames) +
  worldview2d.hpp (2D fit/zoom/pan math) + canvas2d_panel.hpp (top-down
  view with trails) + palette.hpp (shared entity colors).
  Views consume link::Frames only: new solvers add one adapter function
  plus one registry entry, and panels stay untouched. Signal runs graph
  through the shared path (signal Frame adapter + channel selector, which
  defaults to the first signal channel). Frames come from the
  in-process sim, an SHM-subscribed headless run, or a CSV replay (source
  panel); switching sources clears the shared history (one clock per view).
  Startup layout is 1600x900: sim + live views
  open, calculators and plots collapsed one click away.
benchmarks/bench_{ohm,s0,i2c,projectile,signal}.cpp
examples/cli_smoke.cpp + link_sample.cpp (SHM sample reader, built with CLI)
  + link_bridge.cpp (CSV<->bus replay bridge: play/record, built with CLI)
  + link_fastread.cpp (minimum-loss subscriber: no per-frame print, reused
  buffers, lag/loss summary; built with CLI)
  + link_tcp_signal_receiver.py + link_udp_signal_receiver.py (TCP/UDP
  signal receivers; link_socket_protocol.py has the shared decoder,
  link_receiver_accounting.py the shared reorder/session accounting)
include/wengine/{cli,core,emit,link,physics,signal,solvers,units,world}/  (no math dir: glm used directly)
src/{cli,link,solvers,...}/  (cli + link filled; others reserved for .cpp as needed)
tests/test_{ohm_law,math_units,cli_args,s0,i2c,sim,solver_registry,signal,lua,routing,runner,world,emit,link_protocol,link_shm,link_sockets,output_health,link_compact,link_sample_block,output_queue,link_receiver,link_bridge,run_config,paced,determinism,gui_sim_control,signal_panel,signal_panel_lua,gui_stream_source,closed_form_panels,viewport3d,frame_source,canvas2d}.cpp
cmake/ docs/ (reserved/empty)
external/glm/ (git submodule, pinned 1.0.1 — first third-party dep, see policy above)
external/imgui/ (git submodule, pinned v1.91.9b — GUI-only second dep, see policy above)
```
