#pragma once

/**
 * @file
 * Four-state value tables (014) beside the zero-delay 2-state
 * evaluator: the dominant-value gate tables, the NQ-side read, and
 * the mid-window level-element response the transport-delay wheel
 * evaluates with. Value-level only — no nets, no topology, no
 * schedule, no trace — so value-only consumers include this header
 * without dragging in `shared_net`/`tie_set`/`topo` (the wheel in
 * `timing.hpp` is the only consumer that also needs those).
 *
 * Discipline (contracts/timing-semantics.md, research D5/D10):
 * transport-visible 4-state semantics (reference: IEEE 1364
 * gate-level semantics); header-only, allocation-free,
 * exception-free, no strings, no I/O, no syscalls,
 * `[[nodiscard]]` `constexpr` `noexcept` where actually guaranteed;
 * never includes CLI types (logic -> core/status only, and this
 * header needs not even that — `types.hpp` alone).
 *
 * D10 seam: `eval.hpp` gains zero code for this. The 2-state
 * `eval_gate` and these 4-state tables are never unified (the fast
 * path never branches on mode); their agreement on clean Low/High
 * nets is pinned by `tests/test_logic_four_state.cpp` instead — the
 * table-driven parity seam — not by shared code.
 *
 * Consumers: `timing.hpp` (wheel settle + drain) and the
 * four-state parity test. Nobody else links anything new
 * (`wengine_logic` stays an `INTERFACE` target; no `.cpp` TU).
 */

#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

/**
 * @brief NQ-side read of a flop value (X stays X).
 *
 * Unlike `apply_polarity` (whose non-High arm folds everything to
 * Low for the zero-delay consumer contract), the trace must show a
 * transient X on both pins — hence the separate helper. On clean
 * Low/High values the two agree (pinned by test); on X/HighZ they
 * deliberately differ (this passes through, the consumer folds).
 *
 * @param[in] q Flop Q value.
 * @return Low for High, High for Low, anything else untouched.
 */
[[nodiscard]] constexpr LogicValue timing_nq_of(LogicValue q) noexcept {
  if (q == LogicValue::kHigh) {
    return LogicValue::kLow;
  }
  if (q == LogicValue::kLow) {
    return LogicValue::kHigh;
  }
  return q;
}

/**
 * @brief Four-state gate evaluation with dominant-value tables
 *        (research D5, reference: IEEE 1364 gate-level semantics).
 *
 * AND/NAND output 0 if any input reads 0, else X if any input is
 * unknown, else 1 (NAND inverts, X stays X); OR/NOR dual with 1 as
 * the dominant value; XOR/XNOR yield X if any input is unknown
 * (parity has no dominant value); NOT/BUF pass X through; TBUF
 * drives Z only when released. Z at an input reads as X (Z is a net
 * state, not a gate operand — consistent with the shipped funnel
 * where Z is transparent and only consumers give it meaning).
 * Sequential kinds are never evaluated here (the wheel treats edge
 * flops as forces and latches via `timing_eval_latch`).
 *
 * On clean Low/High inputs this agrees with 2-state `eval_gate`
 * exactly (pinned by the parity test, never by shared code — D10).
 *
 * @param[in] kind Gate kind to evaluate (combinational only).
 * @param[in] inputs Per-input four-state values.
 * @param[in] n Input count.
 * @param[out] out Evaluated output.
 * @return True for combinational kinds; false for sequential kinds (caller keeps the current value).
 */
