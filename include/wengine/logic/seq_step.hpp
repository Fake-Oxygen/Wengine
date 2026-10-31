#pragma once

// Scalar single-step sequential eval (LOGIC-V2-SEQ4).
//
// Zero-delay single-clock-edge stepper for DFF/DLATCH with deterministic
// end-of-step evaluation. One call advances every flop by one rising edge
// (global clock — CLK/EN pins are validated Low/High but do not gate; the
// edge itself is the call, same as the signals two-phase convention where
// generators evaluate at time T and sums commit at T+1).
//
// Two-phase (mirrors signals: generators then sums):
//   Phase 1: combinational eval at time T with OLD Qs + block inputs.
//   Snapshot: every flop's D (input 0) resolved from phase 1, in slot order.
//   Commit: all Qs updated from snapshots, in slot order (parallel update —
//     no ordering dependency, swaps and counters behave synchronously).
//   Phase 2: combinational re-eval with NEW Qs for block outputs (zero-delay
//     settling, so outputs are end-of-step values at T+1).
//
// Rules (SEQ4):
// - Rising-edge only. CLK (DFF input 1) / EN (DLATCH input 1) must resolve
//   to kLow/kHigh; HighZ/Unknown fail loudly. The value does not gate the
//   update in SEQ4 — level-sensitive DLATCH semantics arrive in SEQ8, which
//   refines this same function.
// - DFF and DLATCH both capture D on the edge in SEQ4 (documented
//   simplification; SEQ8 gives DLATCH transparent-when-EN behavior).
// - X/Z rejected loudly: block inputs must be kLow/kHigh (same rule as
//   solve()), flop Qs are always kLow/kHigh by construction (power-on kLow,
//   commits only write snapshots resolved to kLow/kHigh); any other value
//   fails without committing.
// - Determinism: slot-order snapshot + slot-order commit; sync via
//   init_seq_state is idempotent, so calling seq_step every step (or
//   sporadically) keeps state. No allocation, no exceptions, noexcept.
// - solve()/eval_block_word() stay combinational-only (untouched by SEQ4):
//   do not call them on nets with sequential gates — ensure_topology sees
//   ALL edges and fails loudly on flop feedback loops by design; use
//   validate_sequential() + seq_step() instead.
//
// Header-only, allocation-free, exception-free, noexcept throughout.
// Fixed stack arrays (kMaxGates bounds), no syscalls, no iostream.

#include "wengine/core/status.hpp"
#include "wengine/logic/eval.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

namespace detail {

// Combinational-only level order for the stepper (SEQ4): Kahn over
// combinational edges only, same determinism as ensure_topology (slot-order
// seeds, slot-order successors). Sequential gates act as roots (their Q is
// the source value) and never appear in the order. Returns false on any
// combinational cycle or dangling-driver defect (validate_sequential covers
// the cycle case first; this stays defensive so stepping never silently
// evaluates a partial order).
[[nodiscard]] inline bool compile_comb_order(const Netlist &nl, std::uint16_t *order_out,
                                            std::size_t &n_out) noexcept {
  std::uint8_t indeg[kMaxGates] = {};
  std::size_t n_comb = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || is_sequential_kind(g.kind)) {
      continue;
    }
    ++n_comb;
    std::uint8_t d = 0;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      if (drv >= kMaxGates || !nl.gates[drv].live) {
        return false; // dangling driver defect: loud, never partial
      }
      if (!is_sequential_kind(nl.gates[drv].kind)) {
        ++d;
      }
    }
    indeg[s] = d;
  }
  std::uint16_t queue[kMaxGates];
  std::size_t head = 0;
  std::size_t tail = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    if (is_sequential_kind(g.kind)) {
      queue[tail++] = s; // roots: Q sources, never emitted
    } else if (indeg[s] == 0) {
      queue[tail++] = s;
    }
  }
  std::size_t n_emit = 0;
  while (head < tail) {
    const std::uint16_t cur = queue[head++];
    if (!is_sequential_kind(nl.gates[cur].kind)) {
      order_out[n_emit++] = cur;
    }
    for (std::uint16_t s = 0; s < kMaxGates; ++s) {
      const Gate &g = nl.gates[s];
      if (!g.live || is_sequential_kind(g.kind) || indeg[s] == 0) {
        continue;
      }
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        if (g.drivers[i] == cur) {
          --indeg[s];
          if (indeg[s] == 0) {
            queue[tail++] = s;
            break; // one push per gate (remaining inputs counted already)
          }
        }
      }
    }
  }
  n_out = n_emit;
  return n_emit == n_comb;
}

