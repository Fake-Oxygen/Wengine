# Data Model: Data-Parallel Word Ops for Bit-Parallel Evaluator

**Date**: 2026-11-22 | **Spec**: [spec.md](spec.md) | **Research**: [research.md](research.md)

No persisted state changes. All entities below are transient
(evaluation scratch) or derived (pool workspace, never serialized).
Serialization bounds, checkpoint format, and `Netlist` equality are
untouched.

## Entities

### 1. Word slice (standing — unchanged)

- **What**: 64 consecutive truth rows. Word `w` covers rows
  `[w*64, w*64+63]`; lane `b` holds row `w*64+b` as bit `b`.
- **Fields**: `word_index: uint64_t` (slice ordinal).
- **Validation**: for `n_inputs < 31`, `word_index <
  bit_words_for_inputs(n_inputs)` else `kInvalidArgument`
  (standing rule, `eval.hpp:291-296`).
- **Relationships**: input `j`'s bit in row `r` is `(r>>j)&1`
  (shared with scalar `solve` and the `truth` verb).

### 2. Word batch (new)

- **What**: `W = kWordBatchWidth = 4` consecutive slices evaluated
  in one level-order traversal. Batch `B` covers slices
  `[B*W, B*W+W-1]`, i.e. 256 truth rows.
- **Fields**: `word_base: uint64_t` (first slice), fixed width `W`.
- **Validation**:
  - `word_base + W` range-checked under the same `n_inputs < 31`
    rule (every lane validated, not just the base);
    `n_inputs >= 31` accepts every base (standing unbounded-slice
    rule).
  - Tail slices (`count % W != 0`) never form a short batch —
    they route to the single-slice entry.
- **State transitions**: none (pure evaluation; no netlist
  mutation, `dirty` flag untouched).

### 3. Batch gate scratch (new, caller-owned primary)

- **What**: `n_slots × W` words, slot-major:
  `base[slot*W + k]` = gate `slot`'s word for slice
  `word_base + k`.
- **Validation**: `gate_scratch.size() >= slot_count(nl) * W`
  else loud `kInvalidArgument` before any work (standing
  scratch discipline).
- **Relationships**: read by `resolve` for wired inputs
  (`gate_scratch[drv*W + k]` with per-attachment NQ polarity
  `~w` per lane); written once per gate in level order.
- **Sizing helper**: `batch_words(n) = n * kWordBatchWidth`
  beside `bit_words_for_inputs` (`block_interface.hpp`).

### 4. Batch block outputs (new, caller-owned)

- **What**: `block_output_count(nl) × W` words; output `i`, lane
  `k` holds output `i`'s word for slice `word_base + k`.
- **Validation**: `block_outputs.size() == block_output_count *
  W` exactly (standing exact-size rule, `eval.hpp:288-290`
  generalized); partial-word mask applied per word with the
  standing formula (`eval.hpp:374-380`).
- **Ordering**: output order = standing interface output order
  (slot order; NQ second outputs `~w` per lane for sequential
  kinds with `nq_fanout == 0`).

### 5. Pool batch workspace `ws_wordblock` (new, derived)

- **What**: pool-owned `n_slots × W` staging behind the
  pool-backed convenience overload (mirrors `ws_words`).
- **Lifecycle**: amortized growth in `ensure_workspace`; never
  serialized; excluded from `Netlist::operator==` (workspace
  precedent); rebuilt lazily after load/reset like all
  workspace.
- **Validation**: undersized use refuses loudly; steady state
  allocates nothing.

### 6. Path selection (behavioral, no stored flag)

- **What**: callers use the batched entry for full batches and
  the single-slice entry for tails; builds without any
  acceleration support compile the same source (scalar batch
  loop auto-vectorized where the optimizer applies).
- **Invariant**: path choice affects only speed — the parity
  matrix (contracts/parity-matrix.md) proves word-identity
  across paths on every corpus net.

## What this feature does NOT model

- No new gate semantics (gate ops `&|^|~` reused verbatim).
- No new failure codes (standing `kInvalidArgument` vocabulary).
- No checkpoint/workspace-format change (`ws_wordblock` follows
  the existing never-serialized workspace rule).
- No CLI surface (no flags, no options; adoption is internal to
  existing loops).
