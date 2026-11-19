# Graphical app

The GUI (`WEngineGui`: Dear ImGui over SDL2 + Vulkan) drives the same
engine as the CLI through its own panels and views. Solver logic lives
in engine-only models; panels only render and rebuild runs, and views
only read solver-agnostic frames — external sources own no sim state,
so lag and failure stay on the display side by construction.

## What was verified on this machine

- The GUI binary was already built at `build-gui/WEngineGui`
  (prerequisites `libsdl2-dev` + `libvulkan-dev` are present on this
  host; no rebuild was needed).
- `./build-gui/WEngineGui --help` was executed and prints the usage
  below (exit 0).
- A real windowed launch was attempted with a display available
  (`DISPLAY=:0`, `WAYLAND=wayland-0`): the application started, logged
  `[vulkan] GPU: llvmpipe (LLVM 20.1.2, 256 bits)`, and kept running
  until stopped after several seconds. No screenshot tooling exists, so
  the panel walkthrough below is written from the shipped sources
  (`apps/gui/main.cpp`, `apps/gui/panels/*.hpp`,
  `apps/gui/views/*.hpp`) and describes the built application as
  shipped — reproduce it with a windowed run.

## Build and launch

If `build-gui/` is missing, configure and build it (GUI defaults off):

```sh
cmake -S . -B build-gui -DBUILD_GUI=ON -DBUILD_TESTS=OFF
cmake --build build-gui --target WEngineGui
```

The only accepted flag is `--help`:

```console
$ ./build-gui/WEngineGui --help
WEngineGui (Dear ImGui + SDL2 + Vulkan)
Usage: WEngineGui [--help]
Opens the engine GUI.
```

Unknown arguments are a usage error (exit 2); startup failures such as
a missing display exit 1. Exit codes otherwise mirror the CLI
convention. Closing the window ends the program with exit 0.

## Window layout

`main.cpp` opens one 1600x900 window holding, from left to right:

- **WEngine** — sim control, signal batch/sources, frame source.
- **Solvers** (starts collapsed) — closed-form calculator tabs.
- **Graphs** (starts collapsed) — channel plots over run history.
- **2D Canvas** — top-down world view with trajectory trails.
- **3D Viewport** — orbit/zoom/pan perspective scene.

Play mode folds wall-clock time into fixed steps through the shared
controller, so Reset + Play matches a headless run bit for bit.

## Panels

### Simulation (stepped solvers) — `panels/sim_panel.hpp`

Mirrors `sim run`: a solver combo (populated from the compile-time
registry, so new solvers appear with no panel change), `dt`, step
budget, particle count, emit stride, gravity, and initial conditions,
plus **Play / Pause / Step / Reset** transport. Every accepted field
edit rebuilds the run from step 0 — the only deterministic semantic,
since a new `dt` or solver invalidates the old trajectory. Rejected
edits keep the live run and show the reason inline. A status line shows
`step`, `t`, solver, and the primary item's position/velocity (or the
first signal channels for item-less domains), with engine errors in red.

### Signals (batch source + live verbs) — `panels/signal_panel.hpp`

Two sections driving the same controller. The batch section sets kind,
quantity, stamped parameters, expression text, and table/Lua file paths
(files are loaded by the panel — GUI owns file I/O), then **Apply**
rebuilds the run from step 0. The live-verbs section lists live sources
by stable serial with **Start / Stop / Drop** buttons, an ADD form
(parametric + expression + sums with operand serials; table/Lua ride
batch rebuilds, like the CLI family), and a SET form (serial + param +
value, with the same unit-suffix firewall as the CLI). Verdicts show
inline (`OK ...` / `ERR ...`); rejected lines change nothing.

### Frame source — `panels/source_panel.hpp`

Three radio modes select what the views display:

- **in-process** (default) — the panel-driven sim above.
- **SHM subscribe** — attaches to a headless publisher's segment
  (default `/wengine-demo`), showing received/missed lag counters and a
  **Reconnect** button (a same-name publisher restart needs an explicit
  re-attach).
- **CSV replay** — loads a snapshot or signals log with **Load**, then
  **Play / Pause / Step / Reset** plus a replay-rate field.

Switching modes clears the shared history (one clock per view). With no
data yet, views show their empty states. Neither external source owns
sim state: a lagging reader or a bad file degrades only the display.

### Solvers (closed-form tabs) — `panels/closed_form_panels.*`

Three tabs — **Ohm**, **S0**, **I2C** — evaluate the matching engine
models as you type, showing the same numbers as the CLI commands. No
LDO/LED tabs exist yet; use the CLI for those.

## Views

### Graphs — `views/graphs_panel.hpp`

Plots one selected channel over the shared frame history. The channel
combo lists the latest frame's schema and falls back to the source's
default channel (signal-aware, so a signal run preselects sensibly).
Series follow the run's emit rate: pausing freezes the plots without
touching sim timing. Empty history shows a hint, not an error.

### 2D Canvas — `views/canvas2d_panel.hpp`

Top-down world view: grid, axes, entity glyphs, velocity vectors, and
trajectory trails from the shared history. Drag pans, wheel zooms;
auto-fit tracks the sim until the first manual move, and **Fit**
resumes tracking. Read-only: navigation never touches sim state.

### 3D Viewport — `views/viewport3d_panel.hpp`

Perspective scene with floor grid, RGB axes, and entity glyphs plus
velocity vectors. Left-drag orbits, right-drag pans, wheel zooms; a
**Track** checkbox follows the primary item. Far-to-near painter order;
camera moves cannot perturb the simulation.

## Keyboard and layout notes

Keyboard navigation is enabled ImGui-wide. The layout is not persisted
between launches (no ini file); windows open at fixed positions on
first use. All panels sharing a window suffix widget labels per panel,
so duplicate names cannot collide.
