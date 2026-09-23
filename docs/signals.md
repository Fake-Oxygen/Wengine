# Signals design (SIG-1)

Goal: unlimited signal sources (sine/square/triangle/saw/DC/pulse/PWM/noise/
expression/table/Lua) stamped with physical quantities (U/I first), routed
N:M to sinks, controllable via CLI/GUI/generic control plane, streamed over
SHM (existing) + TCP/UDP + UART for Zephyr HIL. Variable rate: 1 kHz nominal,
sub-Hz..100 kHz-class `dt` without hard limits (decimation + budgets).

Locked 2026-09-23 (user): signals ARE stepped solvers; bidirectional control
plane is v1 scope; custom = closed expression + table now, Lua as gated 4b;
U/I first with one-entry scaling; TCP/UDP || UART parallel tracks.

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

## 4. Routing

`Snapshot` carries ALL live source values (dense slot order). Per-sink
selection lives in adapters/filters at the edge, never in `Runner`:

- `Runner` fan-out already gives 1 source -> N sinks (attach N sinks).
- N sources -> 1 sink via explicit combinator sources (sum/mux as a source
  kind reading sibling slots) or via sink-side allowlist filter.
- Rate decimation via `emit_every` (global) + approved `StrideSink` wrapper
  (per-sink stride, no `Runner` change).

## 5. Transports (stream-out)

All publishers are `emit::ISink` at the app edge, non-blocking,
copy-and-return, drop-oldest, sticky `Status`, over-bound loud drop:

- SHM: reuse `ShmPublisher` with the signal adapter + domain `"signal"`.
- TCP/UDP (SIG-7): `TcpPublisher`/`UdpPublisher` (`src/link/`, POSIX-first),
  length-prefixed `LinkFrame` bytes, same versioning wording as SHM.
- UART (SIG-8): `UartPublisher` (`--emit-uart`), ASCII line framing
  `step,time,ch_id,value` + optional COBS binary; never back-pressures sim.
- Rate budget (1 kHz sine, 1 source): SHM/TCP trivial; UART 115200 8N1 ASCII
  (~30 B/line -> ~3000 lines/s) carries 1 kHz comfortably, 100 kHz-class
  needs `emit_every`/binary decimation. Budget table ships in SIG-7/8 docs.

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

## 7. Lua source (SIG-4b, dep-gated)

Single shared `lua_State` in an isolated `wengine_signal_lua` STATIC TU
(never in hot headers), per-source function refs, sandboxed libs (math only,
no `io/os`), per-step instruction budget -> `Status` on overrun, seed
exposed to script. Version pin + policy note required before code (third
third-party dep after glm/imgui).

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
| D6 | Combinators as source kinds vs sink-side mix | Source-kind sum/mux (replayable, visible in Snapshot) + sink allowlist filter | LOCKED |
| D7 | Concurrent control endpoints (Zephyr UART + PC TCP) | Fixed drain order, last-writer-wins per step, replay-logged (sec. 6) | LOCKED (2026-09-23) |

Build order: SIG-1 -> SIG-2+SIG-3 -> SIG-5 -> SIG-4a+SIG-6 -> SIG-7 || SIG-8
-> SIG-10 (needs SIG-7 framing) -> SIG-9 -> SIG-4b (dep-gated).
