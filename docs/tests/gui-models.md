# GUI models

Engine-only controller and model checks for the GUI: everything here
links `wengine_core` alone (plus `wengine_link_shm` /
`wengine_signal_lua` where the seam demands it) and runs headless with
no SDL, Vulkan, or ImGui linked. Windowed rendering, interaction feel,
and populated-state screenshots are verified manually per project
practice — these binaries pin the math and parity underneath.

## test_canvas2d

**What it checks**: the 2D world-view math: world↔screen round-trips
with Y-flip, `fit` deriving center and scale from point spans (plus the
empty-input unit view), zoom clamping with `auto_fit` release, pan
direction conventions, and grid-step selection.

**Purpose**: the canvas must map simulation coordinates to pixels
exactly and stay stable under fit/zoom/pan before any drawing happens.

**Guards against**: Y-flip errors, fit-scale drift, zoom-range escapes,
and pan-direction inversions.

**How to run**:

```sh
ctest --test-dir build -R canvas2d
./build/test_canvas2d
```

**Prerequisites**: standard build only.

**Boundaries**: ImGui drawing, interaction feel, and populated-state
visuals are verified manually, not here.

## test_viewport3d

**What it checks**: the orbit-camera math over vendored glm doubles:
target projects to viewport center, behind-camera points culled,
depth ordering along the view direction, grid generation, and guard
rails (pitch/distance clamps, degenerate inputs).

**Purpose**: the 3D viewport rasterizes from these projections; camera
math must be exact while ImGui stays out of the test.

**Guards against**: projection drift, culled-visible inversions, and
grid/camera guard regressions.

**How to run**:

```sh
ctest --test-dir build -R viewport3d
./build/test_viewport3d
```

**Prerequisites**: standard build only.

**Boundaries**: ImGui rasterization is verified visually; only the glm
computation is pinned here.

## test_gui_sim_control

**What it checks**: `SimController` stepped-sim parity with the headless
path: the same `RunConfig` gives bit-identical snapshots via controller
and `run_headless`, independently derived projectile goldens
(`x = 10`, `y = 5.04595`, `vy = 0.19`), step/reset determinism, advance
modes, config validation (gravity handling, non-finite rejection), and
step failure stopping playback with the live state untouched.

**Purpose**: the GUI panel rebuilds `RunConfig` from live state and
re-attaches sinks; the driven snapshots must equal the CLI's exactly.

**Guards against**: controller/headless divergence, gravity-config
mishandling, and failed steps corrupting playback state.

**How to run**:

```sh
ctest --test-dir build -R gui_sim_control
./build/test_gui_sim_control
```

**Prerequisites**: standard build only.

**Boundaries**: panel rendering and stepped-sim interaction feel are
manual; signal batches live in `test_signal_panel`.

## test_gui_stream_source

**What it checks**: external display-side sources: SHM subscribe parity
with the publisher, wait/refusal behavior, lag isolation staying on the
display side, segment switching and same-name republish needing
reconnect, CSV load/step with pacing, CSV three-level parity (CLI text
vs replayed frame vs serialized artifact), and signal-CSV replay.

**Purpose**: external sources own no sim state by construction; lag and
failure must never leak into the simulation.

**Guards against**: subscribe/replay divergence, lag accounting leaking
into sim state, and stale-mapping reads after republish.

**How to run**:

```sh
ctest --test-dir build -R gui_stream_source
./build/test_gui_stream_source
```

**Prerequisites**: standard build only (POSIX SHM loopback; PID-tagged
segment names isolate parallel runs).

**Boundaries**: panel rendering is manual; bus transport mechanics live
in the link-transport page.

## test_signal_panel

**What it checks**: the signal panel model plus controller parity:
empty-field defaults (CLI-missing-option semantics), the `dc 3.3V`
golden, rejection text matching the CLI, batch bit-parity through the
controller bus, table/lua model builds, and SIG-10 verb-session and
expression parity with rejection matrices.

**Purpose**: the panel builds candidate configs from live state; every
accepted config must drive exactly what the headless signal path
drives.

**Guards against**: model/CLI default drift, verb-session divergence,
and silently accepted bad signal configs.

**How to run**:

```sh
ctest --test-dir build -R "^signal_panel$"
./build/test_signal_panel
```

**Prerequisites**: standard build only (runtime-free; Lua wiring lives
in `test_signal_panel_lua`).

**Boundaries**: ImGui rendering is manual; scripting-runtime ownership
is covered next.

## test_signal_panel_lua

**What it checks**: controller scripting-runtime wiring: Lua batches
rejected without the caller-owned runtime (live run untouched), Lua
batch bit-parity with headless once extras plus `has_lua_runtime` are
threaded through, loud script failures with inquiryable text, the
rejection matrix (dummy/non-Lua extra, missing flag, null entries), and
reset re-attaching sinks with the runtime present.

**Purpose**: the Lua runtime is app-owned and lent to the controller,
never in engine headers; the flag plumbing must be exact or scripted
sources silently misbehave.

**Guards against**: Lua running without a runtime, swallowed script
errors, and sink loss across controller resets.

**How to run**:

```sh
ctest --test-dir build -R signal_panel_lua
./build/test_signal_panel_lua
```

**Prerequisites**: standard build only (links `wengine_signal_lua`).

**Boundaries**: sandbox language semantics live in `test_lua`; panel
rendering is manual.

## test_closed_form_panels

**What it checks**: closed-form panel models vs the CLI: the `ohm`
README golden (`12.4V + 0.54A → R = 22.963 Ohm, P = 6.696 W`),
prefix/bare-number parsing, the exactly-two rule with CLI wording,
`s0` mode goldens with usage parity, `i2c` pull-up/series goldens with
limit text parity, invalid-suffix errors naming field and reason, and
the panel registry size pin (`kClosedFormPanels == 3`).

**Purpose**: the same field text must render the same goldens and the
same validation messages as the commands; new solvers must register.

**Guards against**: model/CLI text drift, dropped validation paths, and
unregistered solver panels.

**How to run**:

```sh
ctest --test-dir build -R closed_form_panels
./build/test_closed_form_panels
```

**Prerequisites**: standard build only.

**Boundaries**: ImGui rendering is manual; solver equations belong to
the solvers page.

## test_frame_source

**What it checks**: the frame-source seam: the per-solver adapter
registry (`kFrameSources == 3`, unknown names fall back to the particle
mapping), the signal-aware default channel, `fit_bounds` plot math, the
history ring with reset, bit-exactness against `CsvSink` output for
projectile/signal/logic CSV, and controller sink re-attachment through
the recorder.

**Purpose**: views consume the `link::Frame` view-model only; adapters
plus history must be exact so panels never touch snapshot internals.

**Guards against**: missing adapter registration, default-channel
regressions, history-ring drift, and frame/CSV divergence.

**How to run**:

```sh
ctest --test-dir build -R frame_source
./build/test_frame_source
```

**Prerequisites**: standard build only.

**Boundaries**: graph/canvas rendering is manual; wire encoding lives in
`test_link_protocol`.
