# Contract: Engine Seam — Capacity-Parameterized Logic Domain

What the engine promises after 015, and to whom. Anchored in
[research.md](../research.md) D1–D4.

## The templated capacity form

- The logic stage-path headers (types, shared_net, tie_set,
  block_interface, topo, pins, netlist, eval, seq_state, seq_step,
  optimize family) become `template <std::size_t kCap>` code over a
  dependent slot type.
- **Slot-type rule (D2)**: slot ids, flat indexes, topo levels, fanout
  counters, live/topo counts, and block-input ordinals take the
  capacity-dependent width; `kMaxInputs=8` and `kMaxSharedExtra=3`
  stay fixed.
- **Sentinel rule**: `kNoDriver`, `kNoLeader`, `kBitScratchWords`
  derive from `kCap`.
- **Monomorphic scope (D4)**: `timing.hpp`, `layout.hpp`,
  `seq_config.hpp`, `yaml_store.hpp` + store TUs, world
  serialization/checkpoints, all CLI verbs, GUI/TUI stay exactly as
  today, on the alias.

## Alias guarantees (the load-bearing promise)

1. **Type identity**: `using Netlist = NetlistT<1024>` (likewise
   `PinRef`, `SeqState`, sentinels, constants) is
   layout-identical to the pre-feature types, enforced by
   `static_assert`s (struct sizes, sentinel values, width
   spellings) shipped in `tests/test_logic_scale.cpp`.
2. **Signature identity**: every existing public function callable
   on the alias keeps its exact name, parameter list, return type,
   and `noexcept` — wrappers own the fixed stack scratch internally
   (`noexcept` pins at test_logic.cpp:808/816 keep passing
   untouched).
3. **Behavior identity**: all goldens, example replays, refusals,
   save/load round-trips, and bench measurements pass unchanged
   (spec FR-009); the burden is structural, not aspirational.

## Scratch rule (D3)

- Templated cores take caller-owned scratch as non-owning spans /
  pointer+cap pairs, sized by the formulas in
  [data-model.md](../data-model.md), validated loudly
  (`core::Status`, before any work) — the `eval_block_word`
  precedent (eval.hpp:227–229).
- Cores never allocate, never throw; by-value `Netlist` locals in
  the optimize cores become caller-provided out-buffers; the alias
  wrappers keep by-value convenience semantics.

## Instantiation policy (who may build a big one)

- Production code instantiates only the alias (1024).
- Only two artifacts may instantiate the scale capacity
  (`kScaleGates ≥ 1,000,000` / test sizes): the `bench_logic_scale`
  TU and `test_logic_scale`. No CLI verb, serializer, GUI/TUI, or
  engine run path ever sees another capacity (D10).
- Capacity is a compile-time parameter; there is no runtime
  capacity knob and no new vocabulary anywhere user-facing.

## Determinism contract at every capacity

Slot-order iteration, lowest-free-slot allocation, per-kind
never-reused counters, pinned evaluation order (generators then
combinators analog: leaders then members; slot-order Kahn), and
snapshot timing are capacity-blind: identical inputs ⇒ bit-identical
outputs at 1M exactly as at 1024. Repeated harness constructions
from identical seeds produce bit-identical nets and checksums.

## Fidelity statement (mandatory in the report)

The harness measures the u32 instantiation of the same shipping
source; production ships the u16 alias. Width affects generated
loads, not complexity class; the committed curve set includes alias
anchor points so every ladder starts inside the shipped
instantiation (research D1).

## Doxygen duty

The templated forms and scratch-taking overloads are new public
engine API: `@brief`, parameter docs, and `@file` role paragraphs
ship in the same change (constitution Development Workflow).
