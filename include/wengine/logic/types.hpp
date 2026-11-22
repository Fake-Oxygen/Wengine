#pragma once

/**
 * @file
 * Logic-gate netlist vocabulary: gate kinds, net values, pin references,
 * and the growable Netlist pool that every other logic header builds on.
 * This header is the data-model root of the logic pipeline (netlist parse
 * -> state -> eval -> layout/topo -> optimize).
 *
 * Design (docs/logic.md §4): combinational block of gates with variable
 * fan-in (2..8, NOT/BUF exactly 1, TBUF exactly 2). Identity is per-kind monotonic
 * counters never reused (AND1, NOT5, ...), slots recycle — the same
 * convention as signal serials. Pins spell `<NAME>.Y` (output) or
 * `<NAME>.<1..k>` (input). Single-driver inputs, unlimited fan-out,
 * cycles rejected (combinational v1 only).
 *
 * Storage (dynamic pool): the pool grows geometrically on demand
 * (lowest-free-slot order, so allocation order is identical to the old
 * fixed pool for identical op sequences), and every scan/init is bounded
 * by the current pool size — never by the max cap. A five-gate net costs
 * kilobytes, not hundreds of megabytes. Growth past kMaxGates fails
 * loudly ("netlist full"), never silently. Slot ids are 32-bit; the
 * pool, scratch, and counters live on the heap (Netlist is a small
 * handle: copy/move are deep but proportional to live size).
 *
 * Dependency rules: header-only, standard-library-only (STATUS-free
 * vocabulary; fallible ops use core::Status/Result at the eval/io
 * layer, never here — growth reports plain bool, callers map past-cap
 * refusal to Status). No iostream. Logic is a standalone domain: this
 * header (and every logic header below it) must never depend on CLI
 * types. Consumers: pins/netlist/topo/tie_set/shared_net/block_interface
 * and the rest of the logic pipeline, the CLI `logic` verbs, and the
 * test_logic* suite. Shared-mark vocabulary lives in shared_net.hpp
 * (the SharedNet seam owns shared_meta/extra_* directly); pull
 * vocabulary stays here beside the pool.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wengine::logic {

/**
 * @brief Gate kind: combinational primitives (0..8) plus sequential
 *        elements (16..20).
 *
 * Append-only: persisted as bytes in YAML v1 (docs/logic.md §9).
 * Values 0..8 are combinational; sequential kinds (DFF/LATCH/...)
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
   * @brief Enable-gated buffer (TBUF), exactly 2 inputs: pin 1 is data,
   *        pin 2 is the active-high enable. Enabled output drives the
   *        data level; released output drives kHighZ (the first legal
   *        Z producer in gate_out). V2 gated buses (012): combinational,
   *        stateless, ordinary drivers[] consumers.
   */
  kTbuf = 8,
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
/**
 * @brief Loud maximum slot cap (1048576): growth past this fails loudly,
 *        never silently. Pool storage and every scan/init are bounded by
 *        the current pool size — the cap only bounds how far the pool may
 *        grow, never how much idle storage a small net carries.
 */
inline constexpr std::size_t kMaxGates = 1048576; // loud max slot cap
/**
 * @brief Pool growth seed: a fresh Netlist holds this many slots; growth
 *        doubles from here. Small nets stay kilobytes.
 */
inline constexpr std::size_t kInitialSlots = 64;
/**
 * @brief Extra-driver cap per shared input (011): a shared net holds the
 *        primary driver plus up to kMaxSharedExtra extras — 4 attachments
 *        total per input. Overflow is loud, never silently truncated.
 */
inline constexpr std::size_t kMaxSharedExtra = 3;

/**
 * @brief Gate slot index width: 32 bits, the only width. The pool grows
 *        to kMaxGates slots, so one width covers every net — no
 *        instantiation duality, no width-conditional code.
 */
using Slot = std::uint32_t;

/**
 * @brief Sentinel driver slot for an unwired input; also borrowed as
 *        the not-a-block-input marker by the interface seam
 *        (block_interface.hpp).
 */
inline constexpr Slot kNoDriver = 0xFFFFFFFFu;