// Resolve one gate input to a Low/High value. Wired inputs read gate_out
// (old-Q phase or new-Q phase); dangling inputs read the pre-resolved
// block vector (leaders only — caller fans ties out first); ignored
// dangling pins tie to kLow (netlist rule). Returns false on X/Z or on a
// resolve_net refusal (wired X/Z driver) — loud, never silent.
[[nodiscard]] inline bool resolve_step_input(const Netlist &nl, std::uint16_t slot, std::uint8_t input,
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
    out = sunk;
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

} // namespace detail

// Single rising edge over the whole netlist (global clock). See header note
// for the two-phase contract. On failure the SeqState is untouched (snapshot
// validates fully before any commit) and block_outputs are unspecified;
// the Netlist topology fields are never mutated.
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st, std::span<const LogicValue> block_inputs,
                                          std::span<LogicValue> block_outputs) noexcept {
  init_seq_state(st, nl);
  if (!validate_sequential(nl).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t n_in = block_input_count(nl);
  const std::size_t n_out = block_output_count(nl);
  if (block_inputs.size() != n_in || block_outputs.size() != n_out) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Defensive: Qs are Low/High by construction (power-on Low, commits write
  // snapshots only). A corrupted pool (or a future tri-state Q) fails loudly
  // before any eval reads it.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (st.valid[s] != 0 && st.q[s] != LogicValue::kLow && st.q[s] != LogicValue::kHigh) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  // Dangling-input resolution (same order/rules as solve): leaders consume
  // block_inputs in slot order, X/Z rejected; ignored pins tie to kLow.
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

  std::uint16_t comb_order[kMaxGates];
  std::size_t n_comb = 0;
  if (!detail::compile_comb_order(nl, comb_order, n_comb)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  // Phase 1: combinational eval at time T with OLD Qs.
  LogicValue gate_out[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    gate_out[s] = LogicValue::kLow;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && is_sequential_kind(nl.gates[s].kind)) {
      gate_out[s] = st.q[s];
    }
  }
  LogicValue ins[kMaxInputs];
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const std::uint16_t s = comb_order[oi];
    const Gate &g = nl.gates[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::resolve_step_input(nl, s, i, gate_out, resolved, ins[i])) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }

  // Snapshot: D (input 0) for every flop + CLK/EN (input 1) validation, in
  // slot order. X/Z anywhere fails before any commit (state untouched).
  LogicValue next_q[kMaxGates];
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    next_q[s] = LogicValue::kLow;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live || !is_sequential_kind(g.kind)) {
      continue;
    }
    LogicValue d = LogicValue::kLow;
    LogicValue clk = LogicValue::kLow;
    if (!detail::resolve_step_input(nl, s, 0, gate_out, resolved, d)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if (!detail::resolve_step_input(nl, s, 1, gate_out, resolved, clk)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    if ((d != LogicValue::kLow && d != LogicValue::kHigh) ||
        (clk != LogicValue::kLow && clk != LogicValue::kHigh)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    // SEQ4: edge capture for both kinds (DLATCH level semantics in SEQ8).
    // CLK/EN validated above; the edge itself is the call.
    next_q[s] = d;
  }

  // Commit: parallel Q update in slot order.
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && is_sequential_kind(nl.gates[s].kind)) {
      st.q[s] = next_q[s];
      gate_out[s] = next_q[s]; // seed phase 2 with NEW Qs
    }
  }

  // Phase 2: zero-delay re-settle with NEW Qs for end-of-step outputs.
  for (std::size_t oi = 0; oi < n_comb; ++oi) {
    const std::uint16_t s = comb_order[oi];
    const Gate &g = nl.gates[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!detail::resolve_step_input(nl, s, i, gate_out, resolved, ins[i])) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
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

// Convenience: step with block outputs discarded (state observed via
// SeqState.q / find_seq_serial). Computes the same end-of-step values;
// only the caller-visible copy is dropped.
[[nodiscard]] inline core::Status seq_step(Netlist &nl, SeqState &st,
                                          std::span<const LogicValue> block_inputs) noexcept {
  const std::size_t n_out = block_output_count(nl);
  LogicValue scratch[kMaxGates];
  for (std::size_t i = 0; i < kMaxGates; ++i) {
    scratch[i] = LogicValue::kLow;
  }
  std::span<LogicValue> out_span(scratch, n_out);
  return seq_step(nl, st, block_inputs, out_span);
}

} // namespace wengine::logic
