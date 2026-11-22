#pragma once

/**
 * @file
 * BlockInterface seam (see GLOSSARY.md) of the logic pipeline: the derived block boundary,
 * never stored. One block input per tie class (canonical leader in slot
 * order) plus dangling outputs in slot order, minus ignored pins. Only
 * this header (beside tie_set.hpp for raw tie_rep) derives interface
 * order. Scalar solve, bit-parallel eval_block_word, the sequential
 * stepper, and the CLI poke composer are adapters: they enumerate via
 * block_input_pin / input_index and resolve via fill_resolved.
 *
 * Capacity (015): dynamic pool — every entry takes `const Netlist &`
 * and bounds scans by slot_count(nl), never kMaxGates. Scratch-taking
 * cores (build_input_positions, fill_resolved) keep their existing
 * caller-buffer shapes.
 *
 * Dependency rules (docs/logic.md sections 4, 8): header-only,
 * allocation-free, exception-free; caller-owned buffers throughout
 * (same contract as block_interface). Depends only on input_resolution.hpp,
 * shared_net.hpp, tie_set.hpp, types.hpp, and core/status. Shared representation
 * (shared_meta / extra_*) is owned by shared_net.hpp — this header
 * calls that seam and never names bit shifts, masks, or strides.
 * Logic is a standalone domain: this header must never depend on CLI
 * types. Consumers: eval.hpp, seq_step.hpp, the CLI solve/truth/poke
 * verbs, and the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/input_resolution.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wengine::logic {

/**
 * @brief Per-net contention counters (011): diagnostic session state, never serialized.
 *
 * One counter per destination input (tie_flat index). resolve_input
 * increments per_net[flat] by exactly one per low-vs-high contended
 * resolution when a channel is attached (defaulted nullptr = no
 * counting; value streams byte-identical with or without a channel).
 * Owned beside the other session state at the app edge (CLI logic
 * session), cleared by reset, never written to workspaces.
 *
 */
struct SolveDiag {
  /** @brief Contended-resolution count per destination input. */
  std::vector<std::uint32_t> per_net{};
};

namespace detail {
/**
 * @brief Shared dangling-input predicate: live gate, unwired, non-ignored input.
 *
 * @param[in] nl Netlist holding the gate.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return True when the pin is dangling (unwired and non-ignored).
 */
[[nodiscard]] inline bool is_dangling_input(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return false;
  }
  const Gate &g = nl.slots[slot];
  return g.live && input < g.input_count && g.drivers[input] == kNoDriver && (g.ignored_mask & static_cast<std::uint8_t>(1u << input)) == 0;
}

/**
 * @brief Shared block-input predicate: dangling pin that leads its tie
 *        class. The single filter behind every interface enumeration.
 *
 * A pulled dangling input is *terminated* (012): it resolves to its
 * idle level in fill_resolved and is never a block input — block
 * counts, interface derivation, input_index, and poke all see a
 * terminated net. Pulls and ties are disjoint by construction, so no
 * tie interplay hides here.
 *
 * @param[in] nl Netlist holding the gate and tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return True when the pin is a block input (dangling leader).
 */
[[nodiscard]] inline bool is_block_input(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (!is_dangling_input(nl, slot, input)) {
    return false;
  }
  const std::size_t flat = tie_flat(slot, input);
  if (is_pulled_flat(nl, flat)) {
    return false; // terminated: resolves to idle, consumes no column
  }
  return tie_leader_flat(nl, flat) == flat;
}
} // namespace detail

/**
 * @brief Batch form of input_index: one scan fills the full
 *        (slot,input)->ordinal map.
 *
 * kNoDriver marks wired/ignored/dead pins; tied members share their
 * leader's column. Lets word-eval build its column map in two O(G)
 * passes instead of one O(G) query per pin; the map is bit-identical
 * to per-pin input_index calls.
 *
 * @param[in] nl Netlist to scan (slot order, leaders first).
 * @param[out] in_pos Caller buffer of kCap*kMaxInputs ordinals.
 */