static_assert(kNoDriver == 0xFFFFFFFFu, "kNoDriver keeps the all-ones sentinel");

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
  case GateKind::kTbuf:
    return "TBUF";
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
  Slot slot = 0;
  /** @brief True for an output pin (Y / Q / NQ), false for an input. */
  bool is_output = false;
  /** @brief 0-based input index, valid iff not is_output. */
  std::uint8_t input = 0; // 0-based, valid iff !is_output
  /** @brief NQ source/output marker, valid iff is_output (sequential gates). */
  bool negated = false; // valid iff is_output: NQ source/output on sequential gates

  /**
   * @brief Value equality (whole-pin compare, defaulted).
   *
   * @return True when every field compares equal.
   */
  bool operator==(const PinRef &) const noexcept = default;
};

/**
 * @brief One gate slot in the growable pool.
 *
 * `live` false => free entry (recycled by the lowest-free-slot rule).
 * `counter` is the per-kind monotonic id (display: AND<counter>);
 * `input_count` the configured fan-in; `ignored_mask` bitmask (bit i
 * => input i+1 excluded from the derived block interface) plus
 * `ignore_output` for dangling outputs; `drivers` holds the source
 * slot index per input, kNoDriver when unwired (set explicitly by
 * add_gate; a fresh slot reads zero until then).
 */
struct Gate {
  /** @brief Slot occupancy: false marks a free entry. */
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
  Slot output_fanout = 0; // derived Q fanout, for list/iface
  /** @brief Derived NQ fanout, sequential gates only. */
  Slot nq_fanout = 0;     // derived NQ fanout, sequential gates only
  /**
   * @brief V2 timing hook (docs/logic.md §12): stored now, honored
   *        later; v1 eval is zero-delay regardless of value.
   */
  std::uint64_t propagation_delay_ps = 0; // V2 timing hook (§12): stored
                                            // now, honored later; v1 eval is
                                            // zero-delay regardless of value
  /** @brief Source slot index per input; kNoDriver when unwired. */
  std::array<Slot, kMaxInputs> drivers{};

  /**
   * @brief Value equality (whole-slot compare, defaulted).
   *
   * @return True when every field of the two slots compares equal.
   */
  bool operator==(const Gate &) const noexcept = default;
};

/**
 * @brief Growable gate pool: slots recycle, counters don't.
 *
 * Backing arrays grow geometrically from kInitialSlots on demand and
 * are bounded by kMaxGates (loud refusal past it). Every scan and init
 * in the pipeline is bounded by the current pool size
 * (`slots.size()`), never by the cap — a small net costs small memory
 * and small per-op scans. Allocation order is lowest-free-slot (scan
 * in slot order, append past the end), identical to the old fixed pool
 * for identical op sequences, so deterministic behavior is preserved.
 * Mutation marks `dirty` (topology must be recompiled before eval).
 *
 * Tied inputs (LOGIC-14): input-input shorts as first-class nets. Only
 * dangling (unwired, non-ignored) inputs may be tied. Equivalence
 * classes are stored as a star: `tie_rep[flat] == 0` means singleton
 * (untied); otherwise it holds `leader_flat + 1` where leader_flat is
 * the class canonical rep (lowest slot/input wins, slot-order
 * enumeration). Zero-initialization is the all-singleton state by
 * construction; vectors value-init to zero on growth.
 *
 * Workspace scratch (`ws_*`): caller-invisible transient working
 * storage owned by the pool so no call site sizes scratch. Grown on
 * demand by the seams that need it, never serialized, excluded from
 * equality. Hot paths still perform no allocation once sized (growth
 * happens only in mutation/rebuild entry points, amortized).
 */
