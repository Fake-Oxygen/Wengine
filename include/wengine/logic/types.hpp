#pragma once

/**
 * @file
 * Logic-gate netlist vocabulary (scaffold — LOGIC-2 fills in eval):
 * gate kinds, net values, pin references, and the fixed-capacity
 * Netlist pool that every other logic header builds on. This header
 * is the data-model root of the logic pipeline (netlist parse ->
 * state -> eval -> layout/topo -> optimize).
 *
 * Design (docs/logic.md §4): combinational block of gates with variable
 * fan-in (2..8, NOT/BUF exactly 1). Identity is per-kind monotonic
 * counters never reused (AND1, NOT5, ...), slots recycle — the same
 * convention as signal serials. Pins spell `<NAME>.Y` (output) or
 * `<NAME>.<1..k>` (input). Single-driver inputs, unlimited fan-out,
 * cycles rejected (combinational v1 only).
 *
 * Dependency rules: header-only, standard-library-only (STATUS-free
 * vocabulary; fallible ops use core::Status/Result at the eval/io
 * layer, never here). No iostream. Logic is a standalone domain: this
 * header (and every logic header below it) must never depend on CLI
 * types. Consumers: pins/netlist/eval/topo/tie_set/block_interface and
 * the rest of the logic pipeline, the CLI `logic` verbs, and the
 * test_logic* suite.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::logic {

/**
 * @brief Gate kind: combinational primitives (0..7) plus sequential
 *        elements (16..20).
 *
 * Append-only: persisted as bytes in YAML v1 (docs/logic.md §9).
 * Values 0..7 are combinational; sequential kinds (DFF/LATCH/...)
 * append at >= 16 in V2 (docs/logic.md §12) — never renumber, never
 * reuse persisted byte values.
 */
enum class GateKind : std::uint8_t {
  /** @brief Bitwise AND over 2..8 inputs. */
  kAnd = 0,
  /** @brief Bitwise OR over 2..8 inputs. */
  kOr = 1,
  /** @brief NAND: negated AND over 2..8 inputs. */
  kNand = 2,
  /** @brief NOR: negated OR over 2..8 inputs. */
  kNor = 3,
  /** @brief XOR: odd parity over 2..8 inputs. */
  kXor = 4,
  /** @brief XNOR: negated XOR (even parity). */
  kXnor = 5,
  /** @brief Inverter, exactly 1 input. */
  kNot = 6,
  /** @brief Buffer, exactly 1 input (needed as an optimizer output). */
  kBuf = 7,
  /**
   * @brief Edge-triggered D flip-flop (D + CLK).
   *
   * DFF takes 2 (bare D+CLK, default) or 5 inputs (full control set
   * D+CLK+RST+SET+EN, opt-in). V2 sequential (LOGIC-V2-SEQ1): no
   * eval/state change in SEQ1 — vocabulary only.
   */
  kDff = 16,
  /**
   * @brief Level-sensitive D latch (D + EN).
   *
   * DLATCH takes exactly 2 inputs (valid_fanin); transparent while EN
   * is active, frozen while released. V2 sequential (LOGIC-V2-SEQ1):
   * vocabulary only.
   */
  kDlatch = 17,
  /**
   * @brief Edge-triggered JK flip-flop (J + K + CLK, exactly 3 inputs).
   *
   * V2 JK/T (003-jk-t-flops): both ride the same single-edge stepper;
   * excitation resolves in seq_step.
   */
  kJk = 18,
  /**
   * @brief Edge-triggered T flip-flop (T + CLK, exactly 2 inputs).
   *
   * V2 JK/T (003-jk-t-flops): same stepper and verbs as DFF/JK;
   * excitation resolves in seq_step.
   */
  kT = 19,
  /**
   * @brief Level-sensitive SR latch (S + R, exactly 2 inputs).
   *
   * V2 SR (004-sr-d-latches): set/reset/hold plus loud S=R=1 refusal
   * in the level pass.
   */
  kSrlatch = 20,
};

/**
 * @brief True iff the kind owns clocked state (see seq_state.hpp).
 *
 * Sequential kinds appear in the state pool; combinational gates never
 * do. Explicit (not `ordinal >= 16`) so future appended kinds stay
 * stateless until their own task wires them — never silently
 * DFF-shaped. Lives here (kind vocabulary, beside the enum) so the
 * TieSet and BlockInterface seams can use it without including
 * netlist.hpp.
 *
 * @param[in] k Kind to test.
 * @return True for DFF/DLATCH/JK/T/SRLATCH, false otherwise.
 */
[[nodiscard]] constexpr bool is_sequential_kind(GateKind k) noexcept {
  return k == GateKind::kDff || k == GateKind::kDlatch || k == GateKind::kJk || k == GateKind::kT || k == GateKind::kSrlatch;
}