inline void build_input_positions(const Netlist &nl, Slot *in_pos) noexcept {
  for (std::size_t f = 0; f < slot_count(nl) * kMaxInputs; ++f) {
    in_pos[f] = kNoDriver;
  }
  // Pass 1: leaders take ordinals in slot order (same order input_index counts).
  Slot ordinal = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_block_input(nl, s, i)) {
        continue;
      }
      in_pos[static_cast<std::size_t>(s) * kMaxInputs + i] = ordinal++;
    }
  }
  // Pass 2: tied members share their leader's column (input_index mapped
  // members to the leader ordinal; the batch build must reproduce that).
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_dangling_input(nl, s, i)) {
        continue;
      }
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + i;
      const std::size_t leader = tie_leader_flat(nl, flat);
      if (leader != flat) {
        in_pos[flat] = in_pos[leader];
      }
    }
  }
}

/**
 * @brief Derived block input count: one per tie class (canonical leader
 *        in slot order), minus ignored pins.
 *
 * @param[in] nl Netlist to scan.
 * @return Number of block inputs.
 */
[[nodiscard]] inline std::size_t block_input_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_block_input(nl, s, i)) {
        continue; // wired, ignored, or non-leader members share the column
      }
      ++n;
    }
  }
  return n;
}

/**
 * @brief Derived block output count: dangling outputs in slot order,
 *        minus ignored pins (Q plus the NQ second output on sequential
 *        gates whose NQ is exposed).
 *
 * @param[in] nl Netlist to scan.
 * @return Number of block outputs.
 */
[[nodiscard]] inline std::size_t block_output_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      ++n; // Q (or the single output on combinational gates)
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      ++n; // NQ second output
    }
  }
  return n;
}

/**
 * @brief Derived block interface (never stored): fill caller buffers
 *        with block inputs (one per tie-class leader, slot order) and
 *        block outputs (dangling outputs, slot order).
 *
 * @param[in] nl Netlist to scan.
 * @param[out] in_buf Caller buffer for input pins (may be nullptr to
 *             query counts only).
 * @param[in] in_cap Capacity of in_buf.
 * @param[out] in_n Needed input count (always written, may be nullptr).
 * @param[out] out_buf Caller buffer for output pins (may be nullptr).
 * @param[in] out_cap Capacity of out_buf.
 * @param[out] out_n Needed output count (always written, may be nullptr).
 * @return kOk when both buffers fit; kOutOfRange when either buffer is
 *         too small (counts are still reported).
 */
[[nodiscard]] inline core::Status block_interface(const Netlist &nl, PinRef *in_buf, std::size_t in_cap, std::size_t *in_n, PinRef *out_buf,
                                                  std::size_t out_cap, std::size_t *out_n) noexcept {
  std::size_t need_in = 0;
  std::size_t need_out = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_block_input(nl, s, i)) {
        continue;
      }
      if (in_buf != nullptr && need_in < in_cap) {
        in_buf[need_in] = PinRef{.slot = s, .is_output = false, .input = i};
      }
      ++need_in;
    }
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      if (out_buf != nullptr && need_out < out_cap) {
        out_buf[need_out] = PinRef{.slot = s, .is_output = true, .input = 0, .negated = false};
      }
      ++need_out;
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      if (out_buf != nullptr && need_out < out_cap) {
        out_buf[need_out] = PinRef{.slot = s, .is_output = true, .input = 0, .negated = true};
      }
      ++need_out;
    }
  }
  if (in_n != nullptr) {
    *in_n = need_in;
  }
  if (out_n != nullptr) {
    *out_n = need_out;
  }
  if (need_in > in_cap || need_out > out_cap) {
    return core::Status::failure(core::StatusCode::kOutOfRange);
  }
  return core::Status::success();
}

/**
 * @brief Leader pin for one block-input ordinal (slot order, leaders
 *        only).
 *
 * @param[in] nl Netlist to scan.
 * @param[in] ordinal Block-input ordinal to look up.
 * @param[out] slot_out Slot of the leader pin.
 * @param[out] input_out 0-based input of the leader pin.
 * @return True on success; false for out-of-range ordinals.
 */
[[nodiscard]] inline bool block_input_pin(const Netlist &nl, std::size_t ordinal, Slot &slot_out, std::uint8_t &input_out) noexcept {
  std::size_t cur = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_block_input(nl, s, i)) {
        continue;
      }
      if (cur == ordinal) {
        slot_out = s;
        input_out = i;
        return true;
      }
      ++cur;
    }
  }
  return false;
}

