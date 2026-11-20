# Architecture

How WEngine is organized, how the pieces depend on each other, and what
happens during one simulation step. This page maps the codebase; it does
not repeat the design rationale that lives in the topic guides.

Related guides: [Signal subsystem](../signals.md),
[Delivery guarantees](../delivery.md), [ROS 2 bridge](../ros2.md),
[Zephyr HIL](../zephyr-hil.md), [TUI](../tui.md), [Logic module](../logic.md),
[Domain outputs](../domain-outputs.md).
API references (planned): [API: core](../api/core.md),
[API: world](../api/world.md), [API: emit](../api/emit.md),
[API: link](../api/link.md).

## Module map

Public engine headers live under `include/wengine/`, one directory per
module. Engine modules are `INTERFACE` (header-only) libraries unless
there is a concrete reason to add a translation unit:

| Directory | Contents |
|---|---|
| `units/` | `Dimension`, `Value<Dim>`, arithmetic, literals (`units.hpp`); suffix-to-scale tables (`scales.hpp`) |
| `solvers/` | Closed-form solvers (`ohm_law`, `s0_pulse`, `i2c`, `ldo`, `led`), stepped solvers (`projectile`, `signal`, `sequential`), `solver_registry.hpp`, `registry.hpp`, `run.hpp`, `configured_run.hpp`, `paced_run.hpp`, `signal_control.hpp`, plus `ic/` part-specific datasheet solvers |
| `signal/` | Signal kinds, parameters, quantity table and typed stamping (`types.hpp`), expression programs (`expr.hpp`), table replay (`table.hpp`), Lua source vocabulary (`lua.hpp`), control-plane line vocabulary (`control.hpp`) |
| `core/` | `Status`/`Result` errors, `ISolver` interface, `Runner` fixed-step loop, `RunConfig` headless-run seam |
| `world/` | Entity registry, `Particles` and `Signals` SoA pools, `State` and deep-copy `Snapshot`, versioned binary `serialize.hpp` |
| `emit/` | `ISink` observer interface, console/CSV/signals-CSV sinks, `routing.hpp` subset/stride wrappers |
| `link/` | Solver-agnostic `Frame` view-model and wire codec (`frame.hpp`, `protocol.hpp`), SHM/TCP/UDP/UART publishers, control server, receiver and worker helpers |
| `physics/` | Component declarations that solvers operate on |
| `logic/` | Gate netlist, evaluators, sequential pool (`seq_state.hpp`, `seq_step.hpp`), YAML store vocabulary |
| `cli/` | Zero-copy argument parsing, command specs, compile-time registries, `Shell` REPL, `ic_dispatch.hpp` two-level dispatch |
| `tui/` | Ncurses front end: executor, async worker, forms, batch builders, views |
| `ros2/` | Transport-neutral ROS 2 state-out sink, control and lifecycle vocabulary |

Platform code lives in `src/`, and only there:

- `src/cli/*.cpp` implements the CLI commands (one file per command, plus `src/cli/commands/ic/<chip>.cpp` per part).
- `src/link/*.cpp` holds the POSIX SHM/socket/UART endpoints. This is the
  established exception to the header-only rule: syscalls live in these
  translation units while the headers stay platform-free, so a future
  Windows port replaces the TU and never the header.
- `src/signal/lua_solver.cpp` is the single TU allowed to know Lua exists.
- `src/logic/yaml_store*.cpp` are the store TUs allowed to know yaml-cpp exists.
- `src/ros2/` and `src/tui/` hold the ROS 2 transport and TUI domain TUs.

The GUI lives in `apps/gui/`, layered so each layer has one job:

- `backend/` owns SDL/Vulkan lifetime and frame render/present.
- `controllers/` own the `Runner` plus solver, rebuild from `RunConfig`,
  and re-attach non-owning sinks after every rebuild. External sources
  (`ShmSource`, `CsvReplay`) own no simulation state.
- `models/` holds engine-only logic with no ImGui includes, unit-tested by
  linking `wengine_core` alone.
- `panels/` renders ImGui controls for one feature area.
- `views/` consume the `link::Frame` view-model only: 2D canvas, 3D
  viewport, and graphs. New solver data arrives through one adapter
  function plus one frame-source registry entry; views stay untouched.
- `apps/gui/main.cpp` stays thin: argument handling, backend lifetime,
  the frame loop, and window layout.

## Dependency direction

Dependencies flow one way. Do not reverse them without an explicit
architectural decision:

```text
solvers -> units, math
world   -> standalone state
emit    -> world
link    -> world (+ core/status vocabulary; endpoints attach as emit::ISink
           at the app edge, never inside the engine)
cli     -> solvers, core
core/solver.hpp -> world
core/run_config.hpp -> signal (one aggregated section type per solver)
signal  -> units, core/status (leaf quantity/kind/eval types only)
world/solvers/emit/cli/gui -> signal (leaf types + pool; never the reverse)
apps    -> cli + core
apps/gui -> wengine_core + wengine_imgui + wengine_link_shm (SHM endpoints
            only) + wengine_signal_lua (app-owned scripting runtime extra,
            created in main and lent to the controller — never in engine
            headers) (never the wengine_cli library; header-only cli/args.hpp
            parsing may be included — it adds no link dependency and reads
            the same units tables)
views   -> link::Frame view-model only (never Snapshot internals;
            Frame is shared with the link wire encoder, see link/frame.hpp)
```

