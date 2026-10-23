#pragma once

// Combinational topology + scalar solve + bit-parallel word eval (LOGIC-2/3).
//
// Topology compiles once per mutation (dirty flag): Kahn's algorithm over
// slot order -> deterministic level order, cycle check included.
// Steady-state `solve` never re-sorts and never allocates: two fixed
// stack arrays (resolved dangling inputs + per-gate outputs), caller-owned
// spans for block I/O. Bit-parallel word eval runs against the same level
// order (see below).
//
// Gate semantics (v1, zero-delay):
//   AND/OR/NAND/NOR over 2..8 inputs; NOT/BUF exactly 1; XOR is odd parity
//   over 2..8 (XNOR = NOT(XOR), i.e. even parity). 74-series cascade
//   equivalence holds only for 2 inputs -- the usage text must say so.
//   Only kLow/kHigh flow; kHighZ/kUnknown inputs fail loudly (V2 slot).

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

// Pure gate function. Precondition: every input is kLow/kHigh and n
// matches valid_fanin(kind, n) (both enforced by solve()).
[[nodiscard]] constexpr LogicValue eval_gate(GateKind kind, const LogicValue *inputs, std::uint8_t n) noexcept {
  auto is_high = [](LogicValue v) noexcept { return v == LogicValue::kHigh; };
  switch (kind) {
  case GateKind::kAnd:
  case GateKind::kNand: {
    bool all = true;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (!is_high(inputs[i])) {
        all = false;
        break;
      }
    }
    const bool y = (kind == GateKind::kAnd) ? all : !all;
    return y ? LogicValue::kHigh : LogicValue::kLow;
  }
  case GateKind::kOr:
  case GateKind::kNor: {
    bool any = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (is_high(inputs[i])) {
        any = true;
        break;
      }
    }
    const bool y = (kind == GateKind::kOr) ? any : !any;
    return y ? LogicValue::kHigh : LogicValue::kLow;
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    bool odd = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (is_high(inputs[i])) {
        odd = !odd;
      }
    }
    const bool y = (kind == GateKind::kXor) ? odd : !odd;
    return y ? LogicValue::kHigh : LogicValue::kLow;
  }
  case GateKind::kNot:
    return is_high(inputs[0]) ? LogicValue::kLow : LogicValue::kHigh;
  case GateKind::kBuf:
    return is_high(inputs[0]) ? LogicValue::kHigh : LogicValue::kLow;
  }
  return LogicValue::kLow;
}

// Compile (or reuse) the Kahn level order. Deterministic: zero-indegree
// seeds scan in slot order; successors of each popped gate scan in slot
// order; FIFO queue. Levels: 0 for sources, else 1 + max(driver levels).
// No allocation (fixed stack indegree + queue). Keeps dirty on failure.
[[nodiscard]] inline core::Status ensure_topology(Netlist &nl) noexcept {
  if (!nl.dirty && nl.topo_size == nl.live_count) {
    return core::Status::success();
  }
  std::uint8_t indeg[kMaxGates] = {};
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    std::uint8_t d = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        ++d;
      }
    }
    indeg[s] = d;
    nl.topo_level[s] = 0;
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t order_idx = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    // Depth is final now: every driver popped earlier (topo order).
    std::uint16_t depth = 0;
    const Gate &cg = nl.gates[cur];
    for (std::uint8_t i = 0; i < cg.input_count; ++i) {
      const std::uint16_t drv = cg.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      const std::uint16_t cand = static_cast<std::uint16_t>(nl.topo_level[drv] + 1);
      if (cand > depth) {
        depth = cand;
      }
    }
    nl.topo_level[cur] = depth;
    nl.topo_order[order_idx++] = cur;
    // Successors in slot order (deterministic push order).
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      Gate &g = nl.gates[s];
      if (!g.live || indeg[s] == 0) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] != cur) {
          continue;
        }
        --indeg[s];
        if (indeg[s] == 0) {
          queue[tail++] = s;
          break; // one push per gate (remaining inputs from cur already counted)
        }
      }
    }
  }
  if (order_idx != nl.live_count) {
    nl.topo_size = 0;
    nl.dirty = true;
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  nl.topo_size = static_cast<std::uint16_t>(order_idx);
  nl.dirty = false;
  return core::Status::success();
}

