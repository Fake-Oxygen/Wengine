#pragma once

// Combinational topology + scalar solve (LOGIC-2).
//
// Topology compiles once per mutation (dirty flag): Kahn's algorithm over
// slot order -> deterministic level order, cycle check included.
// Steady-state `solve` never re-sorts and never allocates: two fixed
// stack arrays (resolved dangling inputs + per-gate outputs), caller-owned
// spans for block I/O. Bit-parallel word eval arrives in LOGIC-3 against
// the same level order.
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
// input index); block_outputs order == interface output order (slot
// order). Sizes must match exactly or the call fails loudly (dangling
// pins minus ignored). Ignored dangling inputs tie to kLow. Any kHighZ /
// kUnknown block input fails loudly. Repeated calls with equal inputs
// are bit-identical. No heap use (fixed stack arrays only).
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
      if (cursor >= block_inputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      const LogicValue v = block_inputs[cursor++];
      if (v != LogicValue::kLow && v != LogicValue::kHigh) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      resolved[static_cast<std::size_t>(s) * kMaxInputs + i] = v;
    }
  }
  if (cursor != block_inputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
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

} // namespace wengine::logic
