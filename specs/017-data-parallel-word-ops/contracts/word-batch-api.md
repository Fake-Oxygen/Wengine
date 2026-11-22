# Contract: Batched Word-Evaluation API

**Date**: 2026-11-22 | **Spec**: [../spec.md](../spec.md) | **Research**: [../research.md](../research.md)

Engine-internal header contract (no CLI/serialization/wire
surface). All items live in `wengine::logic`.

## New constants and helpers (`types.hpp` + `block_interface.hpp`)

`kWordBatchWidth` lives in `types.hpp` (pool-shape vocabulary beside
`kMaxInputs` — the pool workspace sizes from it and `block_interface.hpp`
depends on `types.hpp`, never the reverse); the batch scratch helper
lives in `block_interface.hpp` beside the other word geometry.

```cpp
/** @brief Slices per batched pass: one traversal yields W words per gate. */
inline constexpr std::size_t kWordBatchWidth = 4;

/** @brief Batch scratch words for n slots: n * kWordBatchWidth. */
[[nodiscard]] constexpr std::size_t batch_words(std::size_t n_slots) noexcept;
```

## New entries (`eval.hpp`)

```cpp
/** @brief Evaluate W consecutive slices [word_base, word_base+W) in one traversal.
 *
 * Bit-identical per slice to eval_block_word: same row/lane convention,
 * same guards (four-value nets refused at entries; kTbuf arm fails loudly;
 * sequential kinds contribute zero words), same per-word partial masking,
 * same loud kInvalidArgument on undersized scratch, output-size mismatch,
 * or out-of-range lanes (n_inputs < 31 rule applied to every lane).
 * Steady state allocates nothing and throws nothing.
 */
[[nodiscard]] inline core::Status eval_block_words(
    Netlist &nl, std::uint64_t word_base,
    std::span<std::uint64_t> gate_scratch,   // >= slot_count * W
    std::span<std::uint64_t> block_outputs); // == block_output_count * W

/** @brief Pool-backed convenience: grows ws_wordblock amortized, then evaluates. */
[[nodiscard]] inline core::Status eval_block_words(
    Netlist &nl, std::uint64_t word_base,
    std::span<std::uint64_t> block_outputs);
```

## Shared gate-op helper + lane resolver (`eval.hpp`, internal)

```cpp
/** @brief One gate's word op over W lanes (AND/OR/XOR/NAND/NOR/XNOR/NOT/BUF).
 * Pure, capacity-free, noexcept — the single spelling of gate truth
 * for the batched walk (the single-slice walk keeps its frozen body;
 * the parity corpus proves the two spellings agree bit-exactly).
 */
```

`resolve_word_lane` mirrors `resolve_word`'s case tree lane by lane
over the same `InputResolution` seam (strided slot-major scratch
reads + per-lane column words). `resolve_word` itself stays frozen —
the mirror is the price of not touching the reference path, and the
twin structure keeps the two visually diffable.

## Untouched (contract frozen)

- `eval_block_word` signature, semantics, and body shape (portable
  reference + tail engine).
- `input_column_word`, `resolve_word`, `bit_words_for_inputs`,
  `block_input_count`/`block_output_count`, guard predicates
  (`shared_word_blocked`, `has_buffer_gates`).
- `Netlist` persisted fields, `operator==`, checkpoint format.
- Caller-visible CLI behavior of `truth` (output bytes unchanged).

## Error contract

| Condition | Result |
|---|---|
| `gate_scratch.size() < slot_count * W` | `kInvalidArgument`, before any work |
| `block_outputs.size() != block_output_count * W` | `kInvalidArgument`, before any work |
| any lane out of range (`n_inputs < 31` rule) | `kInvalidArgument`, before any work |
| topology defect (cycle, dangling) | `kInvalidArgument` via `topo::compile` |
| live `kTbuf` reaching the walk | `kInvalidArgument` (standing safety net) |
| four-value net via guarded entries | refused before entry (standing) |