/**
 * @brief Block-input ordinal for one pin's tie class leader.
 *
 * kNoDriver marks wired, ignored, dead, or otherwise non-block-input
 * pins. Lets word-eval build its column map without touching tie_rep.
 *
 * @param[in] nl Netlist holding the gate and tie table.
 * @param[in] slot Gate slot.
 * @param[in] input 0-based input index.
 * @return Leader ordinal in slot order, or kNoDriver when the pin is
 *         not a block input.
 */
[[nodiscard]] inline Slot input_index(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (!detail::is_block_input(nl, slot, input)) {
    return kNoDriver;
  }
  // Leaders only (predicate): the pin is its own class canonical rep.
  const std::size_t want = tie_flat(slot, input);
  Slot ordinal = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &o = nl.slots[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      if (!detail::is_block_input(nl, s, i)) {
        continue;
      }
      if (tie_flat(s, i) == want) {
        return ordinal;
      }
      ++ordinal;
    }
  }
  return kNoDriver;
}

/**
 * @brief Single shared dangling-input resolution prelude (was
 *        duplicated in eval::solve, seq_step composition, and the CLI
 *        composer).
 *
 * Leaders consume block_inputs in slot order, X/Z rejected; ignored
 * pins tie to kLow; tied members fan out from their leader. The caller
 * zeroes the full resolved[] first. Sizes must match
 * block_input_count exactly.
 *
 * @param[in] nl Netlist to resolve against.
 * @param[in] block_inputs Block input values in derived interface order.
 * @param[out] resolved Caller-owned span (kCap*kMaxInputs values):
 *             per-(slot,input) values (leaders filled, tied members
 *             fanned out, ignored tied to kLow).
 * @return kOk on success; kInvalidArgument on X/Z inputs, a size
 *         mismatch, or an undersized scratch span.
 */
[[nodiscard]] inline core::Status fill_resolved(const Netlist &nl, std::span<const LogicValue> block_inputs, std::span<LogicValue> resolved) noexcept {
  if (resolved.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (std::size_t i = 0; i < slot_count(nl) * kMaxInputs; ++i) {
    resolved[i] = LogicValue::kLow;
  }
  std::size_t cursor = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue; // wired pins read gate_out, never resolved[]
      }
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + static_cast<std::size_t>(i);
      if (!detail::is_block_input(nl, s, i)) {
        if ((g.ignored_mask & (1u << i)) != 0) {
          resolved[flat] = LogicValue::kLow; // ignored dangles tie off
        } else if (is_pulled_flat(nl, flat)) {
          // Terminated (012): pulled dangling inputs resolve to idle
          // without consuming a block-input column (pull + tie/ignore
          // are disjoint, so no interplay hides here).
          resolved[flat] = (pull_level_flat(nl, flat) == 1) ? LogicValue::kHigh : LogicValue::kLow;
        }
        continue; // non-leader members share the leader's value below
      }
      if (cursor >= block_inputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      const LogicValue v = block_inputs[cursor++];
      if (v != LogicValue::kLow && v != LogicValue::kHigh) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      resolved[flat] = v;
    }
  }
  if (cursor != block_inputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::is_dangling_input(nl, s, i)) {
        continue;
      }
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + static_cast<std::size_t>(i);
      const std::size_t leader = tie_leader_flat(nl, flat);
      if (leader != flat) {
        resolved[flat] = resolved[leader];
      }
    }
  }
  return core::Status::success();
}

/**
 * @brief Single-input scalar resolution: wired inputs read gate_out
 *        (old-Q phase, new-Q phase, or combinational settle); dangling
 *        inputs read the pre-resolved block vector (leaders only —
 *        caller fans ties out first); ignored dangling pins tie to kLow.
 *
 * The one funnel every scalar evaluator (solve, stepper phases, async and level apply) resolves through.
 * ScalarResolver adapter over the InputResolution seam: span assembly,
 * polarity, pull-map, and contention detection come from
 * `input_res`; the loud failure vocabulary (dead/X/Z refuse with
 * `false`) stays here.
 *
 * @param[in] nl Netlist holding drivers and ignore marks.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[in] gate_out Per-slot settled gate outputs.
 * @param[in] resolved Pre-resolved dangling values from fill_resolved.
 * @param[out] out Resolved input value.
 * @param[in] diag Optional contention channel (nullptr = no counting; all existing callers behave identically).
 * @return True on success; false on X/Z values or on a resolve_net refusal (wired X/Z driver) — loud, never silent.
 */
