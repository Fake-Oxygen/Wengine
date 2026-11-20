#pragma once

/**
 * @file
 * Netlist mutation stage (LOGIC-2) of the logic pipeline: pins plus the derived
 * block interface. Header-only, allocation-free, exception-free;
 * fallible ops use core::Status/Result (literals only, no heap).
 * Determinism: free-slot search and interface derivation both scan in
 * slot order; display ids are per-kind monotonic counters never reused
 * (slots recycle). Pin vocabulary lives in types.hpp (so tie_set.hpp
 * can take pins without a netlist.hpp include cycle); pin/kind parsing
 * lives in pins.hpp (the table module); tie math plus block-interface
 * derivation live in tie_set.hpp / block_interface.hpp. This header owns
 * mutation.
 *
 * Ignored-pin rule (v1): `ignore` excludes a dangling pin from the
 * derived block interface. Ignored dangling *inputs* tie off to kLow
 * during eval; wired inputs ignore the mark (the wire wins). `use`
 * clears the mark. Both are idempotent.
 *
 * Dependency rules (docs/logic.md section 4): depends on
 * block_interface.hpp, pins.hpp, tie_set.hpp, topo.hpp, types.hpp, and
 * core/status. Logic is a standalone domain: this header must never
 * depend on CLI types. Consumers: the CLI add/remove/wire/ignore verbs,
 * eval.hpp, seq_step.hpp, yaml_store.hpp, the optimize pipeline, and
 * the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace wengine::logic {

// Pin vocabulary lives in types.hpp (so tie_set.hpp can take pins without
// including this header); pin/kind parsing lives in pins.hpp (the C2
// table Module); tie math + block-interface derivation live in
// tie_set.hpp / block_interface.hpp. This header owns mutation.

// Forward declarations: remove_gate sweeps shared attachments through the
// named-edge removal seam defined below (promotion/compaction keep the
// shared span dense).
[[nodiscard]] inline core::Status unwire(Netlist &nl, std::uint16_t to_slot, std::uint8_t to_input) noexcept;
[[nodiscard]] inline core::Status unwire_driver(Netlist &nl, std::uint16_t from_slot, std::uint16_t to_slot, std::uint8_t to_input) noexcept;

/**
 * @brief Add a gate with explicit fan-in; lowest free slot wins
 *        (deterministic), per-kind counter assigns then increments, never reused.
 *
 * @param[out] nl Netlist receiving the gate.
 * @param[in] kind Gate kind to create.
 * @param[in] inputs Fan-in; must satisfy valid_fanin(kind, inputs).
 * @return kOk with the new slot on success; kInvalidArgument for bad fan-in, an unknown kind, or a full netlist ("netlist full").
 */
[[nodiscard]] inline core::Result<std::uint16_t> add_gate(Netlist &nl, GateKind kind, std::uint8_t inputs) noexcept {
  if (!valid_fanin(kind, inputs)) {
    return core::Result<std::uint16_t>::failure("bad fan-in for kind");
  }
  const std::uint8_t ord = kind_ordinal(kind);
  if (ord > 8 && !is_sequential_kind(kind)) {
    // No such kind exists today (ordinals are 0..8 + 16..20); the guard
    // stays so future appended kinds fail loudly until wired up.
    return core::Result<std::uint16_t>::failure("bad gate kind");
  }
  if (nl.next_counter[ord] == 0) {
    nl.next_counter[ord] = 1; // tolerate zero-init (scaffold default)
  }
  std::uint16_t slot = kNoDriver;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      slot = s;
      break;
    }
  }
  if (slot == kNoDriver) {
    return core::Result<std::uint16_t>::failure("netlist full");
  }
  Gate &g = nl.gates[slot];
  g.live = true;
  g.kind = kind;
  g.counter = nl.next_counter[ord]++;
  if (nl.next_counter[ord] == 0) { // u32 wrap: skip 0 (0 never assigned)
    nl.next_counter[ord] = 1;
  }
  g.input_count = inputs;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = is_sequential_kind(kind); // NQ hidden unless `use`d
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.propagation_delay_ps = 0;
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
    // tie_rep entries are already 0 by the remove/reset invariant
    // (detach_slot_ties clears on remove, reset_all_ties on reset).
  }
  if (nl.live_count < kMaxGates) {
    ++nl.live_count;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Result<std::uint16_t>::success(slot);
}