/**
 * @brief Net value.
 *
 * kLow/kHigh are the driven levels. kHighZ (undriven / tri-state
 * off) and kUnknown (conflict marker) carry resolution semantics in
 * resolve_net (docs/logic.md §12): high-Z contributes nothing,
 * unknown dominates low/high, and active low+high contention
 * resolves to unknown. Consumers that need a definite level (gate
 * inputs, sequential control nets, block sinks) still reject Z/X
 * loudly at their own seams.
 */
enum class LogicValue : std::uint8_t {
  /** @brief Logic 0. */
  kLow = 0,
  /** @brief Logic 1. */
  kHigh = 1,
  /** @brief Undriven / tri-state off: transparent in resolution. */
  kHighZ = 2,   // undriven / tri-state off (transparent to resolve_net)
  /** @brief Conflict marker: dominates low/high in resolution. */
  kUnknown = 3, // conflict marker (low+high contention, unknown driver)
};

/**
 * @brief The net-value numbering is append-only: kHighZ == 2 and
 *        kUnknown == 3 must never change (persisted vocabulary
 *        convention, docs/logic.md §12).
 */
static_assert(static_cast<int>(LogicValue::kHighZ) == 2 && static_cast<int>(LogicValue::kUnknown) == 3,
              "LogicValue numbering is append-only");

/**
 * @brief Single funnel every input resolves through.
 *
 * Total four-value wired resolution (docs/logic.md §12): high-Z
 * drivers are transparent, any kUnknown driver dominates low/high,
 * active low+high contention resolves to kUnknown, active consensus
 * resolves to the agreed level, and a span with no active drivers
 * (empty or all high-Z) resolves to kHighZ. The result depends only
 * on the driver-value multiset, never on slot order, so identical
 * spans always resolve identically.
 *
 * @param[in] drivers Span of candidate driver values; may be empty.
 * @param[in] n Span length; 0 means an undriven net (kHighZ).
 * @param[out] out Resolved net value on success; untouched when the span is ill-formed.
 * @return True on success: every well-formed span resolves to exactly
 *         one of the four values. False only for an ill-formed span
 *         (drivers == nullptr with n > 0). Call sites unchanged from v1.
 */
[[nodiscard]] constexpr bool resolve_net(const LogicValue *drivers, std::size_t n, LogicValue &out) noexcept {
  if (drivers == nullptr && n > 0) {
    return false; // ill-formed span: no values where n claims there are
  }
  LogicValue acc = LogicValue::kHighZ; // identity: undriven until proven driven
  for (std::size_t i = 0; i < n; ++i) {
    const LogicValue v = drivers[i];
    if (v == LogicValue::kHighZ) {
      continue; // transparent: a disabled driver contributes nothing
    }
    if (acc == LogicValue::kHighZ) {
      acc = v; // first active driver sets the running value
      continue;
    }
    if (v == LogicValue::kUnknown || acc == LogicValue::kUnknown || v != acc) {
      acc = LogicValue::kUnknown; // unknown dominates; low+high contention
    }
  }
  out = acc;
  return true;
}

/**
 * @brief Apply NQ sink polarity (T005): invert an already-resolved
 *        Low/High value when the sink input is wired from a flop NQ
 *        output.
 *
 * @param[in] v Resolved Low/High value.
 * @param[in] inv Non-zero for an NQ-wired sink (reads the negated
 *                source value); zero otherwise.
 * @return v untouched when inv is 0, otherwise the Low/High inverse —
 *         non-inverted sinks pass through untouched.
 */
[[nodiscard]] constexpr LogicValue apply_polarity(LogicValue v, std::uint8_t inv) noexcept {
  if (inv == 0) {
    return v;
  }
  return (v == LogicValue::kHigh) ? LogicValue::kLow : LogicValue::kHigh;
}

/** @brief Maximum gate fan-in (8): the allowed per-kind range is 1..8. */
inline constexpr std::size_t kMaxInputs = 8;
/** @brief Gate slot cap (1024); overflow is loud, never silent. */
inline constexpr std::size_t kMaxGates = 1024; // slot cap, loud on overflow
/**
 * @brief Extra-driver cap per shared input (011): a shared net holds the
 *        primary driver plus up to kMaxSharedExtra extras — 4 attachments
 *        total per input. Overflow is loud, never silently truncated.
 */
inline constexpr std::size_t kMaxSharedExtra = 3;

/**
 * @brief Display name for a gate kind (e.g. "AND", "DFF").
 *
 * @param[in] k Kind to name.
 * @return Uppercase kind name; "?" for an unknown kind value.
 * @note Display-only spelling: pins.hpp (parse_gate_kind / kKinds)
 *       owns the accepted input spellings — keep the tables in sync
 *       when adding a kind.
 */
