# Data Model: Sequential Nets Runner

**Feature**: `specs/005-sequential-nets-runner/spec.md` | **Research**: `research.md`

## Entities

### 1. SequentialRunConfig (new, `wengine/logic/seq_config.hpp`)

Nested solver section aggregated into `core::RunConfig` (one member
`logic::SequentialRunConfig seq`, mirroring `signal::SignalRunConfig signal`).

| Field | Type | Meaning |
|---|---|---|
| `netlist` | `logic::Netlist` | gate topology (plain data, copied at build) |
| `stimulus` | `vector<LogicValue>` | driven values in `block_input_count` order; Low/High only |
| `members` | `vector<uint16_t>` | flattened edge-flop slots to step; empty iff `step_all_domains` |
| `step_all_domains` | `bool` | `true` = every live edge flop steps (default) |
| `set` | `bool` | explicitly provided; driver rejects when solver ≠ logic |

**Validation** (driver build, before any step; all `kInvalidArgument`, state untouched):
- `set && solver != "logic"` → reject; `solver == "logic" && !set` → reject (no defaults run: topology is required).
- `stimulus.size() == block_input_count(netlist)`, all Low/High.
- `step_all_domains ? members.empty() : !members.empty()`.
- every member is a live slot of edge kind (DFF/JK/T); no duplicates.
- netlist passes `validate_sequential`.
- `steps >= 1`, `dt` finite and `> 0` (shared `validate_runner_config`).

### 2. State pool (extended `world::State`)

| Member | Type | Lifetime |
|---|---|---|
| `netlist` | `logic::Netlist` | populated once at build from the section; immutable during the run |
| `seq` | `logic::SeqState` | synced at build (`init_seq_state`), mutated per step |
| `seq_inputs` | `vector<LogicValue>` | retained stimulus copy (interface order) |
| `seq_members` | `vector<uint16_t>` | retained member list (empty = all) |
| `seq_outputs` | `vector<LogicValue>` | retained settled outputs, rewritten every step |

`despawn_entity` does not touch the sequential pool (singleton, not
entity-attached — documented deviation, research R1). `State::clear()` resets
all five. `operator==` covers all five (new defaulted `==` on
`Gate`/`Netlist`/`SeqState`).

### 3. Snapshot rows (extended `world::Snapshot`)

Dense, slot order over live sequential slots (same discipline as signals):

| Array | Type | Content |
|---|---|---|
| `seq_serial` | `vector<uint64_t>` | stable flop serial (`SeqState::serial`), never reused |
| `seq_value` | `vector<uint8_t>` | Q after the step (0/1) |
| `seq_output` | `vector<uint8_t>` | settled block outputs in `block_output_count` order (0/1) |

`sizes equal` invariant: `seq_serial.size() == seq_value.size()`;
`seq_output.size() == 0` only when the net has no block outputs. `fill_snapshot`
copies all three reusing warmed capacities (twelve reuse vectors total).

### 4. Clock Domain (derived view, unchanged)

One domain per clock-driver net over edge flops (DFF/JK/T); dangling CLK pins
form the implicit `"clk"` domain; level latches belong to no domain. Derivation
is the existing slot-order algorithm (engine-side copy beside the solver for
validation; CLI-side helper unchanged for name → slot mapping). No stored
entity — derived from `netlist` whenever needed.

### 5. Checkpoint columns (format v6, `world/serialize.hpp`)

Appended after the v5 Lua blob, fixed stride: per slot `live/u8, kind/u8,
counter/u32, input_count/u8, ignored_mask/u8, ignore flags/u8×2,
drivers[8]×u16, driver_inv[8]×u8, tie_rep[8]×u16`; then
`next_counter[21]×u32`, `live_count/u16`; seq `valid/q/serial` per slot +
`next_serial/u64` + `live_count/u16`; stimulus `len/u32 + bytes`;
members `len/u32 + slots`; `step_all/u8`. Derived data (fanouts, topo order,
`dirty`) recomputed on load. v1–v5 files load with an empty sequential pool.

## Relationships

```text
SequentialRunConfig ──build──▶ State{netlist, seq, seq_inputs, seq_members, seq_outputs}
State ──step──▶ SequentialSolver::step ──writes──▶ seq.q + seq_outputs
State ──emit──▶ Snapshot{seq_serial, seq_value, seq_output} ──adapters──▶ Frame / CSV
State ──save/load──▶ v6 checkpoint (topology + Q + stimulus + members)
CLI --seq-file/--seq-clocks/--seq-from ──parse+resolve──▶ SequentialRunConfig
```

## State transitions (per fixed step)

```text
validate dt ──▶ async forces (RST>SET) ──▶ level settle (EN-gated / SR, S=R=1 loud)
──▶ edge snapshot (selected members only) ──▶ parallel commit
──▶ phase-2 settle ──▶ write seq_outputs ──▶ advance sim_time/step_index ──▶ emit
```

Any sub-step failure → `kInvalidArgument`, pool untouched at that boundary,
Runner latches, time/index frozen, emit skipped. Level-only nets traverse the
same transitions with an empty edge set (time still advances).
