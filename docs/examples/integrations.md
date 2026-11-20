# Integrations and samples

The ROS 2 bridge, the five Lua signal sources, the eight
logic-domain sample workspaces, and the Zephyr embedded sample.
Lua and logic entries were all run here; the ROS 2 and Zephyr
entries state their prerequisites and were not run here.

## ros2_bridge.cpp

**Demonstrates**: live state-out wiring without touching the engine —
a `Runner` driving one `SignalSolver` (two stamped sources) plus one
`ProjectileSolver` (three particles) fans out through an edge-owned
chain (`Runner -> StrideSink -> SignalSubsetSink -> RosPublisher ->
RclcppTransport`) onto standard topics (`/wengine/signals` grouped
`JointState` plus exact `SignalBlock`, per-serial topics,
`/wengine/particles` `PointCloud2`), with sim-time header stamps.
At step 250 the first source is despawned and respawned, showing
serial-addressed topics surviving despawn/respawn.

**How to run**: NOT RUN HERE. Prerequisites: a ROS 2 build
(`cmake -S . -B build -DBUILD_ROS2=ON`, this tree is configured with
`BUILD_ROS2=OFF`), a sourced ROS 2 distro, and the fresh build-tree
message libraries on the loader path:

```sh
source /opt/ros/<distro>/setup.bash
export LD_LIBRARY_PATH="$PWD/build/src/ros2/msgs:$LD_LIBRARY_PATH"
./build/ros2_bridge [--per-serial] [--stride N] [--select a,b] [--steps N]
```

**Expected outcome**: not observed here. Claims rest on the source
header plus `docs/ros2.md`: exit `0` with an `attempted/published/
over_bound/backpressure/...` summary line; grouped mode publishes
`JointState` + `SignalBlock` per emitted tick and per-serial mode one
`JointState` topic per live serial (see `docs/ros2.md` for the
`ros2 topic echo` checks).

## lua/sine.lua

**Demonstrates**: the canonical Lua signal — a 1 Hz sine at amplitude
2, as a pure function of `(t, seed, step, dt)`.

**How to run**:

```sh
./build/PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/sine.lua --steps 250 --dt 1ms
```

**Expected outcome**: `s0: lua value=2 U` at `t=0.25s` (quarter
period), exactly matching `sig run --sig-kind sine --sig-freq 1Hz
--sig-amp 2V`.

## lua/burst.lua

**Demonstrates**: branching and `math.floor` for periodic gating — a
gated 440 Hz tone with 0.1 s bursts every 0.5 s and sin² click-free
edges, restarting from the same phase each burst so replay is exact.

**How to run**:

```sh
./build/PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/burst.lua --steps 600 --dt 1ms
```

**Expected outcome**: `s0: lua value=0 U` at `t=0.6s` (between
bursts); silence outside each 0.1 s window, a smooth 440 Hz burst
inside.

## lua/chirp.lua

**Demonstrates**: a linear frequency sweep (1 Hz to 5 Hz over
2 s) with the quadratic swept-phase term spelled out in named locals.

**How to run**:

```sh
./build/PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/chirp.lua --steps 2 --dt 0.25s
```

**Expected outcome**: `s0: lua value=-1 U` at `t=0.5s`
(three-quarter point of the swept phase; `t=0.25s` gives ~0.9239).

## lua/seed_detune.lua

**Demonstrates**: one script fanning out into a detuned stack —
`seed` is the spawn serial, so `--sig-count 2` spawns two sources
with different phases.

**How to run**:

```sh
./build/PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/seed_detune.lua --sig-count 2 --steps 1 --dt 1ms
```

**Expected outcome**: two sources with different values (`s0: lua
value=0.368125 U`, `s1: lua value=-0.583481 U`) — s0 at phase 0, s1
at phase 1 rad.

## lua/softclip.lua

**Demonstrates**: stateless nonlinear shaping — an overdriven 220 Hz
sine through `x/(1+|x|)`, so the output always lands in (-1, 1) and
no non-finite sample can leak downstream.

**How to run**:

```sh
./build/PhysicsSimApp sig run --sig-kind lua --sig-lua examples/lua/softclip.lua --steps 3 --dt 1ms
```

**Expected outcome**: `s0: lua value=-0.716953 U` at `t=0.003s`
(clipped negative lobe, `|value| < 1`).

## logic/fanout.yaml

**Demonstrates**: unlimited fan-out under the single-driver rule —
one `AND1.Y` driving two sinks (`NOT1.1`, `BUF1.1`); block inputs
`AND1.1, AND1.2`, block outputs `NOT1.Y` (= NOT(AND)) and `BUF1.Y`
(= AND).

**How to run**:

