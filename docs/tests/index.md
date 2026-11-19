# Tests

The test catalog: an entry for every test artifact in the repository —
every registered test binary and every test-support script or probe.

Each entry answers six things: **what it checks** (specific behaviors
asserted), **purpose** (why those behaviors matter), **guards against**
(the regression class caught if this broke), **how to run** (exact
commands), **prerequisites** (environments like a real PTY, the ROS 2
stack, or embedded hardware, and what happens without them), and
**boundaries** (what the test deliberately does not cover).

The suite protects, in order: deterministic stepping and state
(`core::Runner`, `world::State`), closed-form solver goldens anchored to
independent references, exact output and routing with loud drop counting,
signal generation with stable serials, scripted sources in a sealed
runtime, versioned transports with exact loss accounting, and the CLI/TUI/
GUI seams that drive them all.

Run the whole suite from a standard build tree:

```console
$ cmake -S . -B build -DBUILD_TESTS=ON
$ cmake --build build
$ ctest --test-dir build --output-on-failure
```

Individual binaries print `test_x: OK` lines and can be run directly
(`./build/test_runner`); the `ctest -R <name>` form uses the CTest name,
which drops the `test_` prefix (`ctest --test-dir build -R runner`
matches target `test_runner`).

Assertion policy: every `test_*` target (plus `assert_fail_probe`) links
`wengine_test_assert` (`-UNDEBUG`), so plain-`assert` checks — including
asserts around operations with side effects — execute in Debug and in
optimized `--preset test-opt` runs alike. Benchmarks never link this
target and keep their own `NDEBUG`/optimization behavior. The policy
itself is pinned by `test_assert_policy`, and the intentional-failure
probe (`assert_fail_probe` + `assert_probe_check.sh`) proves asserts
cannot be silently compiled out.

The suite totals 74 registered CTest targets plus support artifacts;
completeness against the live registration is audited by
`docs/audit_catalogs.py` (zero missing entries, zero ghost anchors at
delivery).

## Domain map

| Domain | Page | Targets |
| --- | --- | --- |
| Core and world | [Core and world](core-world.md) | `test_runner`, `test_world`, `test_sim`, `test_run_config`, `test_determinism`, `test_snapshot_reuse`, `test_particle_identity`, `test_solver_registry` |
| Solvers | [Solvers](solvers.md) | `test_math_units`, `test_ohm_law`, `test_s0`, `test_i2c`, `test_ic`, `test_ldo`, `test_led`, `test_ina219`, `test_ap63205`, `test_tmc22xx` |
| Emit and routing | [Emit and routing](emit-routing.md) | `test_emit`, `test_routing`, `test_output_health`, `test_output_queue`, `test_paced` |
| Signals and scripting | [Signals and scripting](signal-scripting.md) | `test_signal`, `test_signal_control`, `test_lua` |
| Link transport | [Link transport](link-transport.md) | `test_link_shm`, `test_link_protocol`, `test_link_compact`, `test_link_bridge`, `test_link_receiver`, `test_link_sample_block`, `test_link_sockets`, `test_tcp_worker`, `test_uart_codec`, `test_uart_pub` |
| Logic | [Logic](logic.md) | logic solver, sequential extensions, workspace store, CLI verbs |
| TUI | [TUI](tui.md) | TUI executor, forms, batches, async worker, views, `tui_pty_smoke.py` |
| GUI models | [GUI models](gui-models.md) | stream sources, sim control, panels, viewport, frame source, canvas |
| Integrations and fabric | [Integrations and fabric](integrations-fabric.md) | ROS 2 bridge, control server, `assert_fail_probe` + `assert_probe_check.sh`, CLI args |

Support scripts and probes live on their domain pages: the PTY smokes
(`logic_pty_smoke.py`, `tui_pty_smoke.py`, needing a real PTY via
`python3 pty.fork` and the built `PhysicsSimApp`) on the logic and TUI
pages, and the assertion-policy probe (`assert_fail_probe`,
`assert_probe_check.sh`) on the integrations page.

```{toctree}
:maxdepth: 1

core-world
solvers
emit-routing
signal-scripting
link-transport
logic
tui
gui-models
integrations-fabric
```
