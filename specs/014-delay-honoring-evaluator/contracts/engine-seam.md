# Contract: Engine Seam — `logic/timing.hpp` (014)

The single new engine module of this feature. Header-only, like its
siblings; semantics pinned in [timing-semantics.md](timing-semantics.md);
decisions in [research.md](../research.md) D1/D6/D9.

## Invariants (constitution-level, checked at review)

- **Read-only over the world**: never mutates `Netlist`, `SeqState`,
  or any session state; state commits stay with the unchanged
  stepper (`seq_step.hpp`), validation and settled outputs with the
  unchanged `solve`/phase-2 paths.
- **Fast path untouched**: `eval.hpp`, `seq_step.hpp`,
  `block_interface.hpp`, `types.hpp` gain zero code; the default
  evaluation path reads no delay field and branches on no mode
  (spec FR-007, pinned by test + unchanged `bench_logic`).
- **Engine discipline**: no heap after scratch construction, no
  exceptions, no strings, no I/O, no syscalls, `[[nodiscard]]`
  `core::Status` returns, `constexpr`/`noexcept` only where
  actually guaranteed; never includes CLI types (dependency
  direction: `logic -> core/status` only).
- **Doxygen duties**: `@file` role paragraph plus per-function
  `@brief`/`@param`/`@return` comments ship with the header in the
  same change (docs-suite constitution duty).

## API sketch (planning-level; exact signatures are tasks' scope)

```text
struct TimingSeed   { target: driving pin (slot,nq) | block-input
                      leader (slot,input); new_value: LogicValue }
struct TimingEvent  { t_ps: u64; slot: u16; nq: bool;
                      old_value, new_value: LogicValue }
struct TimingTrace  { events: fixed-cap array; n_events;
                      drain_ps; n_annotated; unit_ps }
struct TimingScratch { net values; pending schedule; reverse sink
                      map (slot order, polarity via the existing
                      driver_inv/extra_inv arrays) }

run_timing_window(netlist, seed span, trace&, scratch&) -> Status
  // One window: apply seeds at t=0, propagate transport semantics
  // to drain, fill the trace in pinned order (time, then slot
  // order; NQ pairs its Q timestamp). Loud refusal on
  // kMaxTimingEvents breach or time overflow; no partial trace on
  // refusal.

4-state gate evaluation + transient-tolerant input resolution
  // Dominant-value tables (timing-semantics.md) beside — never
  // inside — eval_gate/resolve_input; the shipped funnel
  // (resolve_net) is reused read-only.
```

Consumers: the three CLI adapters (`solve.cpp`, `step.cpp`,
`truth.cpp`) and `tests/test_logic_timing.cpp`. Nobody else links
anything new (`wengine_logic` stays an `INTERFACE` target; no
`.cpp` TU, no CMake source addition).

## Failure vocabulary (the only new Status producers)

- Event-cap breach: loud runtime failure naming `kMaxTimingEvents`
  and the offending window; workspace and settled output
  untouched (the adapter refuses before printing the trace block).
- Time-axis overflow: loud runtime failure from checked adds.

Everything else refuses through the unchanged paths (settled X/Z,
settled S=R=1, truth's shared/buffer guards, solve's size/echo
rules) — this module adds no duplicate of any existing check.

## Test pins at this seam

- Hand-derived goldens against the 1364-anchored tables and
  path-delay arithmetic (never implementation readback).
- Parity: wheel end-state == zero-delay solve; step trajectories
  equal across modes; determinism re-runs byte-identical.
- Budget: a reconvergent annotated net drives the cap refusal;
  an annotation near the u64 cap drives the overflow refusal.
- The no-read pin: default-path code contains no delay reads (the
  byte-identity suite is the behavioral proof; `bench_logic`
  unchanged is the measured proof).
