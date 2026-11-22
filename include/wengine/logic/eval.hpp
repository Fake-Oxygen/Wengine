#pragma once

/**
 * @file
 * Combinational evaluation stage (LOGIC-2/LOGIC-3) of the logic
 * pipeline: scalar solve plus bit-parallel word eval over one compiled topology. Topology
 * compiles once per mutation (dirty flag): Kahn's algorithm over slot
 * order yields a deterministic level order with a cycle check included;
 * steady-state solve never re-sorts and grows workspace amortized only
 * (caller-owned spans: resolved dangling inputs plus per-gate outputs;
 * block I/O stays caller-owned). Bit-parallel word eval runs against
 * the same level order.
 *
 * Gate semantics (v1, zero-delay): AND/OR/NAND/NOR over 2..8 inputs;
 * NOT/BUF exactly 1; TBUF exactly 2 (data + active-high enable:
 * enabled drives data, released drives kHighZ — the first legal Z
 * producer in gate_out); XOR is odd parity over 2..8 (XNOR = NOT(XOR),
 * i.e. even parity). 74-series cascade equivalence holds only for 2
 * inputs. Only kLow/kHigh flow; kHighZ/kUnknown inputs fail loudly
 * (V2 tri-state slot, docs/logic.md section 12).
 *
 * Dynamic pool: solve and eval_block_word run over Netlist with
 * pool-owned workspace scratch (resolved/gate_out values, word in_pos
 * map, Kahn order/levels); gate_scratch/block_outputs stay
 * caller-owned with loud arity validation. Every bound is the pool's
 * current size (slot_count), never kMaxGates. eval_gate stays
 * capacity-free.
 *
 * Dependency rules (docs/logic.md sections 4, 5): header-only,
 * allocation-free, exception-free; depends on block_interface.hpp,
 * topo.hpp, types.hpp, and core/status for failure codes. Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: the CLI solve/truth verbs, the optimize exact/QM passes
 * (equivalence checks), and the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace wengine::logic {

/**
 * @brief Pure gate function over Low/High inputs.
 *
 * Capacity-free: kinds and values never scale.
 *
 * @param[in] kind Gate kind to evaluate.
 * @param[in] inputs Per-input Low/High values (n entries).
 * @param[in] n Input count; must match valid_fanin(kind, n).
 * @return Gate output. Sequential kinds never reach here (the
 *         stepper owns them); they fall to kLow.
 * @note Precondition: every input is kLow/kHigh and n matches
 *       valid_fanin(kind, n) (both enforced by solve()).
 */
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
  case GateKind::kTbuf:
    // (012) Enable-gated buffer: asserted enable drives the data
    // level, released enable drives high-Z. Precondition (both Low/
    // High, n == 2) is enforced by resolve_input at the buffer's own
    // input seams — eval_gate never observes X/Z here.
    return is_high(inputs[1]) ? inputs[0] : LogicValue::kHighZ;
  case GateKind::kDff:
  case GateKind::kDlatch:
  case GateKind::kJk:
  case GateKind::kT:
  case GateKind::kSrlatch:
    break; // SEQ4 owns sequential stepping; combinational eval never sees
           // these (creation is guarded until SEQ2). Falls to kLow below.
  }
  return LogicValue::kLow;
}

/**
 * @brief Scalar single-assignment solve over the compiled level order.
 *
 * block_inputs order == derived interface input order (slot order, then
 * input index, one entry per tie-class leader); block_outputs order ==
 * interface output order (slot order). Sizes must match exactly or the
 * call fails loudly (dangling pins minus ignored, classes collapsed).
 * Ignored dangling inputs tie to kLow. Tied members share their
 * leader's value. Any kHighZ/kUnknown block input fails loudly.
 *
 * @param[out] nl Netlist to solve (topology recompiled when dirty).
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs Output values in interface order.
 * @param[in] diag Optional contention channel (nullptr = no counting).
 * @param[in] resolved Caller-owned scratch (pool-size * kMaxInputs values).
 * @param[in] gate_out Caller-owned scratch (pool-size per-slot outputs).
 * @return kOk on success; kInvalidArgument on topology errors
 *         (combinational cycle, dangling driver), size mismatches,
 *         X/Z inputs, X/Z wired drivers, or undersized scratch.
 * @note Determinism: repeated calls with equal inputs are bit-identical
 *       (slot-order level order, slot-order output scan). Workspace
 *       growth is amortized (topology compiles through the CompiledOrder
 *       seam, which owns its scratch).
 */