struct Netlist {
  /** @brief Gate slots; a dead slot is a free entry. */
  std::vector<Gate> slots{};
  /**
   * @brief Per-kind monotonic counters (never reused), indexed by
   *        kind_ordinal: 0..8 combinational (8 is TBUF), 16 DFF,
   *        17 DLATCH, 18 JK, 19 T, 20 SR (9..15 unused, stay 1).
   */
  std::array<std::uint32_t, 21> next_counter{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  /**
   * @brief Per-sink output polarity: driver_inv[flat] != 0 means the
   *        sink input reads the NEGATED source value (wire from a flop
   *        NQ output). Zero is the all-non-inverted state by
   *        construction; slot-level wire() always clears, wire_pins()
   *        sets it for NQ sources, unwire()/remove clear.
   */
  std::vector<std::uint8_t> driver_inv{};
  /**
   * @brief Tie-class star (see struct doc): `tie_rep[flat] == 0` means
   *        singleton; otherwise leader_flat + 1.
   */
  std::vector<Slot> tie_rep{};
  /**
   * @brief Shared-net mark + extra count per input (011): owned by the
   *        SharedNet seam (shared_net.hpp) — only that header may
   *        read/write these three arrays directly. Byte value 0 is
   *        unmarked (the fast test on both eval paths); bit0 is the
   *        shared mark, bits1..2 hold the extra-attachment count 0..3.
   *        Indexed by tie_flat(slot, input). Marks are persistent
   *        topology state; counts are diagnostic session state beside
   *        this pool (SolveDiag, never stored here).
   */
  std::vector<std::uint8_t> shared_meta{};
  /**
   * @brief Extra source slots per shared input (011): owned by the
   *        SharedNet seam. Dense entries 0..count-1 at
   *        flat * kMaxSharedExtra + j. The primary attachment stays in
   *        Gate::drivers (kNoDriver semantics preserved); extras never
   *        dangle behind a kNoDriver primary.
   */
  std::vector<Slot> extra_drivers{};
  /**
   * @brief Per-extra NQ polarity (011): owned by the SharedNet seam.
   *        extra_inv at the same index as extra_drivers; nonzero inverts
   *        that attachment's value before the resolve_net fold (Q+NQ
   *        self-contention resolves to X).
   */
  std::vector<std::uint8_t> extra_inv{};
  /**
   * @brief Per-input pull setting (012): 0 = none (the all-none default,
   *        today's behavior), 1 = pull-up, 2 = pull-down. Indexed by
   *        tie_flat(slot, input) — the same granularity as shared_meta.
   *        An all-floating net maps through the pull to its idle level
   *        (up → kHigh, down → kLow, none → kHighZ); driven nets are
   *        inert under every setting. Zero-initialization is the
   *        all-none state by construction; pulls die with their slot
   *        (remove_gate clears, reset clears).
   */
  std::vector<std::uint8_t> pull_cfg{};
  /** @brief Number of live gate slots. */
  std::uint32_t live_count = 0;
  /** @brief Set by every mutation; cleared by a successful topology rebuild. */
  bool dirty = true;
  /** @brief Compiled evaluation order (slot indices; Kahn output). */
  std::vector<Slot> topo_order{};
  /** @brief Per-slot level in the compiled order (0 for sources and sequential roots). */
  std::vector<Slot> topo_level{};
  /** @brief Kahn indegree workspace (pool-sized). */
  std::vector<std::uint8_t> ws_indeg{};
  /** @brief Kahn queue workspace (pool-sized). */
  std::vector<Slot> ws_queue{};
  /** @brief Kahn emission-order workspace (pool-sized). */
  std::vector<Slot> ws_order{};
  /** @brief Kahn level workspace (pool-sized). */
  std::vector<Slot> ws_levels{};
  /** @brief Resolved-value workspace (`size * kMaxInputs` entries). */
  std::vector<LogicValue> ws_resolved{};
  /** @brief Per-slot output workspace (pool-sized). */
  std::vector<LogicValue> ws_gate_out{};
  /** @brief Block-input position workspace (`size * kMaxInputs` entries). */
  std::vector<Slot> ws_in_pos{};
  /** @brief Word-eval gate-word workspace (pool-sized). */
  std::vector<std::uint64_t> ws_words{};
  /** @brief Stepper next-Q workspace (pool-sized). */
  std::vector<LogicValue> ws_next_q{};
  /** @brief Stepper async-forced-Q workspace (pool-sized). */
  std::vector<LogicValue> ws_forced_q{};
  /** @brief Stepper async-force flag workspace (pool-sized). */
  std::vector<std::uint8_t> ws_force{};
  /** @brief Stepper combinational-order workspace (pool-sized). */
  std::vector<Slot> ws_comb_order{};
  /** @brief Tie-detach survivor workspace (`size * kMaxInputs` entries). */
  std::vector<std::size_t> ws_survivors{};
  /** @brief Reachability DFS visited-flag workspace (pool-sized). */
  std::vector<std::uint8_t> ws_seen{};
  /** @brief Reachability DFS stack workspace (pool-sized). */
  std::vector<Slot> ws_stack{};

  /**
   * @brief Value equality over pool semantics (workspace excluded).
   *
   * Compares live content, counters, polarities, ties, marks, pulls,
   * counts, and the compiled topology — everything the checkpoint
   * persists. Pool vectors may differ in allocated size (a loaded pool
   * is stride-sized, a live pool is geometrically sized): the common
   * prefix compares exactly and any longer tail must be inert
   * (fresh-dead slots, zero arrays). Workspace scratch never
   * participates.
   *
   * @param[in] o Other netlist to compare against.
   * @return True when every semantic field compares equal.
   */
  [[nodiscard]] bool operator==(const Netlist &o) const noexcept {
    if (live_count != o.live_count || next_counter != o.next_counter || dirty != o.dirty ||
        topo_order != o.topo_order) {
      return false;
    }
    const std::size_t n = slots.size() < o.slots.size() ? slots.size() : o.slots.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (!(slots[i] == o.slots[i])) {
        return false;
      }
    }
    static const Gate kFresh{};
    for (std::size_t i = n; i < slots.size(); ++i) {
      if (!(slots[i] == kFresh)) {
        return false;
      }
    }
    for (std::size_t i = n; i < o.slots.size(); ++i) {
      if (!(o.slots[i] == kFresh)) {
        return false;
      }
    }
    if (!prefix_equal_zero_tail(driver_inv, o.driver_inv) || !prefix_equal_zero_tail(tie_rep, o.tie_rep) ||
        !prefix_equal_zero_tail(shared_meta, o.shared_meta) || !prefix_equal_zero_tail(extra_drivers, o.extra_drivers) ||
        !prefix_equal_zero_tail(extra_inv, o.extra_inv) || !prefix_equal_zero_tail(pull_cfg, o.pull_cfg) ||
        !prefix_equal_zero_tail(topo_level, o.topo_level)) {
      return false;
    }
    return true;
  }

