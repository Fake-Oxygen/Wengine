# Data Model: Sequential Session Verbs

**Feature**: `specs/001-seq-session-verbs/spec.md` | **Date**: 2026-10-31

Sources: spec Key Entities + research D1–D5. Engine structs (`Netlist`,
`SeqState`) are pre-existing and unchanged except the additive creation
path (D2); this model covers the new CLI-edge state and verb views.
All ordering is slot order unless stated.

## Entities

### 1. ClockDomain (derived view, not stored)

- **What**: The set of live sequential gates whose CLK/EN input (input 1,
  spelled `CLK`/`EN`) is driven by one net, plus the implicit domain for
  undriven/ignored CLK/EN pins.
- **Fields**: `name` (driver source pin text, e.g. `BUF1.Y` or
  `DFF2.NQ`, or the literal `clk` for the implicit domain); `members`
  (flop slots in slot order).
- **Derivation**: Recomputed per `step` from `Netlist` drivers (wired
  input 1 → driver slot's output pin; dangling/ignored input 1 →
  implicit domain). No storage, so it can never go stale.
- **Validation**: `step <domain>` rejects unknown domain names as a
  usage error (FR-007); a domain that exists but drives nothing cannot
  occur by construction (domains derive from live drivers).

### 2. CycleCounter (CLI-edge session state)

- **What**: Successful-edge count per clock domain since session start
  (or last `reset`).
- **Fields**: `domain → uint64 count`; starts empty (implicit 0).
- **Transitions**: `step <d> N` (success) → `count[d] += N` (N=0 reports,
  no increment); `reset` → map cleared; failed `step` → untouched.
- **Validation**: N parsed as strict non-negative integer; negative or
  non-numeric N is a usage error with no state change (spec edge case).

### 3. PokeEntry (CLI-edge session state)

- **What**: One sticky driven value on a dangling block-input pin.
- **Fields**: `leader_flat` (tie-class leader index, `slot*kMaxInputs+input`);
  `value` (`kLow`/`kHigh`); display pin (the poked spelling, canonical
  named form e.g. `DFF1.D` — numeric aliases resolve to the same entry).
- **Transitions**: `poke PIN=0|1` → upsert (wired/ignored/output/unknown
  pins rejected loudly, no change); `poke PIN=release` (or `unpoke`) →
  delete entry; `reset` → map cleared; `step`/`solve` → read-only use.
- **Validation**: Reuses `parse_pin` + tie-leader resolution (same path
  as `solve --from`); duplicate pokes overwrite (last wins, reported);
  `step` composes the `block_inputs` vector in interface order from the
  map with unset pins defaulting Low (echoed as `(defaults assumed)`).

### 4. FlopView (read model for `peek`/`step` reports)

- **What**: One observed flop: `gate_name` (e.g. `DFF1`), `q` (0/1),
  `nq` (always NOT q), `serial` (never-reused `SeqState` id).
- **Source**: `SeqState.q[s]` / `serial[s]` after `init_seq_state` sync;
  `nq` is computed at read time, never stored. Q is always Low/High by
  construction (power-on Low, commits write snapshots only —
  `seq_step.hpp` defensive check stays authoritative).
- **Ordering**: Slot order in all scopes (single flop, per block, whole
  workspace); whole-workspace output is therefore byte-stable for
  scripted assertion (FR-005).

### 5. AdvanceResult (verb report)

- **What**: The observable outcome of one `step` call.
- **Fields**: `domain`, `requested N`, `resulting cycle count`,
  settled block outputs (`PIN=value` lines, end-of-step values from
  phase-2 re-settle), changed-flop list (name old→new for flops whose Q
  flipped; empty advance reports `(no change)`).
- **Failure atomicity**: Engine validates fully before committing
  (`seq_step` touches no state on failure); CLI increments counters and
  prints only on success — a failed step leaves poke map, counters, and
  `SeqState` exactly as before (FR-007).

### 6. OutputPin / sink polarity (engine state, additive)

- **What**: Which output of a gate a wire reads: `Q` (default, all
  existing behavior) or `NQ` (sequential kinds only, reads NOT Q live
  at every evaluation).
- **Fields**: `PinRef`/`BlockPin` output selector; `Netlist` per-sink
  polarity array (zero = non-inverted); `nq_fanout` per gate
  (`output_fanout` keeps Q-only semantics); per-NQ ignore flag
  (`ignore_output` keeps Q-only semantics).
- **Transitions**: `wire DFF1.NQ X.1` → edge stored + polarity set +
  `nq_fanout++`; `unwire X.1` → edge cleared + polarity cleared +
  counter decremented; `remove_gate` → all incident edges and their
  polarity cleared with the gate.
- **Validation**: `NQ` on combinational gates, and named pins
  (`D`/`CLK`/`EN`/`Q`) on combinational gates, are parse-time usage
  errors; numeric forms always accepted.

## Relationships

```text
Netlist ──derives──▶ ClockDomain ──selects──▶ flop subset ──step──▶ AdvanceResult
   │                                                        ▲
   ├──interface order──▶ block_inputs ◀── PokeEntry map ────┘
   ├──wires + polarity──▶ sink values (Q live, NQ = NOT Q at read time)
   └──slots ──init_seq_state──▶ SeqState.q/serial ──▶ FlopView (Q + NQ)
CycleCounter[domain] += N on successful AdvanceResult
```

## State-transition summary (session lifetime)

```text
session start: workspace empty, poke map empty, counters empty
add/wire/...: workspace mutates (existing rules); poke entries naming
              removed/rewired pins are dropped loudly on next use
              (validated per call, never cached across topology change
              without re-resolution)
poke:         map upsert / delete (validated, usage errors change nothing)
step:         validate domain+N → build inputs from map → subset-step →
              commit counters + report (or loud failure, nothing changes)
peek:         read-only sync + print (all scopes)
reset:        workspace cleared + poke map cleared + counters cleared
save/load:    workspace only (poke/counters are session-ephemeral, D3)
```
