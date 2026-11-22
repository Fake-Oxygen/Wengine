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

/**
 * @brief One gate's combinational word op over a full batch (017).
 *
 * Pure, capacity-free, noexcept: the single spelling of gate truth
 * shared by the batched walk (one call per gate per batch). Input
 * words are input-major ([i][w] at in_words[i*kWordBatchWidth+w],
 * n_inputs entries); output is one word per lane. AND/OR/XOR fold
 * with the NAND/NOR/XNOR negation; NOT/BUF are single-input moves;
 * sequential kinds contribute zero (SEQ4 owns clocked state — same
 * convention as the single-slice walk). kTbuf never reaches here:
 * the walk refuses live buffers before calling (same safety net as
 * the single-slice arm).
 *
 * @param[in] kind Gate kind to evaluate.
 * @param[in] in_words Input lane words, input-major (n_inputs * W).
 * @param[in] n_inputs Input count; matches valid_fanin(kind, n).
 * @param[out] out_words Output lane words (W entries).
 */
inline void apply_gate_words(GateKind kind, const std::uint64_t *in_words, std::size_t n_inputs,
                             std::uint64_t *out_words) noexcept {
  switch (kind) {
  case GateKind::kAnd:
  case GateKind::kNand: {
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = ~static_cast<std::uint64_t>(0ULL);
    }
    for (std::size_t i = 0; i < n_inputs; ++i) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] &= in_words[i * kWordBatchWidth + w];
      }
    }
    if (kind == GateKind::kNand) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] = ~out_words[w];
      }
    }
    break;
  }
  case GateKind::kOr:
  case GateKind::kNor: {
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = 0;
    }
    for (std::size_t i = 0; i < n_inputs; ++i) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] |= in_words[i * kWordBatchWidth + w];
      }
    }
    if (kind == GateKind::kNor) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] = ~out_words[w];
      }
    }
    break;
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = 0;
    }
    for (std::size_t i = 0; i < n_inputs; ++i) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] ^= in_words[i * kWordBatchWidth + w];
      }
    }
    if (kind == GateKind::kXnor) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        out_words[w] = ~out_words[w];
      }
    }
    break;
  }
  case GateKind::kNot: {
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = ~in_words[w];
    }
    break;
  }
  case GateKind::kBuf: {
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = in_words[w];
    }
    break;
  }
  case GateKind::kTbuf:
    // Unreachable: the walk refuses live buffers before calling (same
    // safety net as the single-slice arm). Zero lanes, keeps -Wswitch
    // clean without a throw or a status channel (noexcept holder).
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = 0;
    }
    break;
  case GateKind::kDff:
  case GateKind::kDlatch:
  case GateKind::kJk:
  case GateKind::kT:
  case GateKind::kSrlatch:
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      out_words[w] = 0;
    }
    break; // SEQ4 owns sequential stepping; combinational word eval never
           // sees these (creation is guarded until SEQ2). acc stays 0.
  }
}

/**
 * @brief Bit-parallel twin of resolve_word for one batch lane (017).
 *
 * Same case tree as resolve_word (block_interface.hpp) over the same
 * InputResolution seam — the single-slice spelling stays frozen, so
 * the batch path mirrors it lane by lane and the parity corpus
 * (test_logic_word_batch) proves bitequality. Differences from the
 * twin: gate words read slot-major ([slot][lane] at
 * gate_scratch[slot*W+lane]) and the dangling-column word is
 * materialized for word_base+lane instead of one slice.
 *
 * @param[in] nl Netlist holding drivers, ignore marks, and NQ polarity.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[in] gate_scratch Slot-major per-gate lane words from the
 *            compiled level order.
 * @param[in] in_pos (slot,input)->ordinal map from build_input_positions.
 * @param[in] word_index Absolute 64-row slice index for this lane.
 * @param[in] lane Lane within the batch (word_index == word_base+lane).
 * @return 64-bit resolved word for this input over this lane's slice.
 */