 private:
  // Common-prefix equality with an all-zero tail on the longer side.
  template <typename T>
  [[nodiscard]] static bool prefix_equal_zero_tail(const std::vector<T> &a, const std::vector<T> &b) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
      if (!(a[i] == b[i])) {
        return false;
      }
    }
    for (std::size_t i = n; i < a.size(); ++i) {
      if (!(a[i] == T{})) {
        return false;
      }
    }
    for (std::size_t i = n; i < b.size(); ++i) {
      if (!(b[i] == T{})) {
        return false;
      }
    }
    return true;
  }
};

/**
 * @brief Current pool size (allocated slots, live + free).
 *
 * Every scan/init in the pipeline is bounded by this — never by
 * kMaxGates. Memory and per-op cost track the high-water mark.
 *
 * @param[in] nl Pool to measure.
 * @return Allocated slot count (0 for a fresh pool, before growth).
 */
[[nodiscard]] inline std::size_t slot_count(const Netlist &nl) noexcept {
  return nl.slots.size();
}

/**
 * @brief Flat pin index: slot * kMaxInputs + input.
 *
 * @param[in] slot Gate slot index.
 * @param[in] input 0-based input index.
 * @return Flat index into the per-input pool arrays.
 */
[[nodiscard]] inline std::size_t tie_flat(Slot slot, std::uint8_t input) noexcept {
  return static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input);
}

/**
 * @brief Grow the pool so slot `need - 1` is addressable.
 *
 * Geometric growth (doubling from kInitialSlots); vectors value-init
 * (zero == the inert state for every pool array). Never shrinks.
 * Growth past kMaxGates fails loudly (false), never silently.
 *
 * @param[out] nl Pool to grow.
 * @param[in] need Minimum addressable slot count.
 * @return True when `need` slots are addressable; false when need
 *         exceeds kMaxGates ("netlist full"). True requires no work
 *         when already sized (hot paths stay allocation-free).
 */