[[nodiscard]] constexpr std::string_view gate_kind_name(GateKind k) noexcept {
  switch (k) {
  case GateKind::kAnd:
    return "AND";
  case GateKind::kOr:
    return "OR";
  case GateKind::kNand:
    return "NAND";
  case GateKind::kNor:
    return "NOR";
  case GateKind::kXor:
    return "XOR";
  case GateKind::kXnor:
    return "XNOR";
  case GateKind::kNot:
    return "NOT";
  case GateKind::kBuf:
    return "BUF";
  case GateKind::kDff:
    return "DFF";
  case GateKind::kDlatch:
    return "DLATCH";
  case GateKind::kJk:
    return "JK";
  case GateKind::kT:
    return "T";
  case GateKind::kSrlatch:
    return "SRLATCH";
  }
  return "?";
}

/**
 * @brief One struct for every pin address — parsed references,
 *        mutation targets, tie members, and block boundary entries
 *        alike (WENGI-43 unified the former BlockPin here).
 *
 * Pin vocabulary (moved from netlist.hpp so the TieSet seam in
 * tie_set.hpp can take pins without a netlist.hpp include cycle).
 */
struct PinRef {
  /** @brief Gate slot index the pin belongs to. */
  std::uint16_t slot = 0;
  /** @brief True for an output pin (Y / Q / NQ), false for an input. */
  bool is_output = false;
  /** @brief 0-based input index, valid iff not is_output. */
  std::uint8_t input = 0; // 0-based, valid iff !is_output
  /** @brief NQ source/output marker, valid iff is_output (sequential gates). */
  bool negated = false;   // valid iff is_output: NQ source/output on sequential gates
};

/**
 * @brief Sentinel driver slot for an unwired input (0xFFFF); also
 *        borrowed as the not-a-block-input marker by the interface
 *        seam (block_interface.hpp).
 */
inline constexpr std::uint16_t kNoDriver = 0xFFFF;

/**
 * @brief One gate slot in the fixed-capacity pool.
 *
 * `live` false => free-list entry. `counter` is the per-kind monotonic
 * id (display: AND<counter>); `input_count` the configured fan-in;
 * `ignored_mask` bitmask (bit i => input i+1 excluded from the derived
 * block interface) plus `ignore_output` for dangling outputs;
 * `drivers` holds the source slot index per input, kNoDriver when
 * unwired.
 */
struct Gate {
  /** @brief Slot occupancy: false marks a free-list entry. */
  bool live = false;
  /** @brief Gate kind (combinational function or sequential element). */
  GateKind kind = GateKind::kAnd;
  /** @brief Per-kind monotonic id, never reused (display: AND<counter>). */
  std::uint32_t counter = 0; // per-kind, never reused
  /** @brief Configured fan-in; validated per kind at creation. */
  std::uint8_t input_count = 2;
  /** @brief Bit i set => input (i+1) excluded from the derived block interface. */
  std::uint8_t ignored_mask = 0; // bit i => input (i+1) ignored
  /** @brief Dangling output excluded from the derived block interface. */
  bool ignore_output = false; // dangling output excluded from iface
  /**
   * @brief Dangling NQ excluded from the interface by default
   *        (sequential only; `use` re-exposes, wiring bypasses — NQ
   *        shows only on explicit use).
   */
  bool ignore_nq_output = true; // dangling NQ excluded from iface by default
                                // (sequential only; `use` re-exposes, wiring
                                // bypasses — NQ shows only on explicit use)
  /** @brief Derived Q fanout (wired sinks), for list/iface. */
  std::uint16_t output_fanout = 0; // derived Q fanout, for list/iface
  /** @brief Derived NQ fanout, sequential gates only. */
  std::uint16_t nq_fanout = 0;     // derived NQ fanout, sequential gates only
  /**
   * @brief V2 timing hook (docs/logic.md §12): stored now, honored
   *        later; v1 eval is zero-delay regardless of value.
   */
  std::uint64_t propagation_delay_ps = 0; // V2 timing hook (§12): stored
                                          // now, honored later; v1 eval is
                                          // zero-delay regardless of value
  /** @brief Source slot index per input; kNoDriver when unwired. */
  std::array<std::uint16_t, kMaxInputs> drivers{};

  /**
   * @brief Value equality (whole-slot compare, defaulted).
   *
   * @return True when every field of the two slots compares equal.
   */
  bool operator==(const Gate &) const noexcept = default;
};

/**
 * @brief Fixed-capacity SoA-ish pool: slots recycle, counters don't.
 *        Mutation marks `dirty` (topology must be recompiled before
 *        eval).
 *
 * Tied inputs (LOGIC-14): input-input shorts as first-class nets. Only
 * dangling (unwired, non-ignored) input pins may be tied. Equivalence
 * classes are stored as a star: `tie_rep[flat] == 0` means singleton
 * (untied); otherwise it holds `leader_flat + 1` where leader_flat is
 * the class canonical rep (lowest slot/input wins, slot-order
 * enumeration). Zero-initialization is the all-singleton state by
 * construction, so `Netlist{}` starts untied and `reset_netlist` clears
 * to zero. Interface derivation counts one block input per class
 * (leaders only); scalar and bit-parallel eval feed/share one value per
 * class.
 */
