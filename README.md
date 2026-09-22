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

REPL commands: `help [cmd]`, `exit`/`quit`, plus registered solvers.
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
```

All `sim` parameters are named (`--solver`, `--steps`, `--dt`, `--gravity`,
`--particles`, `--x0/--y0/--vx0/--vy0`, `--emit-csv`, `--emit-every`); the CSV
log carries the full snapshot stream for replay. `--solver` selects from the
compile-time solver registry (default `projectile`; unknown names list what's
available), so new stepped solvers become runnable without touching the CLI —
`--gravity` applies to `projectile` only, particle population is generic setup.

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
| `solvers` | `wengine/solvers/ohm_law.hpp`, `s0_pulse.hpp`, `i2c.hpp`, `projectile.hpp`, `registry.hpp`, `solver_registry.hpp`, `run.hpp` | Pure stepping/calculation functions, e.g. `OhmLawSolver::solve_from_ui/ir/ur`, `S0PulseSolver::frequency_hz/power_w_from_frequency/pulses_from_power_time`, `I2cBusSolver::solve_pullup/solve_series`, `ProjectileSolver::step` — no I/O; compile-time `SolverRegistry` catalog (mirrors the CLI registry) |
| `core` | `wengine/core/status.hpp`, `solver.hpp`, `runner.hpp`, `run_config.hpp` | Unified errors: `Status`/`StatusCode` + `Result<T>` (no exceptions/heap strings), `to_exit_code`/`kExitOk/Runtime/Usage` mapping, `ISolver::step(State&, dt)` interface, `Runner` fixed-step loop (`run`/`advance`) |
| `world` | `wengine/world/entity.hpp`, `particles.hpp`, `state.hpp`, `serialize.hpp` | `Entity` registry (freelist + generations), `Particles` SoA pool, `State` (clock + entities + pools), deep-copy `Snapshot`, versioned binary save/load |
| `emit` | `wengine/emit/sink.hpp`, `console_sink.hpp`, `csv_sink.hpp` | `ISink::on_snapshot()` observers: human-readable console lines, replay-grade CSV log (`step,time,slot,x,y,vx,vy`, full double precision) |
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

CMake targets mirror this: `wengine_glm` (third-party import), `wengine_units`,
`wengine_solvers`, `wengine_world`, `wengine_emit` (all `INTERFACE`) combine
into `wengine_core` (`wengine::core` alias). `wengine_cli` (`STATIC`: `src/cli/shell.cpp`,
`src/cli/commands/ohm.cpp`, `src/cli/commands/s0.cpp`,
`src/cli/commands/i2c.cpp`, `src/cli/commands/sim.cpp`) links `wengine_core`;
`PhysicsSimApp` links `wengine_cli`. Tests: `test_ohm_law`, `test_math_units`
(link core), `test_cli_args`, `test_s0`, `test_i2c`, `test_sim` (link cli),
`test_solver_registry`, `test_runner`, `test_world`, `test_emit`,
`test_determinism` (fixed-dt goldens, exact repeat, CSV replay),
`test_gui_sim_control` (GUI controller bit-parity with headless + goldens). Benchmarks:
`bench_ohm`, `bench_s0`, `bench_i2c`, `bench_projectile`.

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

### Emit transport decision (recorded)

Evaluated for getting snapshots to external apps / future GUI:
file+CSV replay, stdout piping, TCP/UDP sockets, POSIX shared-memory
seqlock ring, ZeroMQ, gRPC. **Decision: shared-memory seqlock snapshot bus
(`wengine-link`) for realtime local consumers; CSV/file as the durable
log + replay format.** Rationale: the GUI is local, performance is the
project's identity, and the no-dependency policy holds — SHM gives sub-µs
zero-copy publish with drop-oldest semantics fitting visualization, while
the file log covers determinism, replay, and debugging for free. Sockets /
message queues are deferred until a networked consumer actually exists.
Tracked as a separate Plane epic (link protocol, writer, reader, replay
bridge, versioning).

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
apps/cli/main.cpp  apps/gui/main.cpp (Dear ImGui + SDL2 + Vulkan blank app
  + sim panel)  apps/gui/sim_controller.hpp (engine-only RunConfig driver,
  unit-tested)  apps/gui/sim_panel.hpp (ImGui panel rendering)
benchmarks/bench_{ohm,s0,i2c,projectile}.cpp
examples/cli_smoke.cpp
include/wengine/{cli,core,emit,physics,solvers,units,world}/  (no math dir: glm used directly)
src/{cli,solvers,...}/  (cli filled; others reserved for .cpp as needed)
tests/test_{ohm_law,math_units,cli_args,s0,i2c,sim,solver_registry,runner,world,emit,determinism,gui_sim_control}.cpp
cmake/ docs/ (reserved/empty)
external/glm/ (git submodule, pinned 1.0.1 — first third-party dep, see policy above)
external/imgui/ (git submodule, pinned v1.91.9b — GUI-only second dep, see policy above)
```
