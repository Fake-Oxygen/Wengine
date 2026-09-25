# Signals design (SIG-1)

Goal: unlimited signal sources (sine/square/triangle/saw/DC/pulse/PWM/noise/
sum/expression/table/Lua) stamped with physical quantities (U/I first),
routed N:M to sinks, controllable via CLI/GUI/generic control plane,
streamed over SHM (existing) + TCP/UDP + UART for Zephyr HIL. Variable
rate: 1 kHz nominal, sub-Hz..100 kHz-class `dt` without hard limits
(decimation + budgets).

Locked 2026-09-23 (user): signals ARE stepped solvers; bidirectional control
plane is v1 scope; custom = closed expression + table now, Lua as gated 4b;
U/I first with one-entry scaling; TCP/UDP || UART parallel tracks.

Shipped 2026-09-24 (SIG-4a): expression + table sources (this doc, sec. 4a).
Shipped 2026-09-24 (SIG-4b): Lua scripting source (sec. 7).

## 1. Engine shape

New header-only module `include/wengine/signal/*.hpp` (`wengine_signal`
INTERFACE, linked into `wengine_core`), one stepped solver:

- `Signals` SoA pool in `world::State` (same shape as `Particles`:
  parallel vectors + occupancy + `live_count`), entity-attached via
  `attach()/detach()`, `State::despawn_entity()` detaches it like particles.
- `SignalSolver : core::ISolver` (`kName = "signal"`, `create()`, validates
  `dt`, skips detached slots, `noexcept`, no I/O, SI doubles). Registered in
  `solver_registry.hpp`; runs alongside `projectile` (Runner already steps N
  solvers per tick). Configuration (add/set/drop/start/stop) happens between
  runs or at deterministic step boundaries via the control queue (sec. 5) —
  never mid-step.
- `Snapshot` gains dense signal arrays in slot order (`sig_id`, `sig_value`,
  `sig_quantity`, `sig_flags`), mirroring the particle arrays. This is domain
  data, not GUI growth: allowed by the Snapshot rule.
- `link::frame_from_signal_snapshot()` adapter: one `FrameChannel` per live
  source (`sig.<id>.U` etc.), plus `sig.count`. One `kFrameSources` registry
  entry (`"signal"`); views untouched. CSV gains a `signals` section or a
  second log (decision D2b); SHM reuses `encode_frame` unchanged.

## 2. Stamping (quantities)

Hot path stores `double` SI + `QuantityId` enum (`kVoltage`, `kCurrent`, ...).
A compile-time table maps `QuantityId <-> Dimension` for parsing/formatting
through `units/scales.hpp` only (no ad-hoc tables). Adding a dimension =
one `Dimension` + one literal + one `scales_for` row + one enum entry +
one test (proven by a test-only third dimension in SIG-3).

## 3. Time/phase base

Closed form over absolute sim time: `v = f(sim_time - t0)` with integer
cycle counting for discontinuous kinds (square/pulse/PWM). No unbounded
phase accumulator (avoids drift across `dt` scales); `t0` freezes on
stop and re-bases on start (pause/resume exact). Noise: seeded
xorshift/splitmix advanced once per step in slot order (deterministic,
`dt`-independent sequence; value scaled by params, not by `dt`).

## 4. Routing (SIG-5, implemented 2026-09-23)

Identity: every source owns a stable `serial` (u64, monotonic per State,
never reused across despawn/respawn). Slots recycle, serials don't, so
subscriptions, CSV `sig_id`s, Frame channels (`sig.<serial>.U`), and the
control plane address sources unambiguously (`find_serial` resolves to the
live slot; unknown/despawned serials are "source gone", never slot 0).

`Snapshot` carries ALL live source values (dense slot order). Per-sink
selection lives in adapters/filters at the edge, never in `Runner`:

- 1 source -> N sinks: attach N sinks (existing `Runner` fan-out), each
  behind its own `emit::SignalSubsetSink` allowlist.
