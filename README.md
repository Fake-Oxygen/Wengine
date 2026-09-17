# WEngine

High-performance C++20 physics simulation engine. Currently CLI-driven;
a GUI and external-app streaming layer are planned. The core is a set of
header-only, allocation-free modules (math, units, solvers) with a thin
CLI adapter on top.

## Requirements

- CMake >= 3.20
- C++20 compiler (developed/tested with GCC 13.3)
- No external dependencies (tests use plain `assert`, no gtest fetch)

## Build

Out-of-source builds only (in-source is rejected by `CMakeLists.txt`):

```sh
cmake -S . -B build
cmake --build build
```

Options (all default `ON`):

```sh
cmake -S . -B build -DBUILD_CLI=ON -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
```

Run tests and benchmark:

```sh
ctest --test-dir build --output-on-failure
./build/bench_ohm
```

`compile_commands.json` is exported to `build/` for clangd/clang-tidy.

## Run (CLI)

```sh
./build/PhysicsSimApp
```

REPL commands: `help [cmd]`, `exit`/`quit`, plus registered solvers.
Return codes: `0` ok, `1` runtime/unknown-command error, `2` argument/usage
error, `-1` internal exit signal.

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
`Wh`/`kWh`; power `W`/`kW`; time `s`/`ms`/`min`/`h`.

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
| `math` | `wengine/math/vector_*.hpp` | `Vector2D<T>`/`Vector3D<T>` header-only structs, no virtuals |
| `units` | `wengine/units/units.hpp`, `converter.hpp` | `Dimension<...>`, `Value<Dim>` with dimensional `*`/`/` operators, `_V`/`_A`/`_Ohm` literals |
| `solvers` | `wengine/solvers/ohm_law.hpp`, `s0_pulse.hpp` | Pure `static constexpr` functions, e.g. `OhmLawSolver::solve_from_ui/ir/ur`, `S0PulseSolver::frequency_hz/power_w_from_frequency/pulses_from_power_time` — no I/O |
| `core` | `wengine/core/status.hpp`, `solver.hpp` | `Status`/`StatusCode` (no exceptions/strings in hot path), `ISolver::step(dt)` interface |
| `world` | `wengine/world/state.hpp` | `State` (SoA vectors, `sim_time`, `step_index`), `Snapshot` + `take_snapshot()` |
| `emit` | `wengine/emit/sink.hpp` | `ISink::on_snapshot()` — how external apps/GUI consume the sim |
| `physics` | `wengine/physics/components.hpp` | Component declarations solvers operate on |
| `cli` | `wengine/cli/args.hpp`, `command.hpp`, `command_registry.hpp`, `commands.hpp`, `shell.hpp` | Zero-copy parser + compile-time registry + `Shell` REPL |

Dependency direction (never reversed):

```text
solvers -> units, math
world   -> (standalone state)
emit    -> world
cli     -> solvers, core   (thin adapter, owns all iostream use)
apps    -> cli + core
```

CMake targets mirror this: `wengine_math`, `wengine_units`,
`wengine_solvers`, `wengine_world`, `wengine_emit` (all `INTERFACE`) combine
into `wengine_core` (`wengine::core` alias; `PhysicsEngineCore` is a legacy
alias). `wengine_cli` (`STATIC`: `src/cli/shell.cpp`,
`src/cli/commands/ohm.cpp`, `src/cli/commands/s0.cpp`) links `wengine_core`; `PhysicsSimApp` links
`wengine_cli`. Tests: `test_ohm_law`, `test_math_units` (link core),
`test_cli_args`, `test_s0` (link cli). Benchmark: `bench_ohm`.

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

Planned simulation path (stubs exist): solvers step `world::State`
(`ISolver::step`), `take_snapshot()` produces an immutable `Snapshot`,
`emit::ISink` implementations (file/IPC/network/GUI) consume it. Engine
code never knows its consumers.

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
  `string_view`, `span`, `ParseResult<T>`/`Status` instead. One
  `try/catch` lives only at the `Shell::execute` boundary for `ostream`
  errors.
- Solvers: pure functions over `units::Value<Dim>` (SI base internally);
  never touch `iostream`, CLI types, or `world` I/O directly.
- CLI handlers: signature `int run(ArgCursor&, std::ostream&)`; return
  `0`/`2` (usage) and print `kUsage` on error.
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
apps/cli/main.cpp  apps/gui/ (empty)
benchmarks/bench_ohm.cpp
examples/cli_smoke.cpp
include/wengine/{cli,core,emit,math,physics,solvers,units,world}/
src/{cli,solvers,...}/  (cli filled; others reserved for .cpp as needed)
tests/test_{ohm_law,math_units,cli_args,s0}.cpp
cmake/ docs/ external/ (reserved/empty)
```