[[nodiscard]] constexpr bool timing_eval_gate(GateKind kind, const LogicValue *inputs, std::uint8_t n, LogicValue &out) noexcept {
  if (inputs == nullptr && n > 0) {
    return false;
  }
  auto unknown = [](LogicValue v) noexcept { return v == LogicValue::kUnknown || v == LogicValue::kHighZ; };
  switch (kind) {
  case GateKind::kAnd:
  case GateKind::kNand: {
    bool any_unknown = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (inputs[i] == LogicValue::kLow) {
        out = (kind == GateKind::kAnd) ? LogicValue::kLow : LogicValue::kHigh;
        return true;
      }
      any_unknown = any_unknown || unknown(inputs[i]);
    }
    if (any_unknown) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (kind == GateKind::kAnd) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  case GateKind::kOr:
  case GateKind::kNor: {
    bool any_unknown = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (inputs[i] == LogicValue::kHigh) {
        out = (kind == GateKind::kOr) ? LogicValue::kHigh : LogicValue::kLow;
        return true;
      }
      any_unknown = any_unknown || unknown(inputs[i]);
    }
    if (any_unknown) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (kind == GateKind::kOr) ? LogicValue::kLow : LogicValue::kHigh;
    return true;
  }
  case GateKind::kXor:
  case GateKind::kXnor: {
    bool odd = false;
    for (std::uint8_t i = 0; i < n; ++i) {
      if (unknown(inputs[i])) {
        out = LogicValue::kUnknown;
        return true;
      }
      if (inputs[i] == LogicValue::kHigh) {
        odd = !odd;
      }
    }
    const bool y = (kind == GateKind::kXor) ? odd : !odd;
    out = y ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  }
  case GateKind::kNot:
    if (n < 1) {
      return false;
    }
    if (unknown(inputs[0])) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (inputs[0] == LogicValue::kHigh) ? LogicValue::kLow : LogicValue::kHigh;
    return true;
  case GateKind::kBuf:
    if (n < 1) {
      return false;
    }
    if (unknown(inputs[0])) {
      out = LogicValue::kUnknown;
      return true;
    }
    out = (inputs[0] == LogicValue::kHigh) ? LogicValue::kHigh : LogicValue::kLow;
    return true;
  case GateKind::kTbuf: {
    if (n < 2) {
      return false;
    }
    if (inputs[1] == LogicValue::kHigh) {
      out = unknown(inputs[0]) ? LogicValue::kUnknown : inputs[0];
      return true;
    }
    if (inputs[1] == LogicValue::kLow) {
      out = LogicValue::kHighZ;
      return true;
    }
    out = LogicValue::kUnknown;
    return true;
  }
  case GateKind::kDff:
  case GateKind::kDlatch:
  case GateKind::kJk:
  case GateKind::kT:
  case GateKind::kSrlatch:
    break;
  }
  return false;
}

/**
 * @brief Mid-window level-element response (contracts/timing-semantics.md).
 *
 * DLATCH: asserted enable tracks D (X passes through); anything
 * else holds. SRLATCH: S=R=1 renders X (the transient the settled
 * refusal would hide), S drives 1, R drives 0, S=R=0 holds, any
 * other unknown combination renders X. The annotation on a level
 * element is this response's delay; the settled contract is
 * untouched (settled S=R=1 still refuses on the unchanged path).
 *
 * @param[in] kind kDlatch or kSrlatch (anything else fails).
 * @param[in] a D (latch) or S (SR) value, Z already mapped to X.
 * @param[in] b EN (latch) or R (SR) value, Z already mapped to X.
 * @param[in] hold Current output (the hold source).
 * @param[out] out Recomputed output.
 * @return True for the two level kinds; false otherwise.
 */
[[nodiscard]] constexpr bool timing_eval_latch(GateKind kind, LogicValue a, LogicValue b, LogicValue hold, LogicValue &out) noexcept {
  if (kind == GateKind::kDlatch) {
    out = (b == LogicValue::kHigh) ? a : hold;
    return true;
  }
  if (kind == GateKind::kSrlatch) {
    if (a == LogicValue::kHigh && b == LogicValue::kHigh) {
      out = LogicValue::kUnknown;
      return true;
    }
    if (a == LogicValue::kHigh) {
      out = LogicValue::kHigh;
      return true;
    }
    if (b == LogicValue::kHigh) {
      out = LogicValue::kLow;
      return true;
    }
    if (a == LogicValue::kLow && b == LogicValue::kLow) {
      out = hold;
      return true;
    }
    out = LogicValue::kUnknown;
    return true;
  }
  return false;
}

} // namespace wengine::logic