```sh
printf 'logic load examples/logic/fanout.yaml\nlogic list --wires\nlogic solve --from AND1.1=1 --from AND1.2=1\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `gates (3)` with `AND1 ... fanout=2` plus the
two `AND1.Y -> ...` wires; the solve echoes `in: 11 ...` and prints
`NOT1.Y=0` and `BUF1.Y=1`.

## logic/gated_async_counter.yaml

**Demonstrates**: a full-control-set flop (D/CLK/RST/SET/EN) toggling
itself with enable gating and async forces at total priority RST >
SET > EN-hold > D; the clock dangles, forming the implicit `clk`
domain.

**How to run**:

```sh
printf 'logic load examples/logic/gated_async_counter.yaml\nlogic peek\nlogic poke DFF1.EN=1\nlogic step 1\nlogic peek\nlogic poke DFF1.RST=1\nlogic peek\nlogic poke DFF1.RST=release\nlogic poke DFF1.SET=1\nlogic peek\nlogic poke DFF1.SET=release DFF1.EN=0\nlogic step 1\nlogic peek\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `loaded 2 gates 2 wires`; first `peek` shows
`DFF1.Q=0 NQ=1 (serial 0)`; with EN poked, `step 1` captures
`DFF1.Q: 0->1`; poking RST forces `1->0` at once with no step; after
release, poking SET forces `0->1` at once; with EN released, `step 1`
holds (`DFF1.Q: 1 (no change)`).

## logic/jk_t_latch_chain.yaml

**Demonstrates**: every sequential kind in one run — a JK divider
(J=K=1, poked) driving a T divider, sampled by a level latch that is
transparent while EN is poked High and frozen once EN is released.

**How to run**:

```sh
printf 'logic load examples/logic/jk_t_latch_chain.yaml\nlogic peek\nlogic poke JK1.J=1\nlogic poke JK1.K=1\nlogic poke DLATCH1.EN=1\nlogic step 2\nlogic peek\nlogic poke DLATCH1.EN=0\nlogic step 1\nlogic peek\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `loaded 3 gates 2 wires`; first `peek` shows
all stored zeroes; `step 2` reports `JK1.Q: 0 (no change)`
(toggled twice), `T1.Q: 0->1`, `DLATCH1.Q: 0 (no change)`; with EN
released, `step 1` reports `JK1.Q: 0->1`, `T1.Q: 1 (no change)`,
and the frozen `DLATCH1.Q: 0 (no change)`.

## logic/nand_pair.yaml

**Demonstrates**: the minimal wired pair — `AND1` feeding `NOT1`,
so the single block output is the NAND of the two block inputs.

**How to run**:

```sh
printf 'logic load examples/logic/nand_pair.yaml\nlogic truth\nlogic solve --from AND1.1=1 --from AND1.2=1\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: a 4-row truth table (`AND1.1 | AND1.2 |
NOT1.Y`) ending in `1 | 1 | 0`; the solve echoes `in: 11 ...` and
prints `NOT1.Y=0`.

## logic/parity3.yaml

**Demonstrates**: 3-input odd parity as an `XOR1.Y -> XOR2.1`
cascade, exactly like 74-series parts do for two inputs.

**How to run**:

```sh
printf 'logic load examples/logic/parity3.yaml\nlogic truth\nlogic solve --from XOR1.1=1 --from XOR1.2=1 --from XOR2.2=1\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: an 8-row truth table (`XOR1.1 | XOR1.2 |
XOR2.2 | XOR2.Y`); the solve echoes `in: 111 ...` and prints
`XOR2.Y=1` (three highs = odd).

## logic/shared_bus.yaml

**Demonstrates**: an allowlisted 2-driver wired-OR bus — `AND1.Y`
and `AND2.Y` both drive `OR1.1` under the explicit per-input shared
mark; agreeing drivers resolve to the level while a disagreeing pair
resolves X with a visible per-net contention count.

**How to run**:

```sh
printf 'logic load examples/logic/shared_bus.yaml\nlogic list --wires\nlogic solve --in 11110\nlogic solve --in 11000\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `loaded 4 gates 3 wires`; `list` shows
`OR1 ... shared=OR1.1(2)` plus both `-> OR1.1` wires; the agreeing
solve prints `OR1.Y=1` with `contention: OR1.1=0 (total 0)`; the
contended solve refuses loudly with `contention: OR1.1=1 (total 1)`.

## logic/buffer_bus.yaml

**Demonstrates**: an enable-gated tri-state bus with a pull-up —
`TBUF1` carries `AND1.Y` onto the shared sink `OR1.1` while `AND2.Y`
(the active-high enable) holds, with `AND1.Y` as the second
attachment; `OR1.1` is marked in `shared:` and idled in `pulls:`.

**How to run**:

```sh
printf 'logic load examples/logic/buffer_bus.yaml\nlogic unwire AND1.Y OR1.1\nlogic solve --in 1100\nlogic pull OR1.1 down\nlogic solve --in 1100\nlogic unpull OR1.1\nlogic solve --in 1100\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `loaded 4 gates 4 wires`; parking the bus
(unwiring the direct driver) with the enable low solves `OR1.Y=1`
(buffer released, pull-up idles high) with
`contention: OR1.1=0 (total 0)`; after `pull OR1.1 down` the same
parking solves `OR1.Y=0`; after `unpull` the parked solve refuses
loudly (the float has no idle level).

## logic/tied_demorgan.yaml

**Demonstrates**: the tied-input De Morgan mirror — NOT via
`NAND(x,x)` with tied inputs, so `OR(~c, ~d) == NAND(c, d)`
(`OR1.Y` must match a plain NAND over `(c, d)` on every row), plus a
transitive 3-member tie group where `XOR(E, E)` is always 0.

**How to run**:

```sh
printf 'logic load examples/logic/tied_demorgan.yaml\nlogic list --iface\nlogic truth --limit 4\nlogic solve --from XOR1.2=1 --from AND1.2=1\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `inputs (6)` with one entry per tie class plus
`ties (3)`; the truth header reads `NAND1.1 | NAND1.2 | NAND2.1 |
NAND3.1 | XOR1.1 | AND1.2 | NAND1.Y | OR1.Y | XOR1.Y | AND1.Y` with
row 0 giving `1 | 1 | 0 | 0`; the solve echoes `in: 000011 ...` and
prints `NAND1.Y=1, OR1.Y=1, XOR1.Y=0, AND1.Y=1`.

## logic/tieoff.yaml

**Demonstrates**: a tied-off input plus stored timing data —
`AND1.2` is ignored (out of the block interface, tied to 0 during
eval) while `delay_ps` values persist verbatim for a later timing
simulation.

**How to run**:

```sh
printf 'logic load examples/logic/tieoff.yaml\nlogic list --iface\nlogic solve --from AND1.1=1\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `inputs (1)` lists only `AND1.1`
(`AND1.2` is ignored), `outputs (1)` lists `NOT1.Y`; the solve
echoes `in: 1 ...` and prints `NOT1.Y=1` (AND(1, tied-0) = 0).

## logic/two_clock_counter.yaml

**Demonstrates**: two independent clock domains with mixed power-on
inits — `DFF1` clocked by `BUF1.Y`, `DFF2` on the implicit `clk`
domain, each toggling itself; selective stepping advances one domain
while the other holds.

**How to run**:

```sh
printf 'logic load examples/logic/two_clock_counter.yaml\nlogic peek\nlogic step BUF1.Y 1\nlogic peek\nlogic step clk 1\nlogic peek\nlogic step 1\nlogic peek\nexit\n' | ./build/PhysicsSimApp
```

**Expected outcome**: `loaded 5 gates 5 wires`; first `peek` shows
`DFF1.Q=0 NQ=1 (serial 0)` and `DFF2.Q=1 NQ=0 (serial 1)`; `step
BUF1.Y 1` reports `DFF1.Q: 0->1`; `step clk 1` reports `DFF2.Q:
1->0`; `step 1` advances both (`DFF1.Q: 1->0`, `DFF2.Q: 0->1`).

## zephyr/

**Demonstrates**: the whole embedded sample — two minimal Zephyr
apps (plain C99, no heap) speaking the WEngine wire protocols: the
`uart/` app reads the UART stream plus control plane, the `socket/`
app reads TCP compact records; the `common/` parsers are compiled
unchanged into the host unit test.

**How to run**: NOT RUN HERE. Prerequisites: the Zephyr SDK
(`west`, Zephyr 3.7.0 workspace) plus board hardware — `nrf52dk`
for the UART sample (host serial `/dev/ttyUSB0`, 115200 8N1) or
`stm32f469i_disco` via its app overlay (single UART3 through
ST-Link, `/dev/ttyACM0`), or QEMU x86 with host IP reachability for
the socket sample (see `examples/zephyr/README.md` for the exact
`west build` / `west flash` lines and `docs/zephyr-hil.md` for the
baud budget and single-UART sharing rules).

```sh
west build -b nrf52dk examples/zephyr/uart
west flash
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz \
  --sig-amp 3.3V --steps 1000 --dt 0.1ms \
  --emit-uart /dev/ttyUSB0 --control-uart
```

**Expected outcome**: not observed here. Claims rest on
`examples/zephyr/README.md`: the UART sample prints one line per
sample (`sig id=<serial> step=<n> t=<s> v=<value> <U|I>`, plus `GAP`
when steps skip) and answers the startup `PING` round-trip (`ctl: OK
pong`); the socket sample prints `step/seq/channels` and reports
`frame_seq` gaps. Without hardware, the host loopback path (`--emit-
uart` through a pty pair, `test_uart_codec` goldens) is the runnable
check.