[[nodiscard]] inline std::uint64_t resolve_word_lane(const Netlist &nl, Slot slot, std::uint8_t input,
                                                     const std::uint64_t *gate_scratch, const Slot *in_pos,
                                                     std::uint64_t word_index, std::size_t lane) noexcept {
  input_res::InputSpan span{};
  input_res::assemble_input_span(nl, slot, input, span);
  if (span.n == 1 && !span.has_dead) {
    // Wired single (or a single-attachment shared mark riding the
    // primary word path unchanged): one source lane with NQ polarity.
    // Bounds hold by construction (topology pre-validated drivers; the
    // dead guard above keeps the read in-bounds on defensive paths).
    if (span.drv[0] >= slot_count(nl)) {
      return 0;
    }
    return input_res::apply_word_polarity(gate_scratch[static_cast<std::size_t>(span.drv[0]) * kWordBatchWidth + lane],
                                          span.inv[0]);
  }
  if (span.n > 1) {
    // Multi-attachment shared span: no 1-bit form. Word-loop entries
    // refuse via shared_word_blocked before looping; this arm is the
    // per-lane safety net for future callers (unreachable, zero
    // per-lane cost).
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
  // rule in fill_resolved. Pull-only netlists stream unchanged.
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

/**
 * @brief Evaluate W consecutive 64-row slices starting at row word_base*64.
 *
 * Slice-blocked twin of eval_block_word (017): one level-order
 * traversal produces kWordBatchWidth slices of outputs, with words
 * bit-identical per slice to the single-slice path — same row/lane
 * convention, same guards (four-value nets refused at entries; kTbuf
 * arm fails loudly; sequential kinds contribute zero words), same
 * per-word partial masking, same loud kInvalidArgument on undersized
 * scratch, output-size mismatch, or out-of-range lanes (n_inputs < 31
 * rule applied to every lane). Scratch is slot-major
 * ([slot][lane]); outputs are output-major ([output][lane]).
 *
 * @param[out] nl Netlist to evaluate (topology recompiled when dirty;
 *             input-position map and Kahn scratch ride the pool
 *             workspace).
 * @param[in] word_base First 64-row slice index of the batch.
 * @param[out] gate_scratch Caller-owned scratch (>= slot_count *
 *             kWordBatchWidth words, slot-major).
 * @param[out] block_outputs Caller-owned outputs (exactly
 *             block_output_count(nl) * kWordBatchWidth words,
 *             output-major).
 * @return kOk on success; kInvalidArgument on bad scratch/output
 *         sizes, out-of-range lanes (any lane past the truth when
 *         n_inputs < 31), or topology errors.
 * @note Steady state allocates nothing and uses no SIMD intrinsics:
 *       scalar lane words carry the speedup (auto-vectorized at -O3)
 *       and stay portable; vectorize only on a measured hot path.
 */
[[nodiscard]] inline core::Status eval_block_words(Netlist &nl, std::uint64_t word_base,
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
  if (gate_scratch.size() < n * kWordBatchWidth) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Dangling-input -> block position map via the BlockInterface seam
  // (same leader order scalar `solve` consumes; members share the leader's
  // column). kNoDriver = wired/ignored. Single pass: one O(G) query per
  // pin would be quadratic per word — the map is built once per batch,
  // not once per slice.
  build_input_positions(nl, nl.ws_in_pos.data());
  Slot *in_pos = nl.ws_in_pos.data();
  const std::size_t n_inputs = block_input_count(nl);
  const std::size_t n_outputs = block_output_count(nl);
  if (block_outputs.size() != n_outputs * kWordBatchWidth) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (n_inputs < 31) {
    const std::size_t need = bit_words_for_inputs(n_inputs);
    if (word_base >= static_cast<std::uint64_t>(need) ||
        (kWordBatchWidth - 1) >= (need - static_cast<std::size_t>(word_base))) {
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
  }
  std::uint64_t in_words[kMaxInputs * kWordBatchWidth];
  std::uint64_t out_words[kWordBatchWidth];
  for (Slot oi = 0; oi < nl.topo_order.size(); ++oi) {
    const Slot s = nl.topo_order[oi];
    const Gate &g = nl.slots[s];
    if (g.kind == GateKind::kTbuf) {
      // (012) Four values do not fit in one bit: word eval over a live
      // buffer netlist is refused loudly at the word-loop entries
      // (truth/export check has_buffer_gates first), so this arm is a
      // safety net for future callers — unreachable, zero per-lane
      // cost, keeps -Wswitch clean.
      return core::Status::failure(core::StatusCode::kInvalidArgument);
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        in_words[static_cast<std::size_t>(i) * kWordBatchWidth + w] =
            resolve_word_lane(nl, s, i, gate_scratch.data(), in_pos, word_base + static_cast<std::uint64_t>(w), w);
      }
    }
    apply_gate_words(g.kind, in_words, g.input_count, out_words);
    for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
      gate_scratch[static_cast<std::size_t>(s) * kWordBatchWidth + w] = out_words[w];
    }
  }

  std::size_t out_cursor = 0;
  for (Slot s = 0; s < slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    if (g.output_fanout == 0 && !g.ignore_output) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        block_outputs[out_cursor * kWordBatchWidth + w] = gate_scratch[static_cast<std::size_t>(s) * kWordBatchWidth + w];
      }
      ++out_cursor;
    }
    if (is_sequential_kind(g.kind) && g.nq_fanout == 0 && !g.ignore_nq_output) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        block_outputs[out_cursor * kWordBatchWidth + w] = ~gate_scratch[static_cast<std::size_t>(s) * kWordBatchWidth + w];
      }
      ++out_cursor;
    }
  }
  if (n_inputs < 6) {
    const auto rows = static_cast<std::uint64_t>(1ULL << n_inputs);
    const std::uint64_t mask = (rows >= 64) ? ~static_cast<std::uint64_t>(0ULL) : (static_cast<std::uint64_t>(1ULL << rows) - 1ULL);
    for (std::size_t i = 0; i < n_outputs; ++i) {
      for (std::size_t w = 0; w < kWordBatchWidth; ++w) {
        block_outputs[i * kWordBatchWidth + w] &= mask;
      }
    }
  }
  return core::Status::success();
}

/**
 * @brief Batched word-eval over the pool workspace (017).
 *
 * Pool-backed convenience: grows ws_wordblock amortized, then runs
 * the caller-scratch core above. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[out] nl Netlist to evaluate (topology recompiled when dirty).
 * @param[in] word_base First 64-row slice index of the batch.
 * @param[out] block_outputs Caller-owned outputs (exactly
 *             block_output_count(nl) * kWordBatchWidth words,
 *             output-major).
 * @return kOk on success; kInvalidArgument on output-size mismatch,
 *         out-of-range lanes, or topology errors.
 */
[[nodiscard]] inline core::Status eval_block_words(Netlist &nl, std::uint64_t word_base, std::span<std::uint64_t> block_outputs) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return eval_block_words(nl, word_base,
                          std::span<std::uint64_t>(nl.ws_wordblock.data(), n * kWordBatchWidth), block_outputs);
}

} // namespace wengine::logic