- N sources -> 1 sink: one `SignalSubsetSink` holding N serials; or the
  `kSum` combinator source (sums operand serials, replayable in Snapshot).
- Rate decimation: `emit_every` (global) + `emit::StrideSink` wrapper
  (per-sink, step-aligned `step % stride == 0`, no `Runner` change).
- Mix/mux arithmetic beyond sum needs runtime selection: deferred to the
  SIG-10 control plane (select verb). Source display names (`sig.motor`):
  deferred to the SIG-6 CLI family (serials are the mechanism; names are UX).

Sum rules: operands validated at configure time (each resolves to a live
non-sum source — sums stay flattened, no dangling refs, no self-reference
since the new serial doesn't exist yet). The solver evaluates generators
first, sums second, so sums always read fresh values regardless of slot
order. A later-despawned operand fails the step loudly (`kInvalidArgument`,
time/index frozen) instead of reading a stale slot; a stopped operand
contributes its frozen sample. Batch runs (`run_headless`, GUI controller)
reject `kSum`: identical batch sources cannot hold distinct operands —
the per-source family configures sums directly.

## 4a. Custom sources: expression + table (shipped 2026-09-24)

Both are `SignalKind`s (`kExpression = 8`, `kTable = 9`, append-only
after `kSum`), evaluated in solver pass 1 on the same source-time base
(`t = t_end - t0`, rebased on start). Snapshot/Frame/CSV shapes are
unchanged (still SI value + quantity per source); only the pool grows.

Expression (`signal/expr.hpp`, CLI `--sig-expr`, GUI batch model text
field): closed language — decimal/scientific literals (from_chars, no
implicit multiplication), variable `t`, constants `pi`/`e`, binary
`+ - * / ^` (power, right-associative), unary `-`, functions `sin cos exp
log sqrt abs` (1 arg) + `min max` (2 args), parentheses, ASCII whitespace
ignored. Parsing compiles to a fixed RPN program (<= 64 nodes, no
allocation, no strings) with a fixed eval order, so identical text gives
identical results. Programs store inline per slot (`expr_op/expr_arg/
expr_len` SoA). Errors are loud `Status`, never NaN/inf: division by an
exactly-zero denominator, `log(x <= 0)`, `sqrt(x < 0)`, and any non-finite
intermediate (overflow, out-of-domain pow) fail evaluation; the solver
maps that to a failed step (time/index frozen, nothing emitted), exactly
like a dangling sum operand. A source already failing at `t = 0` is
rejected at configure time.

Table (`signal/table.hpp`, CLI `--sig-table`, batch `SignalRunConfig::
table`): CSV waveform with the exact header `time,value`, one `t,v` row
per line (SI seconds + SI value, no suffixes; full precision for
bit-exact replay; one trailing `\r` tolerated). Points validate as
finite with strictly increasing times (1..65536 per source). Evaluation
is linear interpolation with clamped ends and bit-exact returns on exact
time hits, found by binary search (unique bracket — method-independent,
deterministic). Points store in one append-only global pool store with
per-slot `(table_off, table_len)` ranges (slots recycle, ranges never
move); batch spawns share one appended range. Table points never live in
`SignalParams` (variable length would allocate on per-step copies) —
`configure_source` rejects `kTable`; tables configure via
`configure_table_source` / `append_table_points` + `attach_table_source`.
The GUI batch model rejects tables (no CSV picker in the batch form —
SIG-9 panel follow-up); the per-source family (SIG-6) configures them
directly.

Serialization is v4 (expr columns + table ranges per slot, store tail;
v3 loads with zeroed custom columns, v2/v1 as before). Cross-kind
leftovers (expression program with a sine kind, table points with a
non-table kind) are rejected at every edge (CLI usage error, driver/
controller `kInvalidArgument`), never silently ignored.

Subset sinks preserve ticks: a non-muted subscription forwards EVERY
snapshot (possibly with zero rows), so downstream SHM readers observe
every tick and their LagStats stay exact — only rows filter, never ticks.
Mute = empty subscription (everything dropped, counted). Drops are loud
via `matched()`/`dropped()`/`forwarded_rows()` counters.

## 5. Transports (stream-out)

Delivery promises, clocks, counters, and CSV-vs-checkpoint roles are
defined once in `docs/delivery.md` (PHYSI-28); this section covers the
signal stream-out mechanics only.

All publishers are `emit::ISink` at the app edge, non-blocking,
copy-and-return; transient socket backpressure is counted, hard peer failures
are sticky `Status`, and over-bound frames are loud drops. TCP keeps the
kernel send buffer (256 KiB default) as its queue. An EAGAIN before any bytes
drops that frame; a partially accepted frame is completed before any later
frame is sent, or the TCP stream framing would be corrupted. UDP drops a
backpressured datagram whole. `PublisherStats::dropped_send` reports publisher
drops; `frame_seq` gaps let readers calculate loss with `LagStats`.

- SHM: reuse `ShmPublisher` with the signal adapter + domain `"signal"`.
- TCP/UDP (SIG-7): `TcpPublisher`/`UdpPublisher` (`src/link/`, POSIX-first),
  active connection to `host:port`; TCP begins with a `WLT1` hello carrying
  exact link version + domain, then little-endian `u32` length and full
  `LinkFrame` bytes. A reader rejects any magic/version/domain mismatch
  before decoding; there is no silent version fallback. UDP has no handshake:
  each `WLU1` datagram carries exact version + domain, frame sequence/time,
  and only used `LinkItem`/`LinkChannel` entries in portable little-endian
  encoding. Full LinkFrames are 68,136 B and exceed UDP's 65,507 B payload
  limit, hence the compact UDP form; both paths first run the shared LinkFrame
  bound checks so an over-bound snapshot has the same loud verdict.
- UART (SIG-8): `UartPublisher` (`--emit-uart`), ASCII line framing
  `step,time,ch_id,value` + optional COBS binary; never back-pressures sim.

### Python signal receivers

The stdlib-only examples `examples/link_tcp_signal_receiver.py` and
`examples/link_udp_signal_receiver.py` listen for the signal domain, validate
the corresponding v1 handshake/datagram layout, print `sig.*` channels, and
report sequence gaps. Start a listener first, then run the CLI publisher:

```sh
python3 examples/link_tcp_signal_receiver.py --port 9000
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 100 --dt 0.1ms --emit-tcp 127.0.0.1:9000

python3 examples/link_udp_signal_receiver.py --port 9001
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --steps 100 --dt 0.1ms --emit-udp 127.0.0.1:9001
```

Both scripts import `examples/link_socket_protocol.py`. TCP `LinkFrame` v1
uses the fixed same-host struct layout and the sample explicitly requires a
little-endian host with 8-byte doubles; UDP compact entries are encoded as
little-endian fields.

`examples/link_tcp_signal_plot.py` uses the same TCP decoder and displays each
`sig.*` channel as a live Matplotlib line. Install Matplotlib and optionally
limit the rolling graph history with `--history`. Both plotters drain every
pending frame/datagram but redraw at most `--draw-hz` times per second
(default 30), so receiving keeps up with fast publishers while drawing stays
at a sustainable display rate; the legend rebuilds only when the channel set
changes:

```sh
python3 -m pip install matplotlib
python3 examples/link_tcp_signal_plot.py --port 9000 --history 1000
```

The UDP equivalent is `examples/link_udp_signal_plot.py`. It has the same
rolling graph and exits after three seconds without a datagram by default;
`--idle-timeout 0` waits indefinitely.

For a continuous stream, use `sig run --forever`. It ignores `--steps`, keeps
one runner/state alive so phase and source state do not reset, and handles
Ctrl-C at the next fixed-step boundary before flushing and closing sinks:

```sh
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --dt 0.1ms --forever --emit-tcp 127.0.0.1:9000
```

### Stream rate budget and measured loopback

Emission is simulation-step based. By default the driver runs a fast batch
(tight loop, no wall-clock pacing); `--paced` spaces fixed-dt steps against
absolute steady-clock deadlines instead, with `--rate <Hz>` setting wall
steps per second (default `1/dt` = real-time). Late steps run immediately
and are counted in the trailing `paced:` line (`late=`, `max_late_us=`),
never skipped and never changing `dt` (delivery class policy is in
`docs/delivery.md`). Waiting happens outside the solver/Runner, so TCP
pending bytes are serviced during idle and Ctrl-C stays responsive at step
boundaries. `--paced` works for finite runs and with `--forever`:

```sh
./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V \
  --dt 0.1ms --paced --rate 1kHz --emit-shm /wengine-sig
```

Three rates stay separate: waveform frequency (`--sig-freq`, the physics
being simulated), sample rate `1/(dt*emit_every)`, and paced packet rate.
`--emit-every N` decimates on fixed step boundaries; use it when the
transport or Zephyr receiver cannot sustain the solver's step rate. Values
below are payload-only budgets (protocol headers add a few bytes per frame):

| Transport / snapshot | Bytes per emission | 1 kHz | 100 kHz | Guidance |
|---|---:|---:|---:|---|
| TCP full `LinkFrame` | 68,140 on wire | 68.1 MB/s | 6.81 GB/s | 100x decimation at 100 kHz yields 1 kHz / 68.1 MB/s; configured 256 KiB send buffer is only a burst buffer (a few full frames) |
| UDP compact, one particle (1 item + 6 channels) | 384 | 0.384 MB/s | 38.4 MB/s | Keep datagrams below path MTU where possible; IP fragmentation magnifies loss |
| UDP compact, one signal source (`sig.count` + source) | 160 | 0.160 MB/s | 16.0 MB/s | `LagStats` exposes network/receiver gaps by `frame_seq` |
| UART 115200, ASCII one source | ~30 | ~30 kB/s | ~3 MB/s | 8N1 carries ~384 such frames/s: use at least 3x decimation at 1 kHz and 300x at 100 kHz |

Loopback measurement on the current development host (2026-09-25, GCC
13.3, default unoptimized build `cmake -S . -B build`, `ndebug=0`):
every scenario runs an ACTIVE reader that parses and validates each
frame (sequence IDs, complete framing, values against a deterministic
per-step pattern, final counts); reader threads are stopped/joined
before their descriptors close. Vocabulary follows `docs/delivery.md`:
attempted vs. published/sent vs. dropped vs. received/missed/max_gap vs.
incomplete tail, plus p50/p99 delivery age and process CPU.

| Run | Attempted | Sender accepted / dropped | Reader (unique / missed / tail) | Delivery age p50/p99 |
|---|---:|---:|---:|---:|
| SHM flood, 2,000 x 1 signal, active reader | 187,272/s | 2,000 / 0 | 757 / 1,243 / 0 | 15 / 20 µs |
| SHM paced 1 kHz, 500 x 1 signal | 999.6/s | 500 / 0 | 496 / 4 / 0 | 230 / 771 µs |
| SHM paced 1 kHz, 500 x 64 particles | 999.6/s | 500 / 0 | 499 / 1 / 0 | 260 / 790 µs |
| SHM stalled (attach after 500) | 209,811/s flood | 500 / 0 | 1 / 499 / — (latest-only policy) | — |
| TCP flood, 2,000 x 64 particles | 24,197/s | 1,941 / 59 EAGAIN | 1,941 / 59 / 0, all decoded exact | 69 / 193 µs |
| TCP paced 1 kHz, 300 x 1 signal | 999.1/s | 300 / 0 | 300 / 0 / 0, exact | 148 / 246 µs |
| TCP stalled (no drain during 500 x 4) | 546,937/s | 7 / 492 + 1 pending | 7 / 0 / 0 after drain, exact | — |
| UDP flood, 2,000 x 1 particle | 61,239/s | 2,000 / 0 | 2,000 / 0 / 0, exact | 22 / 99 µs |
| UDP paced 1 kHz, 1,000 x 1 signal | 999.5/s | 1,000 / 0 | 1,000 / 0 / 0, exact | 103 / 175 µs |
| UDP stalled (drain after 2,000 x 1) | 108,772/s | 2,000 / 0 | 167 oldest retained / 0 interior / 1,833 tail | — |

Localization legs (`solo` solver-only, `snap` snapshot+encode, `fanout`
SHM+counter or TCP+UDP): signal `solo` ~1.59 Msteps/s, `snap` ~1.33
Msteps/s with encode alone ~5.15 Mframes/s; particle-64 `solo`
~157 ksteps/s, `snap` ~162 ksteps/s with encode alone ~861 kframes/s —
so the flood ceilings above are transport/reader-bound, not solver-bound.

These are flood/loopback numbers, not a promise of network capacity. The
flood cases intentionally exceed the reader's sustainable rate to exercise
backpressure (TCP EAGAIN drops) and drop-oldest policy (SHM ring,
UDP kernel buffer); the paced 1 kHz legs show the nominal operating
point with zero (sockets) to single-digit (SHM ring race) gaps. Stalled
legs show the loss policy per class: SHM keeps newest, UDP keeps oldest,
TCP counts backpressure loudly. Benchmark commands:
`bench_link --payload signal|particle --steps N --mode all|solo|snap|
flood|paced|stalled|fanout --rate HZ` and `bench_link_sockets
--transport tcp|udp|all --frames N --items N --payload signal|particle
--mode all|solo|snap|flood|paced|stalled|fanout --rate HZ`
(legacy `bench_link [particles] [steps]` and `bench_link_sockets
[tcp|udp] [frames] [items]` still run the historic flood).

## 6. Control plane (SIG-10, v1 scope)

Generic text-line protocol, Zephyr first client, CLI/GUI same verbs:
`hello/version`, `list`, `add`, `set-param`, `start`, `stop`, `drop`,
`ping`. Step-boundary apply: commands queue, drained between steps in slot
order (deterministic; logged to replay). Stream and control are separate
sockets (D4a-A); UART interleaves COBS-escaped control frames (D4b-A).

Concurrent endpoints (LOCKED 2026-09-23, user): any number of control
endpoints may be attached at once (e.g. Zephyr via UART + PC via TCP) and
any of them can force state changes such as pause. Arbitration is
deterministic: each step drains per-channel queues in fixed registration
order (documented at attach; default TCP-control, UART-control, CLI-local),
last-writer-wins per (source, param) within the step. Idempotent verbs
(pause + pause) commute naturally; conflicting verbs (start vs stop same
step) resolve to the fixed order, never to arrival timing. Every applied
command is appended to the replay log as (step, channel, verb, args), so a
multi-endpoint session replays exactly.

## 7. Lua source (SIG-4b, shipped 2026-09-24)

Dependency (third after glm/imgui, decision recorded before code per
policy): PUC Lua 5.4.9 via git submodule `external/lua` (no system
headers or sudo needed; the pin is the submodule commit — `lua::release()`
returns "Lua 5.4.9" and the test asserts it, so a bump fails loudly until
goldens are re-verified). Built from source as `wengine_lua` STATIC
(sources compiled as C++, SYSTEM includes, no project warning flags);
only the interpreter core + base/math/string libs compile
(io/os/package/debug/coroutine/utf8 are not built). The runtime TU
(`src/signal/lua_solver.cpp`, `wengine_signal_lua` STATIC, strict
warnings) is the only TU that knows Lua exists — engine headers stay
Lua-free. 5.4 line chosen over 5.5.x (too fresh for a determinism-critical
embed; revisit by explicit decision); PUC interpreter over LuaJIT (JIT
codegen and warmup are determinism hazards; the interpreter is auditable).

`kLua = 10` (`SignalKind`, append-only). Scripts are chunks evaluated once
per step per live source as PURE functions of four inputs — `t` (source
seconds, same base as every generator), `seed` (spawn seed, exposed per
contract), `step` (1-based step being computed, matching snapshots), `dt`
— and must return a number. Purity is structural (fresh `_ENV` per call:
sandbox + inputs only), so scripts keep no state across steps and
save/load/continue is exact (proven by test). No RNG in 4b (C `rand`
differs across libcs; scripts needing noise sum a noise source).

Sandbox: `math` (minus `random`/`randomseed`) and `string` (minus
`dump`/`format`) behind read-only PROXIES (a bare `__newindex` only guards
absent keys, so existing entries like `math.sin` stayed writable until
the proxy landed — regression-tested), plus a small pure base (`assert`,
`error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`,
`type`, `xpcall`, `_VERSION`). Absent: `io`/`os`/`package`/`debug`/
`coroutine`/`utf8`, `load`/`dofile`/`print`/`warn`/`collectgarbage`,
metatables, and `tostring`/`string.format` (address-exposure channels
that would break the value-determinism contract). Chunks load text-only
(`"t"` — bytecode rejected). Budget is an instruction count per source
per step (default 65536, configurable, 0 rejects; never wall-clock).
Every failure — syntax (rejected at configure/CLI as well), runtime
error, non-number, non-finite, overrun — is a loud `kInvalidArgument`
step failure (time/index frozen), like expressions; the solver keeps the
last message for CLI/GUI display.

Wiring: `LuaSignalSolver` is an EXTRA solver (extras step before the
primary — see solvers/run.hpp): it evaluates `kLua` slots in slot order
first (serial-keyed chunk cache, recompiled when the pool text differs,
pruned on despawn; stopped slots frozen), `SignalSolver` skips them, and
sums over Lua operands stay fresh. Scripts persist as pool text (v5);
without an extra solver the driver rejects `kLua` rather than freezing
silently. Cost (measured 2026-09-24): ~7 us/source-step for a small
script vs ~165 ns parametric — fine at 1 kHz with a few sources, not for
100 kHz-class sweeps (use expression/table there).

## 8. Determinism rules

Fixed-`dt` eval order (slot order), seeded RNG, closed-form time base,
`max_digits10` CSV round-trip (`==` on re-read), golden tests at >= 3 `dt`
scales + exact noise-sequence test. Control events are part of the replay
log. No FP/op-order changes without re-derived goldens.

## 9. Adding a physical quantity (one-entry scaling path, SIG-3)

Stamping is generic; U/I are just the first two rows. To add a quantity
(e.g. temperature):

1. `units/units.hpp`: `using Temperature = Dimension<0,0,0,0,1,0,0>;` plus
   its literal(s) — the established units recipe, unchanged.
2. `units/scales.hpp`: one `scales_for<Temperature>` row — the established
   recipe; generic suffix parsing accepts it with no other change (proven
   by `test_dimension_scaling`, which registers a test-only dimension and
   parses it through the untouched generic machinery).
3. `signal/types.hpp`: append one `kQuantities` row
   `{id, suffix, name, scales_for<...>::value, spellings}` (the new enum
   value goes first) plus one `DimensionFor` specialization line. The
   `static_assert`s fail until all three agree — the compiler enforces the
   recipe.
4. Nothing else changes: hot path (SI double + tag), pool, snapshot,
   serialize validation bound, Frame/CSV/CLI display, and
   `parse_quantity_value` all derive from the table. `StampedValue` gains
   one variant alternative for the new `Value<Dim>`; `stamp()` gains one
   branch.

CLI/GUI/control edges never grow unit tables: parsing goes through
`cli::parse_quantity_value(tag, text)`, display through
`quantity_suffix`/`quantity_name`, typed work through `stamp()` +
`DimensionFor`.

## Decision log

| ID | Question | Recommendation | Status |
|----|----------|----------------|--------|
| D1 | Signal storage: `Signals` pool in `State` vs external registry | Pool (snapshot/replay/despawn free; registry breaks snapshot semantics) | LOCKED (this doc) |
| D2a | `LinkFrame` 64-channel cap vs unlimited sources | One channel/source; over-bound loud drop + counter; paging later on real need | LOCKED A (2026-09-23) |
| D2b | CSV shape for signals | Second signals log (`step,time,sig_id,value,quantity`); particle CSV untouched | LOCKED A (2026-09-23) |
| D3 | Value type: `double`+`QuantityId` vs type-erased `Value<Dim>` | `double`+`QuantityId` + compile-time Dim map (heterogeneous pool can't template per source) | LOCKED |
| D4a | Stream vs control: separate sockets vs multiplexed | Separate sockets (stream stays non-blocking/simple) | LOCKED A (2026-09-23) |
| D4b | UART control embedding | COBS-escaped control frames interleaved with stream | LOCKED A (2026-09-23) |
| D5 | Lua state shape | Single shared `lua_State`, per-source refs, budgeted | LOCKED A (2026-09-23) |
| D6 | Combinators as source kinds vs sink-side mix | Source-kind sum (replayable, in Snapshot) + sink allowlist filter; mux needs runtime select -> SIG-10 | LOCKED, sum done SIG-5 |
| D7 | Concurrent control endpoints (Zephyr UART + PC TCP) | Fixed drain order, last-writer-wins per step, replay-logged (sec. 6) | LOCKED (2026-09-23) |
| D8 | Sum in batch runs (`run_headless`, GUI) | Rejected: identical batches can't hold distinct operands; per-source family configures sums | LOCKED (SIG-5) |
| D9 | Expression failures (div-by-zero, domain, overflow) | Loud `kInvalidArgument` step failure (time/index frozen), never NaN/inf; t=0 failure rejected at configure | LOCKED (SIG-4a, 2026-09-24) |
| D10 | Table replay edges (out-of-span, exact hits) | Clamp (hold ends), bit-exact on exact hits, linear interp between; points outside `SignalParams` (pool store + batch section) | LOCKED (SIG-4a, 2026-09-24) |
| D11 | Custom sources in batch GUI model | Expression text supported (parsed data, no I/O); tables rejected (no CSV picker — SIG-9 panel follow-up) | LOCKED (SIG-4a, 2026-09-24) |
| D12 | Lua version + vendoring | PUC 5.4.9 submodule, built from source (no system headers/sudo; pin = commit, asserted in code+test); 5.5.x deferred, LuaJIT rejected (determinism) | LOCKED (SIG-4b, 2026-09-24) |
| D13 | Lua sandbox set | math(−random)/string(−dump,−format) read-only proxies + small pure base; no io/os/package/debug/coroutine/load/metatables/tostring; text-only chunks | LOCKED (SIG-4b, 2026-09-24) |
| D14 | Lua purity (save/load safety) | Fresh `_ENV` per call: pure functions of t/seed/step/dt; no RNG, no cross-step state; scripts persist as pool text (v5) | LOCKED (SIG-4b, 2026-09-24) |
| D15 | Lua failures + budget | Instruction-count budget (never wall-clock); every failure is loud `kInvalidArgument` (frozen step), message kept for display | LOCKED (SIG-4b, 2026-09-24) |
| D16 | Lua solver wiring | Extra `ISolver` stepping before `SignalSolver` (extras-first); driver rejects `kLua` without one (never silent freeze); GUI batch rejects (no runtime — SIG-9) | LOCKED (SIG-4b, 2026-09-24) |

Build order: SIG-1 -> SIG-2+SIG-3 -> SIG-5 -> SIG-4a+SIG-6 (both shipped) ->
SIG-7 || SIG-8 -> SIG-10 (needs SIG-7 framing) -> SIG-9 -> SIG-4b (shipped
2026-09-24).