[[nodiscard]] inline bool resolve_input(const Netlist &nl, Slot slot, std::uint8_t input,
                                        const LogicValue *gate_out, const LogicValue *resolved,
                                        LogicValue &out, SolveDiag *diag = nullptr) noexcept {
  if (diag != nullptr && diag->per_net.size() < slot_count(nl) * kMaxInputs) {
    return false; // undersized diagnostic channel: loud, never silent
  }
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return false;
  }
  input_res::InputSpan span{};
  input_res::assemble_input_span(nl, slot, input, span);
  if (span.is_shared) {
    // Shared-net span (011): every attachment contributes its present
    // value with per-attachment polarity, assembled once by the seam
    // primary-first in compact index order, folded through the unchanged
    // single funnel. Agreement resolves to the level; low-vs-high
    // resolves kUnknown; no attachments resolve kHighZ. Non-level results
    // fail loudly — the shipped consumer contract (state and timing
    // untouched). Marked inputs never take the dangling/block path below,
    // even when empty: an empty span is kHighZ by contract, never a poked
    // block value.
    if (span.has_dead) {
      return false; // dangling extra / dead driver: loud, never partial
    }
    LogicValue vals[1 + kMaxSharedExtra];
    for (std::size_t k = 0; k < span.n; ++k) {
      vals[k] = input_res::apply_span_polarity(gate_out[span.drv[k]], span.inv[k]);
    }
    LogicValue sunk = LogicValue::kLow;
    if (!resolve_net(vals, span.n, sunk)) {
      return false;
    }
    // (011) Contention counting (SolveDiag): owned once by the seam —
    // a span presenting both polarized low and high increments
    // per_net[flat] by exactly one, computed at span assembly,
    // independent of the funnel's X logic.
    if (diag != nullptr && input_res::span_has_contention(vals, span.n)) {
      ++diag->per_net[span.flat];
    }
    // (012) Pull map: owned once by the seam — an all-floating span
    // resolves through the destination pull to idle; driven spans are
    // inert under every pull setting.
    sunk = input_res::pull_map_idle(nl, span.flat, sunk);
    if (sunk != LogicValue::kLow && sunk != LogicValue::kHigh) {
      return false;
    }
    out = sunk;
    return true;
  }
  if (span.n == 1) {
    // Single-driver wired input: polarity folds before the funnel (the
    // seam order — identical to post-funnel inversion on Low/High, and
    // 4-state-preserving on X/Z where the old post-funnel fold drifted).
    if (span.has_dead) {
      return false;
    }
    const LogicValue pv = input_res::apply_span_polarity(gate_out[span.drv[0]], span.inv[0]);
    LogicValue sunk = LogicValue::kLow;
    if (!resolve_net(&pv, 1, sunk)) {
      return false;
    }
    // (012) Loud level check: a non-level sunk value is reachable only
    // via Z-producing drivers (disabled buffers). Z maps through the
    // seam pull-map to idle; unpulled Z (and anything else non-level)
    // fails loudly instead of flowing into eval_gate.
    sunk = input_res::pull_map_idle(nl, span.flat, sunk);
    if (sunk != LogicValue::kLow && sunk != LogicValue::kHigh) {
      return false;
    }
    out = sunk;
    return true;
  }
  if (input_res::input_is_ignored(nl, slot, input)) {
    out = LogicValue::kLow;
    return true;
  }
  const LogicValue v =
      resolved[static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input)];
  if (v != LogicValue::kLow && v != LogicValue::kHigh) {
    return false;
  }
  out = v;
  return true;
}

// Bit-parallel word geometry. Row convention (shared with scalar `solve`
// and the `truth` verb): truth row r drives block input j with bit
// (r>>j)&1, where j is the derived interface order. Word w covers rows
// [w*64, w*64+63]; lane b holds row w*64+b as bit b.
/** @brief Bit-parallel word size: one word covers 64 truth rows. */
inline constexpr std::size_t kBitWordSize = 64;

/**
 * @brief Slices per batched word-eval pass (017): one level-order
 *        traversal yields kWordBatchWidth consecutive slice words per
 *        gate. Width lives in types.hpp (pool-shape vocabulary beside
 *        kMaxInputs); this seam only derives the batch scratch size.
 *
 * One AVX2 register of 64-bit lanes on x86-64; plain scalar batch loop
 * the optimizer auto-vectorizes at -O3 (no intrinsics, no target
 * attributes). Single-slice tail covers non-multiple counts.
 */