// Scalar single-assignment solve over the compiled level order.
//
// block_inputs order == derived interface input order (slot order, then
// input index, one entry per tie class leader); block_outputs order ==
// interface output order (slot order). Sizes must match exactly or the
// call fails loudly (dangling pins minus ignored, classes collapsed).
// Ignored dangling inputs tie to kLow. Tied members share their leader's
// value. Any kHighZ / kUnknown block input fails loudly. Repeated calls
// with equal inputs are bit-identical. No heap use (fixed stack arrays).
[[nodiscard]] inline core::Status solve(Netlist &nl, std::span<const LogicValue> block_inputs, std::span<LogicValue> block_outputs) noexcept {
  core::Status st = ensure_topology(nl);
  if (!st.ok()) {
    return st;
  }
  // Resolved value per (slot, input); only dangling entries are read.
  LogicValue resolved[kMaxGates * kMaxInputs];
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
  // Fan tied member values out from their leaders.
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

  LogicValue gate_out[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    gate_out[s] = LogicValue::kLow;
  }
  LogicValue ins[kMaxInputs];
  for (std::uint16_t oi = 0; oi < nl.topo_size; ++oi) {
    const std::uint16_t s = nl.topo_order[oi];
    const Gate &g = nl.gates[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv != kNoDriver) {
        LogicValue v = gate_out[drv];
        LogicValue sunk = LogicValue::kLow;
        if (!resolve_net(&v, 1, sunk)) {
          return core::Status::failure(core::StatusCode::kInvalidArgument);
        }
        ins[i] = sunk;
      } else if ((g.ignored_mask & (1u << i)) != 0) {
        ins[i] = LogicValue::kLow;
      } else {
        ins[i] = resolved[static_cast<std::size_t>(s) * kMaxInputs + i];
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }

  std::size_t out_cursor = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout != 0 || g.ignore_output) {
      continue;
    }
    if (out_cursor >= block_outputs.size()) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    block_outputs[out_cursor++] = gate_out[s];
  }
  if (out_cursor != block_outputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

// Bit-parallel truth eval (LOGIC-3): 64 assignments per uint64_t word.
//
// Row convention (shared with scalar `solve` and the future `truth`
// verb): truth row r drives block input j with bit (r>>j)&1, where j is
// the derived interface order (slot order, then input index). Word w
// covers rows [w*64, w*64+63]; lane b holds row w*64+b as bit b.
// `input_column_word` materializes one input's word procedurally, so
// slices stay valid for any input count (bench nets exceed full-truth
// enumeration; only slices are ever evaluated).
//
// `eval_block_word` evaluates one 64-row slice over the compiled level
// order: each gate is one branchless word op (&|^|~). Ignored dangling
// inputs tie to word 0 (same rule as scalar kLow). The last partial word
// (rows < 64, i.e. fewer than 6 block inputs) is zero-masked on outputs
// so don't-care lanes never leak. Steady state allocates nothing:
// `gate_scratch` (kMaxGates words) and `block_outputs` (exactly
// block_output_count words) are caller-owned, same pattern as
// `fill_snapshot`. No SIMD intrinsics: scalar words carry the speedup
// and stay portable; vectorize only on a measured hot path.

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

// Evaluate one 64-row slice starting at row word_index*64.
// gate_scratch must hold >= kMaxGates words; block_outputs must hold
// exactly block_output_count(nl) words. Fails loudly on bad sizes,
// out-of-range slices (when n_inputs < 31), or topology errors.
[[nodiscard]] inline core::Status eval_block_word(Netlist &nl, std::uint64_t word_index,
                                                 std::span<std::uint64_t> gate_scratch,
                                                 std::span<std::uint64_t> block_outputs) noexcept {
  core::Status st = ensure_topology(nl);
  if (!st.ok()) {
    return st;
  }
  if (gate_scratch.size() < kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Dangling-input -> block position map (slot order, then input index,
  // one entry per tie class leader: same order scalar `solve` consumes;
  // tied members share their leader's column word). kNoDriver = wired/ignored.
  std::uint16_t in_pos[kMaxGates * kMaxInputs];
  for (std::size_t i = 0; i < kMaxGates * kMaxInputs; ++i) {
    in_pos[i] = kNoDriver;
  }
  std::size_t n_inputs = 0;
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
      if (tie_leader_flat(nl, flat) != flat) {
        continue; // resolved to the leader's column below
      }
      in_pos[flat] = static_cast<std::uint16_t>(n_inputs);
      ++n_inputs;
    }
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
        in_pos[flat] = in_pos[leader];
      }
    }
  }
  std::size_t n_outputs = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout != 0 || g.ignore_output) {
      continue;
    }
    ++n_outputs;
  }
  if (block_outputs.size() != n_outputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (n_inputs < 31) {
    const std::size_t need = bit_words_for_inputs(n_inputs);
    if (word_index >= static_cast<std::uint64_t>(need)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  for (std::uint16_t oi = 0; oi < nl.topo_size; ++oi) {
    const std::uint16_t s = nl.topo_order[oi];
    const Gate &g = nl.gates[s];
    std::uint64_t acc = 0;
    switch (g.kind) {
    case GateKind::kAnd:
    case GateKind::kNand:
      acc = ~static_cast<std::uint64_t>(0ULL);
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        std::uint64_t v = 0;
        const std::uint16_t drv = g.drivers[i];
        if (drv != kNoDriver) {
          v = gate_scratch[drv];
        } else if ((g.ignored_mask & (1u << i)) != 0) {
          v = 0;
        } else {
          const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(s) * kMaxInputs + i]);
          v = input_column_word(p, word_index);
        }
        acc &= v;
      }
      if (g.kind == GateKind::kNand) {
        acc = ~acc;
      }
      break;
    case GateKind::kOr:
    case GateKind::kNor:
      acc = 0;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        std::uint64_t v = 0;
        const std::uint16_t drv = g.drivers[i];
        if (drv != kNoDriver) {
          v = gate_scratch[drv];
        } else if ((g.ignored_mask & (1u << i)) != 0) {
          v = 0;
        } else {
          const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(s) * kMaxInputs + i]);
          v = input_column_word(p, word_index);
        }
        acc |= v;
      }
      if (g.kind == GateKind::kNor) {
        acc = ~acc;
      }
      break;
    case GateKind::kXor:
    case GateKind::kXnor:
      acc = 0;
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        std::uint64_t v = 0;
        const std::uint16_t drv = g.drivers[i];
        if (drv != kNoDriver) {
          v = gate_scratch[drv];
        } else if ((g.ignored_mask & (1u << i)) != 0) {
          v = 0;
        } else {
          const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(s) * kMaxInputs + i]);
          v = input_column_word(p, word_index);
        }
        acc ^= v;
      }
      if (g.kind == GateKind::kXnor) {
        acc = ~acc;
      }
      break;
    case GateKind::kNot: {
      std::uint64_t v = 0;
      const std::uint16_t drv = g.drivers[0];
      if (drv != kNoDriver) {
        v = gate_scratch[drv];
      } else if ((g.ignored_mask & 1u) != 0) {
        v = 0;
      } else {
        const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(s) * kMaxInputs]);
        v = input_column_word(p, word_index);
      }
      acc = ~v;
      break;
    }
    case GateKind::kBuf: {
      const std::uint16_t drv = g.drivers[0];
      if (drv != kNoDriver) {
        acc = gate_scratch[drv];
      } else if ((g.ignored_mask & 1u) != 0) {
        acc = 0;
      } else {
        const std::size_t p = static_cast<std::size_t>(in_pos[static_cast<std::size_t>(s) * kMaxInputs]);
        acc = input_column_word(p, word_index);
      }
      break;
    }
    }
    gate_scratch[s] = acc;
  }

  std::size_t out_cursor = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || g.output_fanout != 0 || g.ignore_output) {
      continue;
    }
    block_outputs[out_cursor++] = gate_scratch[s];
  }
  if (n_inputs < 6) {
    const auto rows = static_cast<std::uint64_t>(1ULL << n_inputs);
    const std::uint64_t mask = (rows >= 64) ? ~static_cast<std::uint64_t>(0ULL) : (static_cast<std::uint64_t>(1ULL << rows) - 1ULL);
    for (std::size_t i = 0; i < n_outputs; ++i) {
      block_outputs[i] &= mask;
    }
  }
  return core::Status::success();
}

} // namespace wengine::logic