[[nodiscard]] inline core::Status solve(Netlist &nl, std::span<const LogicValue> block_inputs,
                                        std::span<LogicValue> block_outputs, SolveDiag *diag,
                                        std::span<LogicValue> resolved, std::span<LogicValue> gate_out) {
  core::Status st = topo::compile(nl, EdgeFilter::kAll);
  if (!st.ok()) {
    return st;
  }
  if (resolved.size() < slot_count(nl) * kMaxInputs || gate_out.size() < slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (diag != nullptr && diag->per_net.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Resolved value per (slot, input); only dangling entries are read.
  // Shared prelude via the BlockInterface seam (leaders consume inputs in
  // slot order, ignored tie to Low, ties fan out, X/Z loud).
  if (!fill_resolved(nl, block_inputs, resolved).ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }

  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    gate_out[s] = LogicValue::kLow;
  }
  LogicValue ins[kMaxInputs];
  for (Slot oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    const Gate &g = nl.slots[s];
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (!resolve_input(nl, s, i, gate_out.data(), resolved.data(), ins[i], diag)) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
    }
    gate_out[s] = eval_gate(g.kind, ins, g.input_count);
  }

  std::size_t out_cursor = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = gate_out[s];
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      if (out_cursor >= block_outputs.size()) {
        return core::Status::failure(core::StatusCode::kInvalidArgument);
      }
      block_outputs[out_cursor++] = apply_polarity(gate_out[s], 1);
    }
  }
  if (out_cursor != block_outputs.size()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return core::Status::success();
}

/**
 * @brief Scalar single-assignment solve over the compiled level order.
 *
 * Workspace-backed entry: grows the pool's eval/Kahn scratch to the
 * pool size, then runs the core above. (No noexcept: workspace growth
 * may allocate.)
 *
 * @param[out] nl Netlist to solve (topology recompiled when dirty).
 * @param[in] block_inputs Input values in interface order.
 * @param[out] block_outputs Output values in interface order.
 * @param[in] diag Optional contention channel (nullptr = no counting; sized to the pool when present).
 * @return kOk on success; kInvalidArgument on topology errors
 *         (combinational cycle, dangling driver), size mismatches,
 *         X/Z inputs, or X/Z wired drivers.
 * @note Determinism: repeated calls with equal inputs are bit-identical
 *       (slot-order level order, slot-order output scan). No heap use
 *       once sized (amortized growth only).
 */
[[nodiscard]] inline core::Status solve(Netlist &nl, std::span<const LogicValue> block_inputs, std::span<LogicValue> block_outputs, SolveDiag *diag = nullptr) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  if (diag != nullptr && diag->per_net.size() < n * kMaxInputs) {
    diag->per_net.assign(n * kMaxInputs, 0);
  }
  return solve(nl, block_inputs, block_outputs, diag, std::span<LogicValue>(nl.ws_resolved.data(), n * kMaxInputs),
               std::span<LogicValue>(nl.ws_gate_out.data(), n));
}

/**
 * @brief Evaluate one 64-row slice starting at row word_index*64.
 *
 * Bit-parallel truth eval: 64 assignments per uint64_t word over the
 * compiled level order — each gate is one branchless word op (&|^|~).
 * Row convention (shared with scalar solve and the `truth` verb):
 * truth row r drives block input j with bit (r>>j)&1, where j is the
 * derived interface order (slot order, then input index). Word w covers
 * rows [w*64, w*64+63]; lane b holds row w*64+b as bit b.
 * input_column_word materializes one input's word procedurally, so
 * slices stay valid for any input count (bench nets exceed full-truth
 * enumeration; only slices are ever evaluated). Ignored dangling inputs
 * tie to word 0 (same rule as scalar kLow). The last partial word
 * (rows < 64, i.e. fewer than 6 block inputs) is zero-masked on outputs
 * so don't-care lanes never leak.
 *
 * @param[out] nl Netlist to evaluate (topology recompiled when dirty;
 *             input-position map and Kahn scratch ride the pool
 *             workspace).
 * @param[in] word_index 64-row slice index to evaluate.
 * @param[out] gate_scratch Caller-owned scratch (>= pool-size words).
 * @param[out] block_outputs Caller-owned outputs (exactly
 *             block_output_count(nl) words).
 * @return kOk on success; kInvalidArgument on bad scratch/output
 *         sizes, out-of-range slices (when n_inputs < 31), or topology
 *         errors.
 * @note Steady state allocates nothing and uses no SIMD intrinsics:
 *       scalar words carry the speedup and stay portable; vectorize
 *       only on a measured hot path.
 */