/**
 * @brief Batched gate scratch words for n slots: n * kWordBatchWidth.
 *
 * Slot-major layout ([slot][lane]) shared with eval_block_words.
 *
 * @param[in] n_slots Slot count the scratch must cover.
 * @return Word count for one full batch over n_slots.
 */
[[nodiscard]] constexpr std::size_t batch_words(std::size_t n_slots) noexcept {
  return n_slots * kWordBatchWidth;
}

static_assert(batch_words(0) == 0, "empty pool needs no batch scratch");
static_assert(batch_words(1) == kWordBatchWidth, "one slot takes one batch row");
static_assert(batch_words(3) == 3 * kWordBatchWidth, "batch scratch scales per slot");

/**
 * @brief Full-truth word count for n block inputs: ceil(2^n / 64).
 *
 * @param[in] n_inputs Block input count.
 * @return Word count; 0 when n_inputs >= 31 (2^n rows exceed sane
 *         streaming: slice eval stays valid, full enumeration is the
 *         caller's refusal, like `truth --limit`).
 */
[[nodiscard]] constexpr std::size_t bit_words_for_inputs(std::size_t n_inputs) noexcept {
  if (n_inputs >= 31) {
    return 0;
  }
  const auto rows = static_cast<std::size_t>(1ULL << n_inputs);
  return (rows + (kBitWordSize - 1)) / kBitWordSize;
}

/**
 * @brief Procedural input column for block input input_pos over word
 *        word_index.
 *
 * Row convention (shared with scalar solve and the `truth` verb):
 * truth row r drives block input j with bit (r>>j)&1, where j is the
 * derived interface order. Word w covers rows [w*64, w*64+63]; lane b
 * holds row w*64+b as bit b. For positions < 6 the 64-row tile is
 * word-independent (base w*64 is a multiple of 2^(pos+1)); for
 * positions >= 6 the whole word is constant (2^pos rows per bit value
 * covers the tile).
 *
 * @param[in] input_pos Block-input ordinal.
 * @param[in] word_index 64-row slice index.
 * @return 64-bit column word for this input over the slice.
 */
[[nodiscard]] constexpr std::uint64_t input_column_word(std::size_t input_pos, std::uint64_t word_index) noexcept {
  if (input_pos == 0) {
    return 0xAAAAAAAAAAAAAAAAULL;
  }
  if (input_pos == 1) {
    return 0xCCCCCCCCCCCCCCCCULL;
  }
  if (input_pos == 2) {
    return 0xF0F0F0F0F0F0F0F0ULL;
  }
  if (input_pos == 3) {
    return 0xFF00FF00FF00FF00ULL;
  }
  if (input_pos == 4) {
    return 0xFFFF0000FFFF0000ULL;
  }
  if (input_pos == 5) {
    return 0xFFFFFFFF00000000ULL;
  }
  const auto shift = static_cast<unsigned>(input_pos - 6);
  const std::uint64_t bit = (shift >= 64) ? 0ULL : ((word_index >> shift) & 1ULL);
  return (bit != 0ULL) ? ~static_cast<std::uint64_t>(0ULL) : static_cast<std::uint64_t>(0ULL);
}

/**
 * @brief True when the word-parallel path cannot evaluate this netlist (011): any shared input holds >= 2 attachments.
 *
 * Single-bit words cannot carry the four-value funnel (X has no
 * representation); marked inputs with <= 1 attachment ride the primary
 * word path unchanged. Live-gate scan only — unmarked netlists cost one
 * byte test per configured input.
 *
 * @param[in] nl Netlist to inspect.
 * @return True when a shared input holds 2+ attachments.
 */