struct Netlist {
  /** @brief Gate slots; a dead slot is a free-list entry. */
  std::array<Gate, kMaxGates> gates{};
  /**
   * @brief Per-kind monotonic counters (never reused), indexed by
   *        kind_ordinal: 0..7 combinational, 16 DFF, 17 DLATCH, 18 JK,
   *        19 T, 20 SR (8..15 unused, stay 1).
   */
  std::array<std::uint32_t, 21> next_counter{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  /**
   * @brief Per-sink output polarity: driver_inv[flat] != 0 means the
   *        sink input reads the NEGATED source value (wire from a flop
   *        NQ output). Zero is the all-non-inverted state by
   *        construction; slot-level wire() always clears, wire_pins()
   *        sets it for NQ sources, unwire()/remove clear it.
   */
  std::array<std::uint8_t, kMaxGates * kMaxInputs> driver_inv{};
  /**
   * @brief Tie-class star (see struct doc): tie_rep[flat] == 0 is a
   *        singleton; otherwise leader_flat + 1.
   */
  std::array<std::uint16_t, kMaxGates * kMaxInputs> tie_rep{};
  /**
   * @brief Shared-net mark + extra count per input (011): byte value 0
   *        is unmarked (the fast test on both eval paths); bit0 is the
   *        shared mark, bits1..2 hold the extra-attachment count 0..3.
   *        Indexed by tie_flat(slot, input). Marks are persistent
   *        topology state; counts are diagnostic session state beside
   *        this pool (SolveDiag), never stored here.
   */
  std::array<std::uint8_t, kMaxGates * kMaxInputs> shared_meta{};
  /**
   * @brief Extra source slots per shared input (011): dense entries
   *        0..count-1 at flat * kMaxSharedExtra + j. The primary
   *        attachment stays in Gate::drivers (kNoDriver semantics
   *        preserved); extras never dangle behind a kNoDriver primary.
   */
  std::array<std::uint16_t, kMaxGates * kMaxInputs * kMaxSharedExtra> extra_drivers{};
  /**
   * @brief Per-extra NQ polarity (011): extra_inv at the same index as
   *        extra_drivers; nonzero inverts that attachment's value before
   *        the resolve_net fold (Q+NQ self-contention resolves to X).
   */
  std::array<std::uint8_t, kMaxGates * kMaxInputs * kMaxSharedExtra> extra_inv{};
  /** @brief Number of live gate slots. */
  std::uint16_t live_count = 0;
  /** @brief Set by every mutation; cleared by a successful topology rebuild. */
  bool dirty = true;
  /** @brief Compiled evaluation order (slot indices; Kahn output). */
  std::array<std::uint16_t, kMaxGates> topo_order{};
  /** @brief Per-slot level in the compiled order (0 for sources and sequential roots). */
  std::array<std::uint16_t, kMaxGates> topo_level{};
  /** @brief Number of valid entries in topo_order. */
  std::uint16_t topo_size = 0;

  /**
   * @brief Value equality (whole-pool compare, defaulted).
   *
   * @return True when every pool field compares equal (gates, counters,
   *         polarities, ties, topology caches included).
   */
  bool operator==(const Netlist &) const noexcept = default;
};

/**
 * @brief True when the flat destination input carries the shared mark
 *        (011): bit0 of shared_meta[flat].
 *
 * Flat-index form of the mark test, usable from headers that cannot
 * see pin vocabulary (tie_set.hpp): marks and ties are disjoint per
 * flat — a tied input is never marked and a marked input is never
 * tied, enforced loudly on both seams.
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] flat Flat pin index (slot * kMaxInputs + input).
 * @return True when the flat position carries the shared mark.
 */
[[nodiscard]] inline bool is_shared_flat(const Netlist &nl, std::size_t flat) noexcept {
  if (flat >= kMaxGates * kMaxInputs) {
    return false;
  }
  return (nl.shared_meta[flat] & 0x01u) != 0;
}

/**
 * @brief Extra-attachment count at a flat position, 0..kMaxSharedExtra
 *        (011): bits1..2 of shared_meta[flat].
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] flat Flat pin index (slot * kMaxInputs + input).
 * @return Extra count (0 when unmarked or out of range).
 */
[[nodiscard]] inline std::uint8_t shared_extra_flat(const Netlist &nl, std::size_t flat) noexcept {
  if (flat >= kMaxGates * kMaxInputs) {
    return 0;
  }
  return static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
}

} // namespace wengine::logic
