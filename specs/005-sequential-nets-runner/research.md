# Research: Sequential Nets Runner (005-sequential-nets-runner)

**Feature**: `specs/005-sequential-nets-runner/spec.md`
**Date**: 2026-11-04
**Method**: codebase inspection (no external research needed — every seam has an in-repo precedent)

All Technical Context unknowns resolved below. No `NEEDS CLARIFICATION` remains.

## R1. Where the sequential net lives: pool in `world::State`

**Decision**: `world::State` gains two members — `logic::Netlist netlist` (topology)
plus `logic::SeqState seq` (clocked Q + serials) — plus two retained vectors,
`seq_inputs` (stimulus in block-interface order) and `seq_members` (flattened
step-member slots, empty when stepping all domains).

**Rationale**: The spec's FR-006 (snapshot/sink observation) forces the net into
`State`: the Runner only exposes `State` to solvers and only `Snapshot` to
sinks, so solver-owned topology would be invisible to every observer. This is
exactly the AGENTS.md §9 stepped-solver recipe ("pool it in `world::State`,
extend `Snapshot` + `take_snapshot` in slot order"). Stimulus and members are
retained in `State` (not recomputed) so checkpoints restart bit-exactly and
every step reads one source of truth.

**Alternatives considered**:
- *Solver-owned Netlist* (configured via setter like
  `ProjectileSolver::set_gravity`): rejected — breaks snapshot observation and
  checkpoint restart with no compensating benefit.
- *Entity-attached per-flop pool*: rejected — a netlist is one singleton graph,
  not per-entity data; entity attachment would fake a 1:1 mapping that
  `despawn_entity` would then wrongly tear down.

**Deviation note**: the pool is a singleton member, not entity-attached, so
`despawn_entity` does not touch it (documented in the header). Orthogonal to
particles/signals: a sequential run uses zero particles/sources.

## R2. Nested `RunConfig` section shape (parsed values, no strings)

**Decision**: new header `include/wengine/logic/seq_config.hpp` owning
`logic::SequentialRunConfig`:

```cpp
struct SequentialRunConfig {
  Netlist netlist;                          // topology (plain data, copied)
  std::vector<LogicValue> stimulus;         // block-interface order
  std::vector<std::uint16_t> members;       // flattened edge-flop slots
  bool step_all_domains = true;             // all vs. subset
  bool set = false;                         // explicitly provided (driver gate)
};
```

Domain *names* (`"clk"`, `"BUF1.Y"`) never enter the section: the CLI derives
`clock_domains()` with the existing `src/cli/commands/logic/common.cpp` helper,
maps names to member slots, and stores the flattened slot list. The engine
validates slots (live, edge kind DFF/JK/T, no duplicates) and never derives
names. `step_all_domains=true` requires empty `members`; `false` requires
non-empty. `set` gates the one-section-per-solver rule in `ConfiguredRun::build`
(`signal.set` precedent).

**Rationale**: follows `SignalRunConfig` exactly (parsed values only, `set`
flag, driver validates + spawns). Name resolution stays at the config edge
where strings belong; the hot path carries `uint16_t` slots only.

**Alternatives considered**:
- *Domain indices into derived order*: rejected — indices depend on derivation
  order and are less direct than slots; slots are stable within the section's
  own netlist.
- *Names in the section*: rejected — violates the no-strings-in-section rule
  and would drag string tables into the engine.

## R3. Snapshot extension (slot order, allocation-free warm path)

**Decision**: `world::Snapshot` gains three dense slot-order arrays —
`seq_serial` (`u64`, stable flop id), `seq_value` (`u8`, 0/1, Q after the step),
`seq_output` (`u8`, 0/1, settled block outputs in `block_output_count` order).
`fill_snapshot` copies all three from `State` (Q from `seq.q`, outputs from the
retained `seq_outputs` vector the solver writes each step — see R5). Nine
existing reuse-path vectors become twelve; the same warm-capacity discipline
applies. `operator==` stays defaulted (`uint8_t` vectors compare fine).

**Rationale**: mirrors the `sig_serial/sig_value/sig_quantity` triple. Outputs
are retained (not recomputed in `fill_snapshot`) to keep the emit path a pure
copy with no eval coupling in `world/`. Q-only CSV/Frame consumers ignore
`seq_output`; programmatic readback gets full parity with interactive `peek`.

**Alternatives considered**:
- *Recompute outputs in `fill_snapshot`*: rejected — couples `world/` to
  `logic/eval.hpp` and repeats solver work on every emit.
- *Q-only snapshot*: rejected — spec FR-006 demands settled outputs in recorded
  artifacts; retaining them costs one small vector.

## R4. Solver: `solvers::SequentialSolver` (`kName = "logic"`)

**Decision**: header-only `include/wengine/solvers/sequential.hpp`,
`SequentialSolver : core::ISolver` with `kName="logic"`, `kDescription`,
`create()`, `name()`, and `noexcept step()`. `step()` validates `dt`
(`>0`, finite — same gate as `SignalSolver`), then calls
`logic::seq_step_filtered(netlist, seq, stimulus, outputs, members-or-null, n)`
and maps failure to `kInvalidArgument`. Time-stamp convention: Q written during
the step is the value at `sim_time + dt` (end-of-step, matching the signal
solver's T convention). Registered in `SolverRegistry` (`projectile, signal`
→ 3; `test_solver_registry.cpp` and `test_signal.cpp` size asserts 2 → 3).
`dt` advances the clock but never gates edges (zero-delay semantics,
docs/logic.md §12).

**Rationale**: the AGENTS.md §9 new-stepped-solver recipe verbatim
(kName/kDescription/create/register/tests). Name `"logic"` aligns with the
module/session vocabulary and the frame-source entry; `sim run --solver logic`
reads as the batch counterpart of the `logic` session.

**Alternatives considered**: `kName="sequential"` — rejected: the domain
vocabulary everywhere else (command, registry, docs) is `logic`; a second name
would split completion/help vocabulary for one domain.

## R5. Per-step flow inside `step()` (single call, no allocation)

**Decision**: `step()` runs, in order: (1) `apply_async_controls` (RST/SET
forces, global), (2) `apply_level_latches` (transparent/frozen + S=R=1 refusal),
(3) `seq_step_filtered` with the retained member list (edge capture for
selected domains; unselected hold), writing settled outputs into the retained
`State::seq_outputs` vector passed as the output span. Any failure returns
`kInvalidArgument` with the pool untouched at the failing sub-step boundary
(the engine helpers are resolve-all-then-commit), so the Runner latch freezes
time/index on a clean state. Level-only nets step normally (time advances,
latches re-settle). `LogicValue` stack arrays only; `noexcept`, no syscalls,
no iostream.

**Rationale**: reuses the exact session order (`apply_async_and_report` +
`step` top: async → level → edge; `src/cli/commands/logic/step.cpp`), so batch
and interactive semantics are one shared code path, not two implementations.
Resolve-all-then-commit gives the FR-008/FR-010 atomicity (illegal S=R=1 fails
loudly, nothing committed).

**Alternatives considered**: calling only `seq_step_filtered` (which already
does async+level internally per its header) — actually this IS what happens:
`seq_step_filtered` performs level settle + edge; the discrete `apply_*`
calls handle the no-advance visibility which batch does not need. Decision:
single `seq_step_filtered` call per step (async controls resolve inside the
snapshot phase per the documented RST>SET>EN>D priority; level settle runs at
every step top). Simpler, one code path, identical outcome for fixed stimulus.

## R6. Serialization: format v6 with fallback loaders

**Decision**: bump `kStateFormatVersion` 5 → 6, appending fixed-stride seq
columns: per slot `u8 live + u8 kind + u32 counter + u8 input_count +
u8 ignored_mask + u8 ignore flags + drivers[8]×u16 + driver_inv[8]×u8 +
tie_rep[8]×u16`, then `next_counter[21]×u32`, `live_count`, seq arrays
(`valid/q/serial` per slot + `next_serial` + `live_count`), stimulus length +
bytes, member list length + slots, `step_all` flag. Derived data (fanouts,
topo order, dirty) recomputed on load, not stored. Fallback: v5/v4/v3/v2/v1
load with empty netlist/seq/stimulus (same pattern as v1→signals). v6 loader
re-checks sizes/ranges + live-sequential invariants (mirroring the signal
live-source rechecks). `Netlist`, `Gate`, `SeqState` gain defaulted
`operator==` (needed by `State::operator==`; C++20 defaulted comparison over
`std::array` members).

**Rationale**: domain-outputs hook 5 (checkpoint columns + bump + fallback +
rechecks). Fixed stride keeps the format deterministic; ~65 KB fixed cost is
acceptable for a checkpoint. Stimulus persisted so restart needs no external
input.

**Alternatives considered**: *YAML workspace as the only restart path* —
rejected: binary checkpoints are the versioned restart mechanism per the
constitution; workspace YAML is the authoring format, not the restart path.

## R7. Frame projection: `frame_from_logic_snapshot` + registry entry

**Decision**: add `link::frame_from_logic_snapshot` beside `link::Frame`
(channels `seq.<serial>.Q` in snapshot slot order + `seq.count`, no items —
mirrors `frame_from_signal_snapshot`), plus one `kFrameSources` entry
`{"logic", ...}` (2 → 3; `test_frame_source.cpp` assert updated). Sequential Q
probes are scalar time series — the faithful-projection case per
docs/domain-outputs.md §6 (points-plus-scalars suffices; topology stays in the
existing schematic canvas, no new view).

**Rationale**: AGENTS.md views rule (one adapter + one registry entry, views
untouched). Over-bound nets (>63 live flops → 1+64 channels) are refused
loudly at attach/publish time with per-cause counters, same as the signal
`1+N ≤ 64` check in `sim.cpp`.

## R8. CSV recording: `LogicCsvSink` (tidy rows, mirrors signals)

**Decision**: new `include/wengine/emit/logic_sink.hpp`, `LogicCsvSink`
(header `step,time,seq_id,value`, `max_digits10`, sticky status, never throws),
wired in `sim.cpp` when the solver is `logic` (same dispatch shape as
`SignalsCsvSink`). `seq_id` is the stable serial in snapshot slot order
(serials not ascending after recycle — same documented caveat as `sig_id`).
Re-read `==` round-trips (same `==` policy as signals CSV).

**Rationale**: spec FR-006 "recorded artifacts" needs a concrete replayable
shape; the signals sink is the exact precedent ("each domain keeps its exact
round-trip format instead of one widened schema"). UART/TCP/UDP/ROS select
vocabulary stays signal-scoped (serials exist only there per the existing
usage error); sequential uses CSV + SHM + generic sinks in v1 — explicit scope
cut, documented.

## R9. CLI surface: `sim run --solver logic` + `--seq-*` options

**Decision**: `sim run` gains `--seq-file <ws.yaml>` (required for logic),
`--seq-clocks <a,b>` (default: all domains), `--seq-from <PIN>=<0|1>` repeatable
(stimulus; unset pins default Low with the DFF-EN-defaults-High rule via the
existing `compose_block_inputs` semantics — reused helper, not a copy).
`--seq-*` with a non-logic solver is a usage error (exit 2), mirroring
`--gravity`/signal scoping; unknown domain names, empty selection, and bad
st stimulus are usage errors naming the cause. YAML loads via the existing
`logic::yaml::load_text` (engine header, yaml-cpp stays in its TU; `wengine_cli`
already links it for the `logic` command). `kSimOptions` + `complete()` cover
the new flags; `tests/test_cli_args.cpp` gains run/usage/stray/completion
cases (no registry-size change — no new top-level command).

**Rationale**: one-command-per-domain (AGENTS.md §9): sequential batch runs
extend `sim run` (which "runs any registered solver with defaults otherwise")
with solver-scoped options, never a new command. Reusing the CLI session's
`compose_block_inputs` ordering keeps batch stimulus identical to session poke
order.

## R10. What is explicitly out of scope (deferred, not drift)

- **GUI controller/panels** for sequential batch config: deferred to the GUI
  epic (docs/logic.md §12). Only the `kFrameSources` entry ships now.
- **ROS/TCP/UDP/UART select vocabulary** for sequential serials: stays
  signal-scoped; SHM latest-state viewing works via the Frame adapter.
- **Per-step stimulus programs** (waveforms over steps): v1 stimulus is static
  across the run; time-varying drive is a named follow-up.
- **Typed output blocks / paging** (domain-outputs hooks 6–7): sequential Q
  fits `Frame` faithfully; no codec work in v1.
- **New views**: none — scalar Q channels reuse graphs; topology reuse the
  schematic canvas.

## Precedent map (where each pattern was copied from)

| Decision | Precedent |
|---|---|
| Section struct + `set` flag + driver gate | `SignalRunConfig`, `ConfiguredRun::build` signal arm |
| Solver header-only `ISolver` + registry | `solvers/signal.hpp`, `solver_registry.hpp` |
| Snapshot triple + `fill_snapshot` reuse | `sig_serial/sig_value/sig_quantity` in `world/state.hpp` |
| Serialize bump + fallbacks + rechecks | `world/serialize.hpp` v5 + v4/v3/v2/v1 loaders |
| Frame adapter + registry entry | `frame_from_signal_snapshot`, `kFrameSources` |
| Tidy CSV sink + re-read `==` | `emit/signal_sink.hpp` |
| `sim run` solver-scoped options + usage errors | `sig.cpp` `--sig-*`, `sim.cpp` gravity/select scoping |
| `static_assert` registry sizes | `test_solver_registry`, `test_signal`, `test_frame_source` |
| Runner failure latch + frozen index | `core/runner.hpp` header contract |