/**
 * @brief Add a gate with the kind's default fan-in.
 *
 * @param[out] nl Netlist receiving the gate.
 * @param[in] kind Gate kind to create.
 * @return kOk with the new slot on success; kInvalidArgument for a full
 *         netlist ("netlist full").
 */
[[nodiscard]] inline core::Result<std::uint16_t> add_gate(Netlist &nl, GateKind kind) noexcept {
  return add_gate(nl, kind, default_fanin(kind));
}

/**
 * @brief Remove a gate: detaches its inputs (releasing driver fanout)
 *        and clears every input it drives elsewhere.
 *
 * Counters never reused; slot recycles. Tied pins of the removed gate
 * leave their classes (remaining members re-elect the lowest surviving
 * pin as leader). The gate's own pulls die with its slot (like shared
 * marks).
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] slot Slot to remove.
 * @return kOk on success; kInvalidArgument for out-of-range or dead
 *         slots.
 */
[[nodiscard]] inline core::Status remove_gate(Netlist &nl, std::uint16_t slot) noexcept {
  if (slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[slot];
  if (!g.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Detach ties first (while input_count is still valid): each tied pin
  // of this gate leaves its class via the TieSet seam; survivors re-elect.
  detach_slot_ties(nl, slot);
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const std::uint16_t drv = g.drivers[i];
    if (drv != kNoDriver && drv < kMaxGates && nl.gates[drv].live) {
      const std::size_t flat = tie_flat(slot, i);
      if (nl.driver_inv[flat] != 0) {
        nl.driver_inv[flat] = 0;
        if (nl.gates[drv].nq_fanout > 0) {
          --nl.gates[drv].nq_fanout;
        }
      } else if (nl.gates[drv].output_fanout > 0) {
        --nl.gates[drv].output_fanout;
      }
    }
    g.drivers[i] = kNoDriver;
    // Release every extra attachment on this input (polarity-aware) and
    // drop the mark: marks die with their gate's slot.
    const std::size_t flat = tie_flat(slot, i);
    const std::uint8_t extra_n = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
    for (std::uint8_t j = 0; j < extra_n; ++j) {
      const std::size_t eii = flat * kMaxSharedExtra + j;
      const std::uint16_t edrv = nl.extra_drivers[eii];
      if (edrv < kMaxGates && nl.gates[edrv].live) {
        if (nl.extra_inv[eii] != 0) {
          if (nl.gates[edrv].nq_fanout > 0) {
            --nl.gates[edrv].nq_fanout;
          }
        } else if (nl.gates[edrv].output_fanout > 0) {
          --nl.gates[edrv].output_fanout;
        }
      }
      nl.extra_drivers[eii] = 0;
      nl.extra_inv[eii] = 0;
    }
    nl.shared_meta[flat] = 0;
  }
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    Gate &o = nl.gates[s];
    if (!o.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < o.input_count; ++i) {
      if (o.drivers[i] == slot) {
        // Primary attachment of the removed gate: unwire promotes extra
        // 0 when the input holds more drivers (mark preserved).
        (void)unwire(nl, s, i);
      } else {
        // Extra attachment of the removed gate: release exactly that
        // edge (left-compact; remaining drivers resolve unchanged).
        const std::size_t flat = tie_flat(s, i);
        const std::uint8_t extra_n = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
        for (std::uint8_t j = 0; j < extra_n; ++j) {
          if (nl.extra_drivers[flat * kMaxSharedExtra + j] == slot) {
            (void)unwire_driver(nl, slot, s, i);
            break; // one named edge per (from, to, input) scan position
          }
        }
      }
    }
  }
  g.live = false;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
  g.output_fanout = 0;
  g.nq_fanout = 0;
  g.input_count = default_fanin(g.kind);
  for (std::size_t i = 0; i < kMaxInputs; ++i) {
    g.drivers[i] = kNoDriver;
    nl.driver_inv[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0;
    nl.pull_cfg[tie_flat(slot, static_cast<std::uint8_t>(i))] = 0; // pulls die with their slot
    // tie entries already cleared by detach_slot_ties above.
  }
  if (nl.live_count > 0) {
    --nl.live_count;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

/**
 * @brief Remove a gate by display name (e.g. "AND1").
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] name Display name resolved via resolve_gate.
 * @return kOk on success; kInvalidArgument when the name does not
 *         resolve to a live gate.
 */
[[nodiscard]] inline core::Status remove_gate_by_name(Netlist &nl, std::string_view name) noexcept {
  auto gate = resolve_gate(nl, name);
  if (!gate.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return remove_gate(nl, gate.value);
}

/**
 * @brief True when the destination input is marked as a shared (multi-driver) net (011): bit0 of shared_meta[flat].
 *
 * Out-of-range pins read unmarked (never shared); the mark is per
 * input, explicit, and inspectable.
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return True when the input carries the shared mark.
 */
[[nodiscard]] inline bool is_shared_input(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || input >= kMaxInputs) {
    return false;
  }
  return is_shared_flat(nl, tie_flat(slot, input));
}

/**
 * @brief Extra-attachment count on a shared input, 0..kMaxSharedExtra (011): bits1..2 of shared_meta[flat].
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return Extra count (0 when unmarked or out of range).
 */
[[nodiscard]] inline std::uint8_t shared_extra_count(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || input >= kMaxInputs) {
    return 0;
  }
  return shared_extra_flat(nl, tie_flat(slot, input));
}

/**
 * @brief Total attachments on an input (011): the primary driver (when
 *        present) plus extras. A shared net legally holds 0..4 across
 *        its lifetime; 0 attachments resolve kHighZ.
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return Attachment count (0 when unwired/out of range).
 */
[[nodiscard]] inline std::uint8_t shared_attachment_count(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || input >= kMaxInputs || !nl.gates[slot].live) {
    return 0;
  }
  const std::uint8_t primary = (input < nl.gates[slot].input_count && nl.gates[slot].drivers[input] != kNoDriver) ? 1 : 0;
  return static_cast<std::uint8_t>(primary + shared_extra_count(nl, slot, input));
}

namespace detail {

/**
 * @brief Append one extra driver to a marked, occupied input (011).
 *
 * Caller-owned validation: the mark is set, the primary is occupied,
 * and the edge passed the same live/range/untied/cycle checks as any
 * wire. Cap overflow fails loudly, never truncates.
 *
 * @param[out] nl Netlist receiving the attachment.
 * @param[in] from_slot Driver slot.
 * @param[in] to_slot Sink slot.
 * @param[in] to_input 0-based sink input index.
 * @param[in] inv Per-attachment NQ polarity (1 = inverted).
 * @return kOk on append; kInvalidArgument past the cap.
 */
[[nodiscard]] inline core::Status append_shared_extra(Netlist &nl, std::uint16_t from_slot,
                                                      std::uint16_t to_slot, std::uint8_t to_input,
                                                      std::uint8_t inv) noexcept {
  const std::size_t flat = tie_flat(to_slot, to_input);
  const std::uint8_t count = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
  if (count >= kMaxSharedExtra) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // cap 4: loud, never truncated
  }
  nl.extra_drivers[flat * kMaxSharedExtra + count] = from_slot;
  nl.extra_inv[flat * kMaxSharedExtra + count] = inv;
  nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & 0x01u) | ((count + 1u) << 1));
  Gate &from = nl.gates[from_slot];
  if (inv != 0) {
    ++from.nq_fanout;
  } else {
    ++from.output_fanout;
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

} // namespace detail

