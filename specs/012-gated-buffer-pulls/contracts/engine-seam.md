# Contract: Engine Seam — Gated Buffer + Pulls (012)

**Seam**: `include/wengine/logic/{types,pins,netlist,block_interface,eval,seq_step}.hpp`
and the optimizer family. Behavioral contract — signatures are
implementation detail; every rule below is testable as stated.

## Buffer lifecycle

- **`add_gate(kTbuf, 2)`**: creates `TBUF<n>` with `drivers = {kNoDriver,
  kNoDriver}`; any other fan-in fails loudly (`bad fan-in for kind`).
  Counter comes from `next_counter[8]`, never reused.
- **Wiring** data (`TBUFx.1`) and enable (`TBUFx.2`) passes the full
  per-wire validation (live slots, input range, untied destination,
  flop-aware cycle check) — buffers bypass nothing. A buffer output
  (`TBUFx.Y`) wires as an ordinary driver, including as a shared-net
  attachment within the existing cap.
- **`remove_gate(slot)`**: releases exactly the removed gate's edges
  wherever they sit and clears the slot's `pull_cfg` flats — no
  dangling entries survive; remaining drivers resolve unchanged. The
  gate's own pulls die with its slot (like shared marks).

## Buffer evaluation (research D2)

- `eval_gate(kTbuf, {data, en}, 2)`: `en == kHigh` → `data`;
  otherwise → `kHighZ`. Precondition (both Low/High) is enforced by
  the input seams — `eval_gate` never observes X/Z.
- A buffer input presenting `kUnknown`/`kHighZ` (including dangling)
  fails loudly at that input's seam with simulation state, step count,
  and timing untouched. A dangling enable never defaults on or off;
  a dangling data never invents a value.
- Sequential phases inherit buffers with no change: every
  combinational evaluation in `seq_step.hpp` routes through
  `resolve_input` + `eval_gate`, so gated drivers work in stepped
  runs with identical per-evaluation determinism.

## Pull mapping (research D4 — the all-Z map, nowhere else)

- After the unchanged `resolve_net` fold, an all-Z span maps through
  the destination input's pull: up → `kHigh`, down → `kLow`,
  none → `kHighZ`. Driven spans (agreement, contention, unknown) are
  unaffected under every pull setting.
- The single-driver path fails loudly on a non-level sunk value
  (reachable only via Z-producing drivers); pre-012 netlists cannot
  reach it, so their evaluation is bit-identical.
- Contention counting is computed at span assembly (polarized span
  presents both low and high), before the pull map: pulls neither
  create nor mask counts, and the `contention:` readback keeps its
  shape.
- Terminated dangling inputs (pulled, unwired) resolve to idle in
  `fill_resolved` without consuming block-input columns; they are
  invisible to interface derivation and `poke`.
- Every consumer keeps the shipped contract: definite levels
  (including pulled idle levels) flow; genuinely unresolvable
  `kHighZ`/`kUnknown` inputs fail loudly with state and timing
  untouched. Detection (counting, mapping) happens at the net, before
  any downstream consumer refusal.

## Word path (four values do not fit in one bit)

- `truth` / `export --truth` refuse loudly when any live `kTbuf`
  gate exists, naming the construct and pointing at scalar `solve`
  (exit 1, never a silent misrepresentation). Pull-only netlists
  stream unchanged.
- `eval_block_word`'s `kTbuf` arm returns a loud failure (defense in
  depth for future callers; unreachable through the guarded entries;
  zero per-word cost).

## Optimizer policy (FR-016 made concrete)

- **Buffer-gate freeze**: no pass folds, drops, or retags a `kTbuf`
  gate (`const_fold_pass` and root-collapse skip them; `bypass_pass`
  never matches them; `classify` holds them at `kV`).
- **Sink-side skip**: no pass folds, drops, or retags through a
  pulled input (`has_pulled_input`, the `has_marked_input` pattern);
  `classify` treats pulled nets as variable (they resolve
  level-or-Z, never constant-foldable through the pull).
- **`exact_simplify_pass`**: conservative whole-net skip when live
  buffers or any pull exist.
- **O2 / O2-min**: loud refusal when live buffers or pulls exist —
  the netlist is left untouched, O0/O1 remain available; a
  verbatim-carry O2 emitter is the named follow-on.
- **Machinery**: `pull_cfg` participates in defaulted equality, so
  test-equality oracles inherit it; `remove_gate`/`reset_netlist`
  clear it (no stale pulls behind recycled slots); fanout recounts
  and reachability need no pull awareness (pulls fabricate no
  driver slots).

## Determinism pins

- Span folding stays multiset-order-independent (shipped property);
  the pull map is a pure function of (all-Z, level).
- Terminated-dangling derivation is slot-ordered like all interface
  derivation; identical netlists derive identical interfaces.
- No RNG, no allocation, no exceptions on new paths (pull lookup is
  a flat array read; spans reuse the fixed cap-4 stack buffer).

## Reference anchoring (constitution I/III)

Buffer evaluation is verified against the independently derived
4-row enable×data table (EN×D → out), and pulled resolution against
the shipped 010/011 reference table plus the two float-mapping rows
(all-Z + up → High; all-Z + down → Low) — goldens from the
reference, never from the implementation. Exhaustive gated-span
verification (enabled/disabled × ordinary mixes up to the shared cap,
every pull setting, all attachment orders) is permutation-invariant
by construction of the funnel.