Three rules in that diagram deserve emphasis. First, engine and solver
code never perform application I/O: the CLI owns interaction, sinks own
output, and engine code exposes data and `Status` instead. Second,
publishers are `ISink` objects owned at the app edge, never members of
`RunConfig`, so the engine never knows its consumers. Third, GUI views
never see `Snapshot` internals or wire structs; the shared `link::Frame`
type is the single projection both views and the wire encoder consume.

## Life of a simulation

A run passes through five stages, each owned by a different layer:

1. **Configuration.** A frontend (CLI today, GUI panels tomorrow) fills
   a plain-data `core::RunConfig`: loop tuning (`dt`, `emit_every`),
   the solver registry name, and one nested section struct per solver
   (for example `signal::SignalRunConfig`). Sinks are not part of the
   config; the caller creates and owns them and passes them to the
   driver. See [API: core](../api/core.md) for the struct layout.
2. **State.** `world::State` owns the live simulation: the clock
   (`sim_time`, `step_index`), the entity registry, and one SoA pool
   per domain (`Particles`, `Signals`, sequential state). Stepped
   solvers mutate this state in place through the `core::ISolver`
   contract.
3. **Stepping.** `core::Runner` advances the clock in fixed `dt` steps.
   `run(N)` takes exactly N steps for batch, headless, and test runs;
   `advance(frame_dt)` folds wall-clock time into fixed chunks with an
   accumulator for realtime loops. Each step calls every registered
   solver in order, validates the clock range before any mutation, and
   latches a solver failure so later calls return it instead of
   compounding a partial step. Rebuilding (or restoring) the run is the
   only way to clear the latch.
4. **Snapshots.** After stepping, the Runner refills one reused
   `Snapshot` buffer (`fill_snapshot`) and hands the immutable
   reference to every attached sink. A snapshot is a deep, independent
   copy in deterministic slot order; it never exposes mutable live
   state and its reference is valid only for the duration of the
   `on_snapshot` call.
5. **Sinks and outputs.** Sinks are best-effort, non-throwing observers:
   console lines for humans, CSV logs for replay, SHM/TCP/UDP/UART
   publishers for live consumers, and edge-side routing wrappers
   (`SignalSubsetSink` for serial subscriptions, `StrideSink` for
   step-aligned decimation). A sink failure never alters simulation
   state or timing. Whoever owns the run loop re-attaches its
   non-owning observers after every rebuild; sinks themselves stay
   unaware of rebuilds.

A headless batch run drives these stages through one seam: the
frontend builds a `RunConfig` and calls `solvers::run_headless()` with
caller-owned sinks (see `solvers/configured_run.hpp` and
`solvers/run.hpp`). The GUI controller uses the same shared builder,
which is why headless and GUI runs agree bit-for-bit for the same
config.

```text
> sim run --steps 100 --particles 1
sim run: solver=projectile g=9.81
steps: 100 x 0.01s -> t=1s
particles: 1
p0: x=10 y=5.04595 vx=10 vy=0.19
```

## Collaborators, per module

- **units** owns dimensions, values, and every suffix table. It
  collaborates with nobody: solvers, signal, and CLI all read from it,
  and the tables stay in exactly one place so parsing can never drift
  between frontends.
- **solvers** own the physics: pure stepped or closed-form functions
  over SI quantities. They collaborate with `units` (types), `world`
  (state they mutate), and `core` (the `ISolver` contract and
  `RunConfig` sections that configure them).
- **signal** owns leaf vocabulary only: kinds, parameters, the quantity
  table, and pure evaluation. It collaborates with `units` and
  `core/status`; the live pool lives in `world`, never here, and the
  dependency flows toward signal, never back. Full design:
  [Signal subsystem](../signals.md).
- **core** owns the engine skeleton: errors, the solver interface, the
  Runner loop, and the headless-run config. It collaborates with
  `world` (state it steps) and `signal` (one aggregated section type
  per solver in `RunConfig`).
- **world** owns live state and its frozen copies. It collaborates with
  `signal` (pool column types) and nothing else; it is deliberately
  standalone so every other layer can depend on it safely.
- **emit** owns output. It collaborates with `world` (snapshots it
  reads, never retains) and offers the `ISink` interface every
  publisher implements.
- **link** owns transport. It collaborates with `world` (snapshots it
  adapts into `Frame`s) and `core/status` (sticky failure vocabulary),
  and its endpoints attach as `emit::ISink` at the app edge. Delivery
  promises and clock separation: [Delivery guarantees](../delivery.md).
- **physics** declares the components solvers operate on; it is data
  shaped by solver needs.
- **logic** owns the gate netlist, evaluators, and clocked sequential
  state. Design and YAML workspace format: [Logic module](../logic.md).
- **cli** owns user interaction: parsing, the compile-time command
  registry, and the `Shell` REPL. It collaborates with `solvers` and
  `core` and owns all `iostream` use.
- **tui** is a terminal front end over the same commands: every line
  runs through the shared executor so TUI text stays byte-identical to
  the CLI by construction. Details: [TUI](../tui.md).
- **ros2** is a transport-neutral state-out sink plus control and
  lifecycle vocabulary, with rmw transport isolated in `src/ros2/`.
  Details: [ROS 2 bridge](../ros2.md).

## Error vocabulary

Hot engine paths never throw, never allocate, and never build strings.
Fallible work returns `core::Status` (a code plus a zero-copy literal
view) or `core::Result<T>` where a value is needed. `std::optional`
means absence only, `bool` means predicates only, and solver sentinel
values (`inf`, `0`, `feasible` flags) are domain data, not errors.
The CLI maps `Status` to process exit codes in exactly one place
(`to_exit_code` in `core/status.hpp`):

```text
0  success
1  runtime/check failure
2  usage error
-1 internal exit signal
```
