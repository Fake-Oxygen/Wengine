# Contract: Engine Seam — Shared Nets (011)

**Seam**: `include/wengine/logic/{types,netlist,block_interface,eval,seq_step}.hpp`
and the optimizer family. Behavioral contract — signatures are
implementation detail; every rule below is testable as stated.

## Mark lifecycle

- **`wire(from, to, input)`** on an unmarked occupied input: loud
  refusal (`kInvalidArgument`), netlist untouched — byte-identical to
  today. This is the default single-driver rule and never weakens.
- **Marking** (via the CLI `wire --shared` flow, which calls the mark
  then the wire): marks the `(slot, input)` flat; legal on unoccupied
  inputs (pre-marking) and idempotent on marked ones. Refused on tied
  inputs (ties never join a driver span).
- **Attachment onto a marked input**: appends after the primary,
  bounded by `kMaxSharedExtra = 3` extras (4 attachments total);
  overflow refuses loudly, never truncates. Every attachment passes
  the full per-wire validation: live slots, input range, untied
  destination, flop-aware cycle check (`topo::reaches`, `kCombOnly`) —
  marking bypasses nothing.
- **`unshare(slot, input)`**: clears the mark; refused loudly while
  the input holds ≥2 attachments; refused on an unmarked input.
- **`unwire(slot, input)`** removes one attachment and leaves the mark
  alone: removing the primary promotes extra 0 (slot + polarity) into
  `drivers[]`/`driver_inv` and left-compacts; removing extra j
  left-compacts. A marked input legally holds 0..4 attachments at any
  time (0 attachments resolve `kHighZ`).
- **`remove_gate(slot)`**: releases exactly the removed gate's
  attachments wherever they sit (primary or extra, polarity-aware
  fanout release per attachment, extras swept in the as-driver pass);
  no dangling entries survive; remaining drivers resolve unchanged.
  The gate's own marks die with its slot.

## Resolution (the single funnel, unchanged semantics)

- Evaluation of any input resolves through `resolve_input`; a marked
  input with extras assembles its span (primary + extras, per
  attachment polarity applied, compact index order) and folds it with
  the shipped `resolve_net`: agreement → level, low+high → `kUnknown`,
  no attachments → `kHighZ`, order-independent.
- Inputs with ≤1 attachment (marked or not) take today's path
  structurally: read `drivers[]`, one-element span, `driver_inv`
  polarity. Unmarked netlists never assemble a span.
- Every consumer keeps the shipped contract: definite levels flow;
  `kHighZ`/`kUnknown` inputs fail loudly with simulation state and
  step timing untouched — gate inputs (`eval.hpp`), sequential control
  and clock nets (`seq_step.hpp`), block sinks. A contended shared net
  feeding such a consumer fails loudly **after** the count increments
  (detection happens at the net).
- The word-parallel path (`eval_block_word`, `resolve_word`) cannot
  carry four values: it refuses loudly when any shared input holds
  ≥2 attachments (naming the construct); marked inputs with ≤1
  attachment ride the primary word path unchanged. Measured
  refinement (D11): the refusal is enforced once at each word-loop
  entry (`truth`/`draw`/`export` verbs via `support::write_truth_*`,
  `bench_logic` before its timed region) through the same
  `shared_word_blocked` seam — not once per word — because a per-word
  scan is the same complexity class as the word eval itself and
  regressed `bench_logic` ~15% (measured). Loudness is preserved at
  every entry; the hot path stays branch-identical to v1.

## Contention diagnostics (observation-only)

- `SolveDiag` is a POD per-flat counter array, passed by defaulted
  pointer to `solve` and the seq stepping entry points (null = no
  counting; all existing callers compile and behave identically).
- A resolution whose polarized span presents both low and high
  increments `per_net[flat]` by exactly one — computed at span
  assembly, independent of `resolve_net`'s X logic (future X/Z
  sources cannot false-count).
- Counting never alters values, state, timing, or outputs: identical
  runs with and without a channel produce byte-identical value
  streams; identical runs with a channel produce identical count
  sequences.
- The engine `Runner`/`SequentialSolver` path attaches no channel —
  counting is an interactive-module diagnostic surfaced by CLI verbs.
- A step's settle phases each resolve shared nets; every contended
  resolution counts (a persistently contended net counts once per
  phase). Documented, deterministic.

## Determinism pins

- Span folding is multiset-order-independent (shipped `resolve_net`
  property); span assembly order is pinned (primary first, extras in
  compact index order).
- Promotion/compaction orders are pinned (extra 0 promotes; left
  compact) — attachment sequences in different orders converge to
  identical array states.
- No RNG, no allocation, no exceptions on the fold path (stack span
  buffer, fixed 4).

## Optimizer policy (FR-017 made concrete)

- **Driver-side freeze**: a gate with any attachment into a marked
  input is never removed, retagged, or rewired-through (the
  `feeds_seq` freeze pattern, applied at transform decision points —
  never inside `rewire_users`); DCE treats such gates as roots.
- **Sink-side skip**: no pass folds, drops, or retags through a
  marked input; `classify` treats marked nets as variable.
- **`exact_simplify_pass`**: conservative whole-net skip when marked
  inputs exist (its documented sequential-refusal flavor).
- **O2 / O2-min**: loud refusal when marked inputs exist — the
  netlist is left untouched, O0/O1 remain available; a verbatim-carry
  O2 emitter is the named follow-on.
- **Machinery**: fanout recounts, reachability (`follow_drivers`,
  `compute_feeds_seq`), `remove_gate` sweeps, and test-equality
  oracles account for extras — a pass that dropped or duplicated an
  extra is a correctness bug, not a policy choice.

## Reference anchoring (constitution I/III)

Multi-driver resolution is verified against the same independently
derived IEEE-1164-anchored four-value table that pins the funnel
(specs/010 contracts/resolve-net.md), extended through the netlist
seam: exhaustive L/H multisets over 2..4 attachments, per-attachment
polarity mixes (including Q+NQ self-contention), plus permutation
invariance — goldens from the reference, never from the
implementation.
