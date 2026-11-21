# Data Model: Opt-In Delay-Honoring Evaluator (014)

Phase 1 output. Entities of the timing mode and their validation
rules, derived from the spec's Key Entities and FR-001..FR-014; all
decisions anchored in [research.md](research.md) (D1–D12). The mode
adds **no persisted entity anywhere**: everything below is
per-invocation scratch beside the netlist, exactly like the session
poke maps and the contention channel (`SolveDiag`).

## Entity overview

```text
Netlist (existing, read-only here)      SeqState (existing, untouched here)
   │  propagation_delay_ps per gate        │  q/valid/serial per slot
   ▼                                       ▼
TimingSeed ──► run_timing_window(netlist, state-snapshot, seeds,
   │              scratch, trace)  [logic/timing.hpp]
   │                 │
   ▼                 ▼
TimingTrace (fixed capacity)      TimingScratch (fixed capacity)
   events[] + drain time            net values, pending queue, sink map
   │
   ▼  printed by the CLI helper (D8 grammar), never stored
Terminal trace block  +  unchanged settled tail (ordinary solve/step path)
```

## Entities

### TimingSeed — one net transition at the window anchor (t=0)

The input vocabulary of the wheel: what changed to open the window.

- **Fields**:
  - `target` — either a *driving pin* (`slot`, `nq` flag: a gate
    output, Q or NQ of a sequential element) or a *block input
    class* (the tie leader `(slot, input)` of a dangling input,
    fanning to its members);
  - `new_value` — a `LogicValue` (0/1 for solve/step seeds; the
    transient windows produce X/Z only inside the wheel, never in
    seeds).
- **Relationships**: produced by the adapters (D6) — solve diffs the
  applied assignment against the all-zero pre-vector; step diffs Q
  after each unchanged `seq_step_filtered` call (NQ seeds share the
  Q seed's target gate with the complement value — emitted as one
  event pair at the same timestamp); truth diffs each row against
  the all-zero vector.
- **Validation**: adapters refuse nothing new — the ordinary
  validation paths (sizes, pin classes, X/Z block inputs) already
  ran; a window with zero seeds is legal (empty trace, drain 0 ps).

### TimingEvent — one observable transition in the window

- **Fields**:
  - `t_ps` — `std::uint64_t`, whole picoseconds from the window
    anchor; strictly increasing per net; every event's time ≥ 1 ps
    after its cause (D3 unit floor);
  - `slot`, `nq` — the driving pin (the module's display identity
    renders `<NAME>.Y` / `<NAME>.Q` / `<NAME>.NQ`);
  - `old_value`, `new_value` — `LogicValue` four-state (transients
    may be X; Z appears only from a released TBUF, D5).
- **Relationships**: appended to the trace by the wheel in pinned
  order (time, then slot order; a Q event's NQ sibling follows
  immediately at the same timestamp).
- **Validation**: no-op evaluations emit nothing (D4 collapse);
  duplicates for the same (gate, time) collapse to one evaluation;
  the trace refuses loudly at `kMaxTimingEvents` (D9) rather than
  truncate.

### TimingTrace — the window's observable

- **Fields**: fixed-capacity event array + `n_events`, `drain_ps`
  (the last event's time, 0 for an empty window), `n_annotated`
  (count of nonzero `propagation_delay_ps` gates, for the header
  line), `unit_ps` (constant 1, D2).
- **Relationships**: consumed only by the CLI printer (D8 grammar)
  and by tests (goldens, parity). Never serialized, never part of
  `Snapshot`/workspace/YAML/checkpoint (FR-012; no schema or
  version bump anywhere).
- **Validation**: bounded by construction (D9 cap + D3 progress);
  end-state parity with the zero-delay result is a test duty, not a
  field.

### TimingScratch — caller-owned wheel state

- **Fields**: per-slot current output values (four-state), pending
  schedule entries (time, slot) with the collapse rule applied,
  and the reverse sink map (driver slot → consumer `(slot, input)`
  attachments in slot order, polarity via `driver_inv`/`extra_inv`
  — the same arrays `resolve_input` reads).
- **Relationships**: sized to the existing fixed caps
  (`kMaxGates`, `kMaxInputs`, `kMaxSharedExtra`); allocation-free
  after construction, like `solve`'s scratch pattern.
- **Validation**: no heap, no exceptions, no syscalls, no I/O
  (engine hot-path rules; the wheel is cold but not exempt).

### The window lifecycle (state transitions)

```text
[dry] ──seeds──► [t=0 apply] ──► [propagate] ──drained──► [report]
                     │                │
                     │   pending nonempty, time strictly
                     │   advances (D3 floor)
                     ▼                ▼
              loud refusal      loud refusal on cap/overflow (D9)
              on invalid seed   (state untouched — wheel owns no state)
```

- Every window starts at `t=0` (per-invocation / per-edge anchor,
  D6); no cross-window time accumulates.
- The wheel **writes no Netlist/SeqState field**: state commits
  belong to the unchanged stepper; the settled tail is printed by
  the unchanged `solve`/phase-2 path. This is the root of the
  spec's trajectory-parity guarantee (FR-005/FR-006).
- A settled window may still refuse through the unchanged
  validation path (settled X/Z, settled S=R=1) — transient windows
  never refuse on X alone (D5).

## Rules carried over unchanged (no new validation invented)

- Settled-state refusals ride the existing paths verbatim (spec
  FR-009): settled X/Z at level consumers, settled S=R=1, truth's
  shared/buffer refusals, solve's loud default echo.
- 013's conventions govern the read side: zero annotation =
  un-annotated (unit in this mode), O2's reset note stays the
  optimizer's own contract, save/load round-trips untouched.
- The budget/overflow refusals are the only *new* failure
  vocabulary, and they are runtime failures (exit 1) with output
  and workspace untouched (D7/D9).
