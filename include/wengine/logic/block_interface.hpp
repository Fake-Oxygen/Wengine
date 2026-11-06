#pragma once

// BlockInterface seam (see GLOSSARY.md): derived block boundary, never
// stored. One block input per tie class (canonical leader in slot order)
// plus dangling outputs in slot order, minus ignored pins.
//
// Only this header (beside tie_set.hpp for raw tie_rep) derives interface
// order. Scalar `solve`, bit-parallel `eval_block_word`, the sequential
// stepper, and the CLI poke composer are adapters: they enumerate via
// `block_input_pin`/`input_index` and resolve via `fill_resolved`.
//
// Header-only, allocation-free, exception-free. Caller-owned buffers
// throughout (same contract as `block_interface`).

#include "wengine/core/status.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

// Derived block interface (never stored): one input per tie class
// (canonical leader in slot order) plus dangling outputs in slot order.
// Fixed caller buffers, loud kOutOfRange when too small.
[[nodiscard]] inline std::size_t block_input_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (tie_leader_flat(nl, flat) != flat) {
        continue; // non-leader members share the leader's column
      }
      ++n;
    }
  }
  return n;
}

[[nodiscard]] inline std::size_t block_output_count(const Netlist &nl) noexcept {
  std::size_t n = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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

[[nodiscard]] inline core::Status block_interface(const Netlist &nl, PinRef *in_buf, std::size_t in_cap,
                                                  std::size_t *in_n, PinRef *out_buf,
                                                  std::size_t out_cap, std::size_t *out_n) noexcept {
  std::size_t need_in = 0;
  std::size_t need_out = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (tie_leader_flat(nl, flat) != flat) {
        continue;
      }
      if (in_buf != nullptr && need_in < in_cap) {
        in_buf[need_in] = PinRef{.slot = s, .is_output = false, .input = i};
      }
      ++need_in;
    }
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
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

// Leader pin for one block-input ordinal (slot order, leaders only).
// Returns false for out-of-range ordinals.
[[nodiscard]] inline bool block_input_pin(const Netlist &nl, std::size_t ordinal, std::uint16_t &slot_out,
                                          std::uint8_t &input_out) noexcept {
  std::size_t cur = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (tie_leader_flat(nl, flat) != flat) {
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

// Block-input ordinal for one pin's tie class leader; kNoDriver when the
// pin is wired, ignored, dead, or otherwise not a block input. Lets
// word-eval build its column map without touching tie_rep.
[[nodiscard]] inline std::uint16_t input_index(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || !nl.gates[slot].live) {
    return kNoDriver;
  }
  const Gate &g = nl.gates[slot];
  if (input >= g.input_count || g.drivers[input] != kNoDriver) {
    return kNoDriver;
  }
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    return kNoDriver;
  }
  const std::size_t want = tie_leader_flat(nl, tie_flat(slot, input));
  std::uint16_t ordinal = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &o = nl.gates[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      if (o.drivers[i] != kNoDriver) {
        continue;
      }
      if ((o.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (tie_leader_flat(nl, flat) != flat) {
        continue;
      }
      if (flat == want) {
        return ordinal;
      }
      ++ordinal;
    }
  }
  return kNoDriver;
}

// Single shared dangling-input resolution prelude (was duplicated in
// eval::solve, seq_step::compose_resolved, and the CLI composer):
// leaders consume block_inputs in slot order, X/Z rejected; ignored pins
// tie to kLow; tied members fan out from their leader. Callee zeroes the
// full resolved[] first. Sizes must match block_input_count exactly.
[[nodiscard]] inline core::Status fill_resolved(const Netlist &nl, std::span<const LogicValue> block_inputs, LogicValue *resolved) noexcept {
  for (std::size_t i = 0; i < kMaxGates * kMaxInputs; ++i) {
    resolved[i] = LogicValue::kLow;
  }
  std::size_t cursor = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
        resolved[static_cast<std::size_t>(s) * kMaxInputs + i] = LogicValue::kLow;
        continue;
      }
      const std::size_t flat = static_cast<std::size_t>(s) * kMaxInputs + static_cast<std::size_t>(i);
      if (tie_leader_flat(nl, flat) != flat) {
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
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & (1u << i)) != 0) {
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

// Single-input resolution (scalar): wired inputs read gate_out (old-Q
// phase, new-Q phase, or combinational settle); dangling inputs read the
// pre-resolved block vector (leaders only — caller fans ties out first);
// ignored dangling pins tie to kLow (netlist rule). Returns false on X/Z
// or on a resolve_net refusal (wired X/Z driver) — loud, never silent.
// The one funnel every scalar evaluator (solve, stepper phases, async and
// level apply) resolves through.
[[nodiscard]] inline bool resolve_input(const Netlist &nl, std::uint16_t slot, std::uint8_t input,
                                        const LogicValue *gate_out, const LogicValue *resolved,
                                        LogicValue &out) noexcept {
  const Gate &g = nl.gates[slot];
  const std::uint16_t drv = g.drivers[input];
  if (drv != kNoDriver) {
    if (drv >= kMaxGates) {
      return false;
    }
    LogicValue sunk = LogicValue::kLow;
    if (!resolve_net(&gate_out[drv], 1, sunk)) {
      return false;
    }
    // NQ source: the sink reads the negated value (T005).
    out = apply_polarity(sunk, nl.driver_inv[static_cast<std::size_t>(slot) * kMaxInputs + input]);
    return true;
  }
  if ((g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
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
inline constexpr std::size_t kBitWordSize = 64;
inline constexpr std::size_t kBitScratchWords = kMaxGates;

// Full-truth word count for n block inputs: ceil(2^n / 64). Returns 0
// when n >= 31 (2^n rows exceed sane streaming; slice eval stays valid,
// full enumeration is the caller's refusal, like `truth --limit`).
[[nodiscard]] constexpr std::size_t bit_words_for_inputs(std::size_t n_inputs) noexcept {
  if (n_inputs >= 31) {
    return 0;
  }
  const auto rows = static_cast<std::size_t>(1ULL << n_inputs);
  return (rows + (kBitWordSize - 1)) / kBitWordSize;
}

// Procedural input column for block input `input_pos` over word
// `word_index`. For positions < 6 the 64-row tile is word-independent
// (base w*64 is a multiple of 2^(pos+1)); for positions >= 6 the whole
// word is constant (2^pos rows per bit value covers the tile).
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

// Bit-parallel twin of resolve_input: wired inputs read the source word
// (with NQ polarity, T005); ignored dangling pins read word 0 (same rule
// as scalar kLow); dangling block inputs read their procedural interface
// column. `in_pos` maps (slot, input) to the block-input ordinal
// (kNoDriver for wired/ignored pins — never read on those paths).
[[nodiscard]] inline std::uint64_t resolve_word(const Netlist &nl, std::uint16_t slot, std::uint8_t input,
                                                const std::uint64_t *gate_scratch,
                                                const std::uint16_t *in_pos,
                                                std::uint64_t word_index) noexcept {
  const std::uint16_t drv = nl.gates[slot].drivers[input];
  if (drv != kNoDriver) {
    const std::uint64_t v = gate_scratch[drv];
    if (nl.driver_inv[static_cast<std::size_t>(slot) * kMaxInputs + input] != 0) {
      return ~v;
    }
    return v;
  }
  if ((nl.gates[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    return 0;
  }
  const std::size_t p =
      static_cast<std::size_t>(in_pos[static_cast<std::size_t>(slot) * kMaxInputs + input]);
  return input_column_word(p, word_index);
}

} // namespace wengine::logic
