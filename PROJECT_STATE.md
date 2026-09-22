# PROJECT_STATE.md — WEngine status snapshot

Living status file: where the project stands, what is open, and what
needs verification, fixing, or reworking. Refresh the counts and the
commit pointer whenever they move. Architectural rules belong in
`AGENTS.md`, not here.

- Date: 2026-09-22
- Tree: clean at `aca9c51` ("Decouple GUI views from particle solver")
- Tests: 17/17 pass, zero warnings (default build; strict flags everywhere)
- GUI build (`-DBUILD_GUI=ON`): clean, windowed run verified on this
  machine (SDL2 2.30, Vulkan 1.3.275, lavapipe software rendering)

## 1. Where we are

Engine backbone is done and stable: fixed-step `core::Runner` over
`world::State`, `RunConfig` frontend seam, `projectile` stepped solver,
`ohm`/`s0`/`i2c` closed-form solvers, console + CSV sinks, CLI with REPL
and headless `sim run`.

GUI (Dear ImGui + SDL2 + Vulkan, `apps/gui/`) is functional:

| Area | Status | Notes |
|---|---|---|
| Backend scaffold (`backend/`) | Done | Window, swapchain, frame loop; `SYSTEM` ImGui includes |
| Closed-form panels | Done | Ohm/S0/I2C tabs, live re-parse, CLI-identical text |
| Stepped-sim control | Done | Play/pause/step/reset, budget-clamped exact park |
| Graphs | Done | Channel selector + trajectory over frame history |
| 2D canvas | Done | Grid, axes, glyphs, vectors, trails, Fit tracking |
| 3D viewport | Done | Orbit camera, grid/axes, depth-sorted glyphs |
| View-model seam | Done | Views consume `DataFrame` only; 1-entry adapter registry |

## 2. Open work (Plane, `PHYSI` project)

GUI epic (`gui-integration`) still in Backlog:

- Engine linkage: in-process + SHM subscribe + CSV replay in the GUI.
  Depends on the `wengine-link` epic (protocol spec, SHM writer/reader,
  replay bridge, versioning — all still Backlog).
- Backend hardening: validation layers (not installed here) + `--gpu`
  selection override.
- Automated smoke test: `--frames N` hook for CI.

Future solvers (fields, circuits, rigid bodies) each need: engine-side
state domains + stepped/closed-form implementation first, then exactly
one frame-source adapter entry — views stay untouched.

## 3. Needs manual verification (no input/display-automation tooling)

These paths are unit-covered but never click-driven here (no xdotool,
no Xlib bindings, no screenshot-diff harness):

- Typing in solver fields; Play/Step/Reset transport buttons.
- Orbit/pan/zoom feel in 2D/3D views; Fit-button behavior.
- Populated graphs/trails after a real run (empty states screenshotted).
- Multi-GPU device choice; validation-layer output once installed.

## 4. Known limitations (deferred by decision, with triggers)

- Hs-mode I2C has no GUI equivalent (combo offers valid speeds only).
- Plot channels cover the primary item + count only; per-particle bulk
  scalars are not plotted (spatial bulk lives in items/trajectories).
- The generic graphs dropped the old all-slots `y(t)` overlay; it was
  particle-specific coupling. It returns only as a generic multi-trace
  feature with a concrete consumer.
- 3D is CPU-projected via ImDrawList; custom plots instead of ImPlot.
  Triggers to revisit either: measured frame cost, or needs neither
  covers (log axes, histograms, volumes, 100k+ entities).
- GPU pick is first-discrete-else-first with no override (hardening task).
- `SimController` drives one solver per run; multi-solver runs need a
  small extension (Runner already supports N solvers).

## 5. Rework only on concrete need

- Do not pre-split `main.cpp` further; it is thin assembly by rule.
- Do not grow engine `Snapshot` for GUI needs; extend the adapter layer.
- Do not vendor new GUI libraries without a decision record.

## 6. Build quick reference

```sh
cmake -S . -B build -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S . -B build-gui -DBUILD_GUI=ON -DBUILD_TESTS=OFF
cmake --build build-gui --target WEngineGui
./build-gui/WEngineGui --help
```