[[nodiscard]] inline bool ensure_slots(Netlist &nl, std::size_t need) {
  if (need <= nl.slots.size()) {
    return true;
  }
  if (need > kMaxGates) {
    return false; // loud max cap: the caller maps this to Status
  }
  std::size_t cap = nl.slots.size() * 2;
  if (cap < kInitialSlots) {
    cap = kInitialSlots;
  }
  if (cap < need) {
    cap = need;
  }
  if (cap > kMaxGates) {
    cap = kMaxGates;
  }
  nl.slots.resize(cap);
  nl.driver_inv.resize(cap * kMaxInputs);
  nl.tie_rep.resize(cap * kMaxInputs);
  nl.shared_meta.resize(cap * kMaxInputs);
  nl.extra_drivers.resize(cap * kMaxInputs * kMaxSharedExtra);
  nl.extra_inv.resize(cap * kMaxInputs * kMaxSharedExtra);
  nl.pull_cfg.resize(cap * kMaxInputs);
  nl.topo_level.resize(cap);
  return true;
}

/**
 * @brief Grow pool workspace scratch for a pool of `n` slots.
 *
 * Sizes every ws_* array for `n` addressable slots (resolved-style
 * arrays take n * kMaxInputs). No-op when already sized; never
 * shrinks. Workspace growth cannot fail past the pool itself (n is
 * always pool-bounded at call sites).
 *
 * @param[out] nl Pool whose workspace grows.
 * @param[in] n Addressable slot count the workspace must cover.
 */
inline void ensure_workspace(Netlist &nl, std::size_t n) {
  if (nl.ws_indeg.size() < n) {
    nl.ws_indeg.resize(n);
  }
  if (nl.ws_queue.size() < n) {
    nl.ws_queue.resize(n);
  }
  if (nl.ws_order.size() < n) {
    nl.ws_order.resize(n);
  }
  if (nl.ws_levels.size() < n) {
    nl.ws_levels.resize(n);
  }
  if (nl.ws_gate_out.size() < n) {
    nl.ws_gate_out.resize(n);
  }
  if (nl.ws_words.size() < n) {
    nl.ws_words.resize(n);
  }
  if (nl.ws_next_q.size() < n) {
    nl.ws_next_q.resize(n);
  }
  if (nl.ws_forced_q.size() < n) {
    nl.ws_forced_q.resize(n);
  }
  if (nl.ws_force.size() < n) {
    nl.ws_force.resize(n);
  }
  if (nl.ws_comb_order.size() < n) {
    nl.ws_comb_order.resize(n);
  }
  if (nl.ws_resolved.size() < n * kMaxInputs) {
    nl.ws_resolved.resize(n * kMaxInputs);
  }
  if (nl.ws_in_pos.size() < n * kMaxInputs) {
    nl.ws_in_pos.resize(n * kMaxInputs);
  }
  if (nl.ws_survivors.size() < n * kMaxInputs) {
    nl.ws_survivors.resize(n * kMaxInputs);
  }
  if (nl.ws_seen.size() < n) {
    nl.ws_seen.resize(n);
  }
  if (nl.ws_stack.size() < n) {
    nl.ws_stack.resize(n);
  }
}

/**
 * @brief True when the flat destination input carries a pull (012):
 *        pull_cfg[flat] is 1 (up) or 2 (down). Zero (none) and
 *        out-of-range flats read unpulled.
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] flat Flat pin index (slot * kMaxInputs + input).
 * @return True when the flat position carries a pull-up or pull-down.
 */
[[nodiscard]] inline bool is_pulled_flat(const Netlist &nl, std::size_t flat) noexcept {
  if (flat >= nl.pull_cfg.size()) {
    return false;
  }
  const std::uint8_t v = nl.pull_cfg[flat];
  return v == 1 || v == 2;
}

/**
 * @brief Pull level at a flat position (012): 1 = up, 2 = down,
 *        0 = none (or out of range).
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] flat Flat pin index (slot * kMaxInputs + input).
 * @return The stored pull level (0/1/2); never anything else.
 */
[[nodiscard]] inline std::uint8_t pull_level_flat(const Netlist &nl, std::size_t flat) noexcept {
  if (flat >= nl.pull_cfg.size()) {
    return 0;
  }
  const std::uint8_t v = nl.pull_cfg[flat];
  return (v == 1 || v == 2) ? v : 0;
}

} // namespace wengine::logic
