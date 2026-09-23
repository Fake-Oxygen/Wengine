# PROJECT_STATE.md — WEngine status snapshot

Living status file: where the project stands, what is open, and what
needs verification, fixing, or reworking. Refresh the counts and the
commit pointer whenever they move. Architectural rules belong in
`AGENTS.md`, not here.

- Date: 2026-09-23
- Tree: clean at `bc132a2` ("Link GUI SHM")
- Tests: 21/21 pass, zero warnings (default build; strict flags everywhere)
- GUI build (`-DBUILD_GUI=ON`): clean, windowed runs verified on this
  machine (SDL2 2.30, Vulkan 1.3.275, lavapipe software rendering;
  8 s healthy loop, killed by timeout = exit 124, no crash)

## 1. Where we are

Engine backbone is done and stable: fixed-step `core::Runner` over
`world::State`, `RunConfig` frontend seam, `projectile` stepped solver,
`ohm`/`s0`/`i2c` closed-form solvers, console + CSV sinks, CLI with REPL
and headless `sim run` (`--emit-csv`, `--emit-shm`).

`wengine-link` epic is done (all 6 Plane children Done): versioned
seqlock SHM snapshot bus with a solver-agnostic `link::Frame` view-model,
C++ publisher/reader endpoints (`wengine_link_shm` STATIC TU), CSV↔bus
replay bridge (`link_bridge` play/record, byte-identical round-trips),
`link_sample` printer + `link_fastread` minimum-loss subscriber, and
`bench_link` (6.1 µs/frame at 1 item, 16.6 µs at 64, 218 µs at 1024;
slow-reader lag exact: received=1, missed=N-1).

GUI (Dear ImGui + SDL2 + Vulkan, `apps/gui/`) is functional:

| Area | Status | Notes |
|---|---|---|
| Backend scaffold (`backend/`) | Done | Window, swapchain, frame loop; `SYSTEM` ImGui includes |
| Closed-form panels | Done | Ohm/S0/I2C tabs, live re-parse, CLI-identical text |
| Stepped-sim control | Done | Play/pause/step/reset, budget-clamped exact park |
| Engine linkage | Done | Source panel (in-process/SHM/CSV) + engine-only `ShmSource`/`CsvReplay`; lag-isolated, parity-tested |
| Graphs | Done | Channel selector + trajectory over frame history |
| 2D canvas | Done | Grid, axes, glyphs, vectors, trails, Fit tracking |
| 3D viewport | Done | Orbit camera, grid/axes, depth-sorted glyphs, Track-follows-primary |
| View-model seam | Done | Views consume shared `link::Frame` only; 1-entry adapter registry |

## 2. Open work (Plane, `PHYSI` project)

`wengine-link` epic is Done. GUI epic (`gui-integration`) still open:

- Backend hardening: validation layers (not installed here) + `--gpu`
  selection override.
- Automated smoke test: `--frames N` hook for CI.
- Engine-linkage follow-ups are verified as far as automation reaches;
  the remaining click-through (mode switching, replay transport feel,
  populated views per source, Track feel) needs a human pass — no
  automation tooling exists.

Future solvers (fields, circuits, rigid bodies) each need: engine-side
state domains + stepped/closed-form implementation first, then exactly
one `link::Frame` adapter function + one frame-source registry entry —
views stay untouched.

## 3. Needs manual verification (no input/display-automation tooling)

These paths are unit-covered but never click-driven here (no xdotool,
no Xlib bindings, no screenshot-diff harness):

- Typing in solver fields; Play/Step/Reset transport buttons.
- Source-panel flows: SHM segment switch vs Reconnect, CSV Load/play/
  step/reset across two different logs, mode-switch history resets.
- Orbit/pan/zoom feel in 2D/3D views; Fit-button and Track-checkbox feel.
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
- Same-name publisher restart needs explicit Reconnect (POSIX fd
  semantics: the old mapping outlives the rerun; undetectable, by design).
- Flood publishing (~10^5 frames/s into 8 slots) loses the bulk by policy;
  paced/interactive rates keep up. `link_fastread` reports exact loss%.
- UNRESOLVED (2026-09-23): `link_fastread` goes blind for a run's last
  frames and stays blind through long timeouts while the mapping provably
  holds them; `link_sample` on identical runs completes. In-process,
  post-exit in-process, flood tracking (1M+ frames), and unit coverage all
  pass — isolated to the live publisher-exit window, cause unknown. Needs
  a focused session with strace/gdb allowed + poll tracing. See task notes.

## 5. Rework only on concrete need

- Do not pre-split `main.cpp` further; it is thin assembly by rule.
- Do not grow engine `Snapshot` for GUI needs; extend the adapter layer.
- Do not grow `link::Frame` for transport needs; wire-only concerns live
  in `LinkFrame`.
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