/**
 * @brief Mark a destination input as a shared (multi-driver) net (011): the explicit per-input opt-in lifting the single-driver rule.
 *
 * Legal on unoccupied inputs (pre-marking) and idempotent on marked
 * ones. Refused on tied inputs (ties never join a driver span) and on
 * dead/out-of-range pins. Marks are persistent topology state and die
 * with their gate's slot.
 *
 * @param[out] nl Netlist receiving the mark.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return kOk on mark; kInvalidArgument for dead/out-of-range pins or tied destinations.
 */
[[nodiscard]] inline core::Status mark_shared(Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || !nl.gates[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.gates[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, slot, input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  nl.shared_meta[tie_flat(slot, input)] |= 0x01u; // idempotent
  return core::Status::success();
}

/**
 * @brief Clear the shared mark on an input (011).
 *
 * Refused loudly while the input holds more than one attachment
 * (unwire down to at most one driver first) and on unmarked inputs.
 * The attachment set is untouched — only the mark clears.
 *
 * @param[out] nl Netlist holding the mark.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return kOk on unmark; kInvalidArgument for dead/out-of-range pins,
 *         unmarked inputs, or inputs holding >= 2 attachments.
 */
[[nodiscard]] inline core::Status unshare_input(Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || !nl.gates[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.gates[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t flat = tie_flat(slot, input);
  if ((nl.shared_meta[flat] & 0x01u) == 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // not shared
  }
  if (((nl.shared_meta[flat] >> 1) & 0x03u) != 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // unwire extras first
  }
  nl.shared_meta[flat] = 0;
  return core::Status::success();
}

/**
 * @brief Set the pull on a destination input (012): 1 = pull-up,
 *        2 = pull-down. An all-floating net maps through the pull to
 *        its idle level; driven nets are inert under every setting.
 *
 * Legal on wired inputs (any attachment count, marked or not) and on
 * dangling inputs (termination). Idempotent: re-setting the same level
 * succeeds; re-setting the other level replaces it. Refused loudly on
 * tied inputs ("untie first") and on ignored inputs ("use first") —
 * pulls and ties/ignores are disjoint per flat in both directions.
 *
 * @param[out] nl Netlist receiving the pull.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @param[in] level Pull level: 1 (up) or 2 (down).
 * @return kOk on set; kInvalidArgument for dead/out-of-range pins,
 *         tied or ignored destinations, or levels other than 1/2.
 *         The netlist is untouched on failure.
 */
[[nodiscard]] inline core::Status set_pull(Netlist &nl, std::uint16_t slot, std::uint8_t input, std::uint8_t level) noexcept {
  if (slot >= kMaxGates || !nl.gates[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.gates[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (level != 1 && level != 2) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, slot, input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  if ((nl.gates[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // ignored: use first
  }
  nl.pull_cfg[tie_flat(slot, input)] = level;
  return core::Status::success();
}

/**
 * @brief Clear the pull on a destination input (012).
 *
 * Refused loudly when no pull is set. The netlist is untouched on
 * failure.
 *
 * @param[out] nl Netlist holding the pull.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return kOk on clear; kInvalidArgument for dead/out-of-range pins or
 *         unpulled inputs.
 */
[[nodiscard]] inline core::Status clear_pull(Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  if (slot >= kMaxGates || !nl.gates[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.gates[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t flat = tie_flat(slot, input);
  if (!is_pulled_flat(nl, flat)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // no pull set
  }
  nl.pull_cfg[flat] = 0;
  return core::Status::success();
}

/**
 * @brief Single-driver wire, with the shared-net opt-in (011).
 *
 * Exactly one wire per destination input — unless that input is marked
 * shared, in which case the edge appends as an extra attachment (up to
 * kMaxSharedExtra extras; overflow is loud, never truncated). The
 * occupied-input refusal on unmarked inputs is byte-identical to the
 * pre-011 rule.
 *
 * An edge into a sequential-gate input can never close a combinational
 * cycle (flop Q outputs break loops), so the incremental check is
 * vacuous there and acceptance never depends on wiring order. The
 * whole-graph validator stays authoritative for combinational
 * destinations (a live-but-uninvolved flop shields nothing).
 *
 * @param[out] nl Netlist receiving the wire.
 * @param[in] from_slot Driver slot.
 * @param[in] to_slot Sink slot.
 * @param[in] to_input 0-based sink input index.
 * @return kOk on success; kInvalidArgument for dead/out-of-range
 *         slots or inputs, occupied unmarked inputs, tied destinations
 *         (untie first), shared-cap overflow, or cycle-closing wires.
 *         The netlist is untouched on failure. Fan-out is unlimited.
 */
[[nodiscard]] inline core::Status wire(Netlist &nl, std::uint16_t from_slot, std::uint16_t to_slot, std::uint8_t to_input) noexcept {
  if (from_slot >= kMaxGates || to_slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &from = nl.gates[from_slot];
  Gate &to = nl.gates[to_slot];
  if (!from.live || !to.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const bool shared_marked = is_shared_input(nl, to_slot, to_input);
  if (to.drivers[to_input] != kNoDriver && !shared_marked) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // occupied
  }
  if (is_tied(nl, to_slot, to_input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  // Q-boundary: an edge into a sequential-gate input can never close a
  // combinational cycle (flop Q outputs break loops — see
  // validate via topo::validate(kCombOnly), which excludes exactly these edges), so the
  // incremental check is vacuous there and acceptance never depends on
  // wiring order. The whole-graph validator stays authoritative for
  // combinational destinations (a live-but-uninvolved flop shields
  // nothing).
  if (!is_sequential_kind(to.kind) && topo::reaches(nl, to_slot, from_slot, EdgeFilter::kCombOnly)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // cycle
  }
  if (to.drivers[to_input] != kNoDriver) {
    // Shared append: the mark is the prior explicit opt-in; every
    // attachment passes the same validation above. Never truncates.
    return detail::append_shared_extra(nl, from_slot, to_slot, to_input, 0);
  }
  to.drivers[to_input] = from_slot;
  ++from.output_fanout;
  nl.driver_inv[tie_flat(to_slot, to_input)] = 0; // slot-wire is never inverted
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

/**
 * @brief Remove the wire driving one input (polarity-aware: NQ edges decrement nq_fanout, Q edges decrement output_fanout).
 *
 * On a shared input holding extras, removing the primary promotes
 * extra 0 (slot + polarity) into drivers[]/driver_inv and left-compacts
 * the remainder — the mark is untouched, so a marked input legally
 * holds 0..cap attachments across its lifetime.
 *
 * @param[out] nl Netlist holding the wire.
 * @param[in] to_slot Sink slot.
 * @param[in] to_input 0-based sink input index.
 * @return kOk on success; kInvalidArgument for dead/out-of-range slots or inputs, or an already-unwired input.
 */
[[nodiscard]] inline core::Status unwire(Netlist &nl, std::uint16_t to_slot, std::uint8_t to_input) noexcept {
  if (to_slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &to = nl.gates[to_slot];
  if (!to.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::uint16_t drv = to.drivers[to_input];
  if (drv == kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  to.drivers[to_input] = kNoDriver;
  if (drv < kMaxGates && nl.gates[drv].live) {
    // Polarity-aware release: NQ edges decrement nq_fanout, Q edges
    // decrement output_fanout.
    const std::size_t flat = tie_flat(to_slot, to_input);
    if (nl.driver_inv[flat] != 0) {
      nl.driver_inv[flat] = 0;
      if (nl.gates[drv].nq_fanout > 0) {
        --nl.gates[drv].nq_fanout;
      }
    } else if (nl.gates[drv].output_fanout > 0) {
      --nl.gates[drv].output_fanout;
    }
    // Promote extra 0 into the primary slot (pinned order: attachment
    // sequences in different orders converge to identical array
    // states). The promoted edge keeps its counted fanout — the source
    // still drives this input.
    const std::uint8_t count = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
    if (count > 0) {
      to.drivers[to_input] = nl.extra_drivers[flat * kMaxSharedExtra + 0];
      nl.driver_inv[flat] = nl.extra_inv[flat * kMaxSharedExtra + 0];
      for (std::uint8_t j = 0; j < static_cast<std::uint8_t>(count - 1u); ++j) {
        nl.extra_drivers[flat * kMaxSharedExtra + j] = nl.extra_drivers[flat * kMaxSharedExtra + static_cast<std::size_t>(j) + 1];
        nl.extra_inv[flat * kMaxSharedExtra + j] = nl.extra_inv[flat * kMaxSharedExtra + static_cast<std::size_t>(j) + 1];
      }
      const std::size_t tail = flat * kMaxSharedExtra + (count - 1u);
      nl.extra_drivers[tail] = 0;
      nl.extra_inv[tail] = 0;
      nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & 0x01u) | ((count - 1u) << 1));
    }
  }
  nl.dirty = true;
  nl.topo_size = 0;
  return core::Status::success();
}

/**
 * @brief Remove one named edge onto a shared input (011): the exact
 *        attachment from from_slot, wherever it sits.
 *
 * Removing the primary delegates to unwire() (promotion included);
 * removing extra j releases its fanout polarity-aware and left-compacts
 * (pinned order). Removing an edge that is not attached fails loudly.
 * No dangling entries survive; remaining drivers resolve unchanged.
 *
 * @param[out] nl Netlist holding the wire.
 * @param[in] from_slot Driver slot of the edge to release.
 * @param[in] to_slot Sink slot.
 * @param[in] to_input 0-based sink input index.
 * @return kOk on release; kInvalidArgument for dead/out-of-range pins
 *         or an edge that is not attached.
 */
[[nodiscard]] inline core::Status unwire_driver(Netlist &nl, std::uint16_t from_slot, std::uint16_t to_slot, std::uint8_t to_input) noexcept {
  if (from_slot >= kMaxGates || to_slot >= kMaxGates) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &to = nl.gates[to_slot];
  if (!to.live || !nl.gates[from_slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to.drivers[to_input] == from_slot) {
    return unwire(nl, to_slot, to_input); // primary edge (promotion included)
  }
  const std::size_t flat = tie_flat(to_slot, to_input);
  const std::uint8_t count = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
  for (std::uint8_t j = 0; j < count; ++j) {
    const std::size_t eij = flat * kMaxSharedExtra + j;
    if (nl.extra_drivers[eij] != from_slot) {
      continue;
    }
    // Polarity-aware release of exactly this edge.
    if (nl.extra_inv[eij] != 0) {
      if (nl.gates[from_slot].nq_fanout > 0) {
        --nl.gates[from_slot].nq_fanout;
      }
    } else if (nl.gates[from_slot].output_fanout > 0) {
      --nl.gates[from_slot].output_fanout;
    }
    for (std::uint8_t k = j; k < static_cast<std::uint8_t>(count - 1u); ++k) {
      nl.extra_drivers[flat * kMaxSharedExtra + k] = nl.extra_drivers[flat * kMaxSharedExtra + static_cast<std::size_t>(k) + 1];
      nl.extra_inv[flat * kMaxSharedExtra + k] = nl.extra_inv[flat * kMaxSharedExtra + static_cast<std::size_t>(k) + 1];
    }
    const std::size_t tail = flat * kMaxSharedExtra + (count - 1u);
    nl.extra_drivers[tail] = 0;
    nl.extra_inv[tail] = 0;
    nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & 0x01u) | ((count - 1u) << 1));
    nl.dirty = true;
    nl.topo_size = 0;
    return core::Status::success();
  }
  return core::Status::failure(core::StatusCode::kInvalidArgument); // edge not attached
}

/**
 * @brief Wire output pin from to input pin to, with NQ polarity.
 *
 * Moves the edge from the Q fanout to the NQ fanout and marks the sink
 * inverted (wire() always establishes the non-inverted state).
 *
 * @param[out] nl Netlist receiving the wire.
 * @param[in] from Output pin (NQ sources exist on sequential gates only).
 * @param[in] to Input pin.
 * @return kOk on success; kInvalidArgument for input-as-source pins, dead slots, NQ sources on combinational gates, occupied or
 *         tied destinations, or cycle-closing wires.
 */
[[nodiscard]] inline core::Status wire_pins(Netlist &nl, PinRef from, PinRef to) noexcept {
  if (!from.is_output || to.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (from.slot >= kMaxGates || !nl.gates[from.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // NQ sources exist on sequential gates only (parse_pin gates this, but
  // slot-built refs bypass text — stay loud, never silent).
  if (from.negated && !is_sequential_kind(nl.gates[from.slot].kind)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Shared append detection before the mutation: wire() lands the edge
  // as the last extra on marked occupied inputs (Q fanout, inv 0).
  const bool shared_append = is_shared_input(nl, to.slot, to.input) && nl.gates[to.slot].drivers[to.input] != kNoDriver;
  const core::Status st = wire(nl, from.slot, to.slot, to.input);
  if (!st.ok() || !from.negated) {
    return st;
  }
  if (shared_append) {
    // Move the just-appended extra from the Q fanout to the NQ fanout
    // and mark it inverted (wire() always establishes non-inverted).
    Gate &src = nl.gates[from.slot];
    if (src.output_fanout > 0) {
      --src.output_fanout;
    }
    ++src.nq_fanout;
    const std::size_t flat = tie_flat(to.slot, to.input);
    const std::uint8_t count = static_cast<std::uint8_t>((nl.shared_meta[flat] >> 1) & 0x03u);
    nl.extra_inv[flat * kMaxSharedExtra + (count - 1u)] = 1;
    return core::Status::success();
  }
  // Move the edge from the Q fanout to the NQ fanout and mark the sink
  // inverted (wire() always establishes the non-inverted state).
  Gate &src = nl.gates[from.slot];
  if (src.output_fanout > 0) {
    --src.output_fanout;
  }
  ++src.nq_fanout;
  nl.driver_inv[tie_flat(to.slot, to.input)] = 1;
  return core::Status::success();
}

/**
 * @brief Remove the wire driving one input pin.
 *
 * @param[out] nl Netlist holding the wire.
 * @param[in] pin Input pin to unwire.
 * @return kOk on success; kInvalidArgument for output pins or an
 *         already-unwired input.
 */
[[nodiscard]] inline core::Status unwire_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return unwire(nl, pin.slot, pin.input);
}

/**
 * @brief Parse two pin spellings and wire output to input.
 *
 * @param[out] nl Netlist receiving the wire.
 * @param[in] from_text Output pin spelling (e.g. "AND2.Y").
 * @param[in] to_text Input pin spelling (e.g. "NOR1.1").
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         wire_pins refusal.
 */
[[nodiscard]] inline core::Status wire_text(Netlist &nl, std::string_view from_text, std::string_view to_text) noexcept {
  auto from = parse_pin(nl, from_text);
  if (!from.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto to = parse_pin(nl, to_text);
  if (!to.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return wire_pins(nl, from.value, to.value);
}

/**
 * @brief Ignore a pin: exclude it from the derived block interface.
 *
 * Idempotent: marking twice still succeeds. Ignoring a tied input
 * fails loudly (untie first); tied pins are never ignored by
 * construction. Ignoring a pulled input fails loudly (unpull first);
 * pulls and ignores are disjoint per flat in both directions.
 *
 * @param[out] nl Netlist holding the pin.
 * @param[in] pin Pin to ignore (inputs mark the ignored bit, outputs
 *            mark ignore_output / ignore_nq_output).
 * @return kOk on success; kInvalidArgument for dead/out-of-range pins
 *         or tied inputs.
 */
[[nodiscard]] inline core::Status ignore_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[pin.slot];
  if (pin.is_output) {
    if (pin.negated) {
      g.ignore_nq_output = true;
    } else {
      g.ignore_output = true;
    }
    return core::Status::success();
  }
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, pin.slot, pin.input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  if (is_pulled_flat(nl, tie_flat(pin.slot, pin.input))) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // pulled: unpull first
  }
  g.ignored_mask = static_cast<std::uint8_t>(g.ignored_mask | (1u << pin.input));
  return core::Status::success();
}

/**
 * @brief Re-expose an ignored pin to the derived block interface.
 *
 * Idempotent: clearing an unmarked pin still succeeds.
 *
 * @param[out] nl Netlist holding the pin.
 * @param[in] pin Pin to re-expose.
 * @return kOk on success; kInvalidArgument for dead/out-of-range pins.
 */
[[nodiscard]] inline core::Status use_pin(Netlist &nl, PinRef pin) noexcept {
  if (pin.slot >= kMaxGates || !nl.gates[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.gates[pin.slot];
  if (pin.is_output) {
    if (pin.negated) {
      g.ignore_nq_output = false;
    } else {
      g.ignore_output = false;
    }
    return core::Status::success();
  }
  if (pin.input >= g.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  g.ignored_mask =
      static_cast<std::uint8_t>(g.ignored_mask & ~(1u << pin.input));
  return core::Status::success();
}

/**
 * @brief Clear the whole netlist to the fresh state: every slot dead, ties cleared, per-kind counters restarted at 1.
 *
 * @param[out] nl Netlist to reset.
 */
inline void reset_netlist(Netlist &nl) noexcept {
  for (std::size_t s = 0; s < kMaxGates; ++s) {
    Gate &g = nl.gates[s];
    g.live = false;
    g.kind = GateKind::kAnd;
    g.counter = 0;
    g.input_count = 2;
    g.ignored_mask = 0;
    g.ignore_output = false;
    g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
    g.output_fanout = 0;
    g.nq_fanout = 0;
    g.propagation_delay_ps = 0;
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      g.drivers[i] = kNoDriver;
    }
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      nl.driver_inv[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
      nl.pull_cfg[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
      nl.shared_meta[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
      for (std::size_t j = 0; j < kMaxSharedExtra; ++j) {
        const std::size_t eij = (static_cast<std::size_t>(s) * kMaxInputs + i) * kMaxSharedExtra + j;
        nl.extra_drivers[eij] = 0;
        nl.extra_inv[eij] = 0;
      }
    }
  }
  reset_all_ties(nl);
  for (std::size_t i = 0; i < nl.next_counter.size(); ++i) {
    nl.next_counter[i] = 1;
  }
  nl.live_count = 0;
  nl.dirty = true;
  nl.topo_size = 0;
}

/**
 * @brief Parse two input-pin spellings and tie them (text adapter over
 *        the TieSet seam).
 *
 * @param[out] nl Netlist receiving the tie.
 * @param[in] a_text First pin spelling.
 * @param[in] b_text Second pin spelling.
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         tie_pins refusal.
 */
[[nodiscard]] inline core::Status tie_text(Netlist &nl, std::string_view a_text, std::string_view b_text) noexcept {
  auto a = parse_pin(nl, a_text);
  if (!a.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  auto b = parse_pin(nl, b_text);
  if (!b.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return tie_pins(nl, a.value, b.value);
}

/**
 * @brief Parse one input-pin spelling and untie it.
 *
 * @param[out] nl Netlist receiving the untie.
 * @param[in] text Pin spelling.
 * @return kOk on success; kInvalidArgument for unparsable pins or any
 *         untie_pin refusal.
 */
[[nodiscard]] inline core::Status untie_text(Netlist &nl, std::string_view text) noexcept {
  auto p = parse_pin(nl, text);
  if (!p.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return untie_pin(nl, p.value);
}

} // namespace wengine::logic