[[nodiscard]] inline bool shared_word_blocked(const Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + i;
      if (is_shared_flat(nl, flat) && shared_extra_flat(nl, flat) != 0) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief True when the netlist holds any shared mark (011).
 *
 * Conservative whole-net predicate for passes that cannot prove safety
 * for shared nets (exact_simplify, O2): any input marked, regardless
 * of attachment count.
 *
 * @param[in] nl Netlist to inspect.
 * @return True when any live gate input carries the mark.
 */
[[nodiscard]] inline bool any_shared_mark(const Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (is_shared_flat(nl, static_cast<std::size_t>(s) * kMaxInputs + i)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief True when the netlist holds any live enable-gated buffer gate
 *        (012): the word-path entry guards (truth/export) and the O2
 *        refusal key on this.
 *
 * Live-gate scan only — mirrors shared_word_blocked.
 *
 * @param[in] nl Netlist to inspect.
 * @return True when any live gate is kind kTbuf.
 */
[[nodiscard]] inline bool has_buffer_gates(const Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (nl.slots[s].live && nl.slots[s].kind == GateKind::kTbuf) {
      return true;
    }
  }
  return false;
}

/**
 * @brief True when the netlist holds any pull setting (012):
 *        persistent topology state beside any_shared_mark.
 *
 * Conservative whole-net predicate for passes that cannot prove safety
 * for pulled nets (exact_simplify, O2): any live gate input pulled,
 * regardless of wiring.
 *
 * @param[in] nl Netlist to inspect.
 * @return True when any live gate input carries a pull.
 */
[[nodiscard]] inline bool any_pull(const Netlist &nl) noexcept {
  for (Slot s = 0; s < slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (is_pulled_flat(nl, static_cast<std::size_t>(s) * kMaxInputs + i)) {
        return true;
      }
    }
  }
  return false;
}

/**
 * @brief Bit-parallel twin of resolve_input: wired inputs read the
 *        source word (with NQ polarity); ignored dangling pins read
 *        word 0 (same rule as scalar kLow); dangling block inputs read
 *        their procedural interface column.
 *
 * WordResolver adapter over the InputResolution seam: span assembly and
 * word polarity come from `input_res`; the single-bit failure
 * vocabulary stays here (multi-attachment shared spans and live TBUF
 * nets are refused at the word-loop entries via `shared_word_blocked` /
 * `has_buffer_gates`, never inside the lane fold — the per-word safety
 * nets below are unreachable and cost nothing).
 *
 * in_pos maps (slot, input) to the block-input ordinal (kNoDriver for
 * wired/ignored pins — never read on those paths; tied members carry
 * their leader's ordinal).
 *
 * @param[in] nl Netlist holding drivers, ignore marks, and NQ polarity.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[in] gate_scratch Per-slot words from the compiled level order.
 * @param[in] in_pos (slot,input)->ordinal map from build_input_positions.
 * @param[in] word_index 64-row slice index.
 * @return 64-bit resolved word for this input over the slice.
 */
[[nodiscard]] inline std::uint64_t resolve_word(const Netlist &nl, Slot slot, std::uint8_t input,
                                                const std::uint64_t *gate_scratch, const Slot *in_pos,
                                                std::uint64_t word_index) noexcept {
  input_res::InputSpan span{};
  input_res::assemble_input_span(nl, slot, input, span);
  if (span.n == 1 && !span.has_dead) {
    // Wired single (or a single-attachment shared mark riding the
    // primary word path unchanged): one source word with NQ polarity.
    // Bounds hold by construction (topology pre-validated drivers; the
    // dead guard above keeps the read in-bounds on defensive paths).
    if (span.drv[0] >= slot_count(nl)) {
      return 0;
    }
    return input_res::apply_word_polarity(gate_scratch[span.drv[0]], span.inv[0]);
  }
  if (span.n > 1) {
    // Multi-attachment shared span: no 1-bit form. Word-loop entries
    // refuse via shared_word_blocked before looping; this arm is the
    // per-word safety net for future callers (unreachable, zero
    // per-word cost).
    return 0;
  }
  if (span.has_dead) {
    // Dead driver on a topology the entries already refused: stay
    // in-bounds and neutral rather than reading out-of-range.
    return 0;
  }
  if (input_res::input_is_ignored(nl, slot, input)) {
    return 0;
  }
  // (012) Terminated (pull-only) dangling inputs never reach the
  // procedural column map (is_block_input excludes them): up reads the
  // all-ones word, down the all-zero word — the word twin of the idle
  // rule in fill_resolved. Pull-only netlists stream truth unchanged.
  // Shared-empty pulled nets land here too (same idle word the scalar
  // pull-map produces); shared-empty unpulled is floating (scalar fails
  // loudly there — excluded from the 0/1 property, rides the dangling
  // column today).
  const std::size_t flat = tie_flat(slot, input);
  if (is_pulled_flat(nl, flat)) {
    return input_res::pull_idle_word(nl, flat);
  }
  const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(slot) * kMaxInputs + input]);
  return input_column_word(p, word_index);
}

} // namespace wengine::logic
