# PROJECT_STATE.md — WEngine status snapshot

Living status file: where the project stands, what is open, and what
needs verification, fixing, or reworking. Refresh the counts and the
commit pointer whenever they move. Architectural rules belong in
`AGENTS.md`, not here.

- Date: 2026-10-19
- Tree: `d01c621` ("Add INA219 solver") + uncommitted TUI-4 run-builder work
  (`include/wengine/tui/batches.hpp`, F4 builders in `src/tui/panes_batches.cpp`,
  `tests/test_tui_batches.cpp`)
- Tests: 46/46 pass, zero warnings (default build; strict flags everywhere)
- GUI build (`-DBUILD_GUI=ON`): clean, windowed runs verified on this
  machine (SDL2 2.30, Vulkan 1.3.275, lavapipe software rendering;
  8 s healthy loop, killed by timeout = exit 124, no crash), including
  the signal batch panel + signal graphs (user-verified 2026-09-23)

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
| Signal batch panel | Done* | `models/signal_model.hpp` (CLI-parity validation) + `panels/signal_panel.hpp` (Apply-rebuild); *uncommitted, user-verified |
| Engine linkage | Done | Source panel (in-process/SHM/CSV) + engine-only `ShmSource`/`CsvReplay`; lag-isolated, parity-tested |
| Graphs | Done | Channel selector + trajectory over frame history; signal-aware default channel |
| 2D canvas | Done | Grid, axes, glyphs, vectors, trails, Fit tracking |
| 3D viewport | Done | Orbit camera, grid/axes, depth-sorted glyphs, Track-follows-primary |
| View-model seam | Done | Views consume shared `link::Frame` only; 1-entry adapter registry (projectile, signal) |

Signal generation (new this session, `docs/signals.md` design + decision
log D1–D16): unlimited stamped sources (`sine/square/triangle/saw/dc/
pulse/noise/sum/expression/table/lua`) as a stepped `SignalSolver` (+ Lua
extra solver stepping first); stable per-`State`
serials; physical stamping via the `kQuantities` single table +
`DimensionFor`/`StampedValue` typed binding (U/I first, one-entry
extension); N:M edge-side routing (`SignalSubsetSink` allowlists,
`StrideSink` decimation, tick-preserving, loud counters); signals CSV
log (`step,time,sig_id,value,quantity`) beside the particle log; CLI via
`sig run` (`sim run` stays vocabulary-free; `--sig-expr` closed math over
`t`, `--sig-table` CSV replay with linear interp + clamped ends,
`--sig-lua` sandboxed Lua 5.4 scripts — pure functions of t/seed/step/dt,
instruction budget, vendored `external/lua` pinned v5.4.9);
serialization `v5` with `v4`/`v3`/`v2`/`v1` fallback loaders;
`bench_signal` (~150 ns/source-step; Lua ~7 us/source-step).

## 2. Open work (Plane, `PHYSI` project)

`wengine-link` epic is Done. GUI epic (`gui-integration`) still open:

- Backend hardening: validation layers (not installed here) + `--gpu`
  selection override.
- Automated smoke test: `--frames N` hook for CI.
- Engine-linkage follow-ups are verified as far as automation reaches;
  the remaining click-through (mode switching, replay transport feel,
  populated views per source, Track feel) needs a human pass — no
  automation tooling exists.

Signals epic (new top-level epic, `4341806e`): SIG-1 (design), SIG-2
(engine core), SIG-3 (stamping), SIG-5 (routing), SIG-9 (scoped GUI
delivery) are Done; `sig run` shipped early under SIG-6, leaving the
per-source family (list/add/set/drop, display names) for later. Still
open, in build order:

- SIG-4: custom sources — 4a closed expression + table/CSV replay SHIPPED
  2026-09-24 (RPN expr, div-by-zero→Status; table store + v4 serialization;
  CLI `--sig-expr`/`--sig-table`; GUI batch takes expressions, rejects
  tables pending a SIG-9 picker), 4b Lua scripting source SHIPPED
  2026-09-24 (vendored PUC 5.4.9, sandboxed pure-function scripts,
  instruction budget, v5 serialization, CLI `--sig-lua`; GUI batch rejects
  Lua pending the SIG-9 scripting panel).
- SIG-7 / SIG-8 (independent parallel tracks): SHIPPED — TCP/UDP
  socket publisher sinks (SIG-7) and UART publisher + Zephyr sample
  consumer (SIG-8, 2026-09-28: `--emit-uart device[,baud]` ascii/COBS
  lines, `--control-uart` COBS/`!` control interleave, `examples/zephyr/`
  UART + socket apps with shared C99 parsers, `docs/zephyr-hil.md`).
- SIG-10 (v1 scope, high): SHIPPED 2026-09-28 — generic control plane
  (param set/start/stop over TCP control socket + UART control frames,
  concurrent endpoints with last-writer-wins, replay-logged, mux select
  verb reserved). Sensor feedback stays a follow-up.

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
- Populated graphs/trails after a real run (empty states screenshotted;
  signal graphs user-verified 2026-09-23).
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
  semantics: the old mapping outlives the rerun; undetectable, by design).- Flood publishing (~10^5 frames/s into 8 slots) loses the bulk by policy;
  paced/interactive rates keep up. `link_fastread` reports exact loss%.
- RESOLVED (2026-09-28, PHYSI-43): the `link_fastread` end-of-run blind
  spot was termination/reporting, not reads — attached readers provably
  drain the retained final frame after publisher exit/unlink (v2 slot
  protocol holds). Three causes, all reproduced cross-process: (a) the
  10 ms attach retry missed short-flood segment lifetimes (<5 ms for a
  5-step burst) → now 1 ms like `link_sample`; late attach after exit
  stays impossible by design (segment lives only while the publisher
  lives). (b) `--frames` counts frames RECEIVED, unreachable under
  drop-oldest loss → idle timeouts went silent; both tools now print a
  final loss summary with the parked cursor (`timeout ... last_seq=N`,
  exit 1). (c) same-name republish parks stale readers on the old final
  frame (POSIX fd semantics, undetectable by design) → explicit reconnect
  documented in both tools. Regression: `test_link_shm` (post-exit
  readability, stale/reconnect, attach-miss) + 4-scenario
  `examples/link_fastread_stall_repro.py` (4/4).
- Signals (deferred by decision, with triggers): source display names
  (`sig.motor`) wait for the SIG-6 CLI family (serials are the mechanism);
  mux combinator waits for the SIG-10 select verb; `kSum` is rejected in
  identical-batch runs (per-source operands only); `LinkFrame` caps at 64
  channels — over-bound frames drop loudly, paging only on a real >64
  run; `sig run`/`sim run` cover batch sources, per-source tuning needs
  SIG-6/SIG-10.

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