[[nodiscard]] inline core::Status eval_block_word(Netlist &nl, std::uint64_t word_index,
                                                   std::span<std::uint64_t> gate_scratch,
                                                   std::span<std::uint64_t> block_outputs) {
  // (011) Word-loop entry points (verbs, bench, exact) check
  // shared_word_blocked() once before looping: the per-word path stays
  // branch-identical to v1 (D11 fast-path claim), and a shared net with
  // >= 2 attachments is refused loudly there, never silently folded
  // into 1-bit lanes. Marked inputs with <= 1 attachment ride the
  // primary word path unchanged.
  core::Status st = topo::compile(nl, EdgeFilter::kAll);
  if (!st.ok()) {
    return st;
  }
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  if (gate_scratch.size() < n) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Dangling-input -> block position map via the BlockInterface seam
  // (same leader order scalar `solve` consumes; members share the leader's
  // column). kNoDriver = wired/ignored. Single pass: one O(G) query per
  // pin would be quadratic per word.
  build_input_positions(nl, nl.ws_in_pos.data());
  Slot *in_pos = nl.ws_in_pos.data();
  const std::size_t n_inputs = block_input_count(nl);
  std::size_t n_outputs = block_output_count(nl);
  if (block_outputs.size() != n_outputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (n_inputs < 31) {
    const std::size_t need = bit_words_for_inputs(n_inputs);
    if (word_index >= static_cast<std::uint64_t>(need)) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  for (Slot oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    const Gate &g = nl.slots[s];
    std::uint64_t acc = 0;
    switch (g.kind) {
    case GateKind::kAnd:
    case GateKind::kNand:
      acc = ~static_cast<std::uint64_t>(0ULL);
      for (std::uint8_t i = 0; i < g.input_count; ++i) {
        const std::uint64_t v = resolve_word(nl, s, i, gate_scratch.data(), in_pos, word_index);
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
        const std::uint64_t v = resolve_word(nl, s, i, gate_scratch.data(), in_pos, word_index);
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
        const std::uint64_t v = resolve_word(nl, s, i, gate_scratch.data(), in_pos, word_index);
        acc ^= v;
      }
      if (g.kind == GateKind::kXnor) {
        acc = ~acc;
      }
      break;
    case GateKind::kNot: {
      acc = ~resolve_word(nl, s, 0, gate_scratch.data(), in_pos, word_index);
      break;
    }
    case GateKind::kBuf: {
      acc = resolve_word(nl, s, 0, gate_scratch.data(), in_pos, word_index);
      break;
    }
    case GateKind::kTbuf:
      // (012) Four values do not fit in one bit: word eval over a live
      // buffer netlist is refused loudly at the word-loop entries
      // (truth/export check has_buffer_gates first), so this arm is a
      // safety net for future callers — unreachable, zero per-word
      // cost, keeps -Wswitch clean.
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    case GateKind::kDff:
    case GateKind::kDlatch:
    case GateKind::kJk:
    case GateKind::kT:
    case GateKind::kSrlatch:
      break; // SEQ4 owns sequential stepping; combinational word eval never
             // sees these (creation is guarded until SEQ2). acc stays 0.
    }
    gate_scratch[s] = acc;
  }

  std::size_t out_cursor = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      block_outputs[out_cursor++] = gate_scratch[s];
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      block_outputs[out_cursor++] = ~gate_scratch[s];
    }
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
