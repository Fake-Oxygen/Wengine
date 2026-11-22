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
 * Dynamic pool: every mutation entry works over Netlist with pool-owned
 * workspace scratch; growth is geometric with loud refusal past
 * kMaxGates, and every bound is the pool's current size (slot_count),
 * never kMaxGates. The text adapters (wire_text/tie_text/untie_text),
 * remove_gate_by_name, and the pin table paths stay text-bound — the
 * scale harness builds by slot directly.
 *
 * Dependency rules (docs/logic.md section 4): depends on
 * block_interface.hpp, pins.hpp, shared_net.hpp, tie_set.hpp, topo.hpp,
 * types.hpp, and core/status. Logic is a standalone domain: this header
 * must never depend on CLI types. Shared representation (shared_meta /
 * extra_drivers / extra_inv) is owned by shared_net.hpp — this header
 * calls that seam and never names bit shifts, masks, or strides.
 * Consumers: the CLI add/remove/wire/ignore verbs, eval.hpp,
 * seq_step.hpp, yaml_store.hpp, the optimize pipeline, and the
 * test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/adjacency.hpp"
#include "wengine/logic/block_interface.hpp"
#include "wengine/logic/pins.hpp"
#include "wengine/logic/pool_store.hpp"
#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/tie_set.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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
[[nodiscard]] inline core::Status unwire(Netlist &nl, Slot to_slot, std::uint8_t to_input) noexcept;
[[nodiscard]] inline core::Status unwire_driver(Netlist &nl, Slot from_slot, Slot to_slot, std::uint8_t to_input) noexcept;

/**
 * @brief Add a gate with explicit fan-in; lowest free slot wins
 *        (deterministic), per-kind counter assigns then increments, never reused.
 *
 * @param[out] nl Netlist receiving the gate.
 * @param[in] kind Gate kind to create.
 * @param[in] inputs Fan-in; must satisfy valid_fanin(kind, inputs).
 * @return kOk with the new slot on success; kInvalidArgument for bad fan-in, an unknown kind, or growth past kMaxGates ("netlist full").
 */
[[nodiscard]] inline core::Result<Slot> add_gate(Netlist &nl, GateKind kind, std::uint8_t inputs) {
  if (!valid_fanin(kind, inputs)) {
    return core::Result<Slot>::failure("bad fan-in for kind");
  }
  const std::uint8_t ord = kind_ordinal(kind);
  if (ord > 8 && !is_sequential_kind(kind)) {
    // No such kind exists today (ordinals are 0..8 + 16..20); the guard
    // stays so future appended kinds fail loudly until wired up.
    return core::Result<Slot>::failure("bad gate kind");
  }
  if (nl.next_counter[ord] == 0) {
    nl.next_counter[ord] = 1; // tolerate zero-init (scaffold default)
  }
  // Lowest free slot wins (016): the free-slot heap answers exactly
  // what the slot-order scan answered — deterministic, and identical
  // op sequences allocate identically. ensure_slots indexes fresh
  // slots, so a heap miss here means genuine growth (or a full pool).
  Slot slot = detail::heap_pop_free(nl);
  if (slot == kNoDriver) {
    // Pool full: grow geometrically (amortized); loud refusal past the
    // max cap, never silent. The appended slot is the old size — every
    // slot below it was live or recycled, so this is the lowest free
    // slot by construction, and allocation order matches the old fixed
    // pool for identical op sequences.
    const std::size_t old = slot_count(nl);
    if (!ensure_slots(nl, old + 1)) {
      return core::Result<Slot>::failure("netlist full");
    }
    slot = detail::heap_pop_free(nl);
    if (slot == kNoDriver) {
      return core::Result<Slot>::failure("netlist full"); // defensive: growth left no free slot
    }
  }
  Gate &g = nl.slots[slot];
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
  ++nl.live_count;
  pool_store::note_edge_event(nl);
  return core::Result<Slot>::success(slot);
}

/**
 * @brief Add a gate with the kind's default fan-in.
 *
 * @param[out] nl Netlist receiving the gate.
 * @param[in] kind Gate kind to create.
 * @return kOk with the new slot on success; kInvalidArgument for a full
 *         netlist ("netlist full").
 */
[[nodiscard]] inline core::Result<Slot> add_gate(Netlist &nl, GateKind kind) {
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
 * @param[in] survivors Caller-owned scratch (pool-size * kMaxInputs flats)
 *            for the tie-detach survivor sweep.
 * @return kOk on success; kInvalidArgument for out-of-range or dead
 *         slots, or undersized scratch.
 */
[[nodiscard]] inline core::Status remove_gate(Netlist &nl, Slot slot, std::span<std::size_t> survivors) noexcept {
  if (slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.slots[slot];
  if (!g.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (survivors.size() < slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Detach ties first (while input_count is still valid): each tied pin
  // of this gate leaves its class via the TieSet seam; survivors re-elect.
  detach_slot_ties(nl, slot, survivors);
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const Slot drv = g.drivers[i];
    if (drv != kNoDriver && drv < slot_count(nl) && nl.slots[drv].live) {
      const std::size_t flat = tie_flat(slot, i);
      // Adjacency (016): release the primary edge into this slot.
      (void)adjacency::adj_unlink_edge(nl, drv, slot, i, 0);
      // Feeder counters (016): the released edge left a marked input.
      if (is_shared_flat(nl, flat) && nl.feeds_shared[drv] > 0) {
        --nl.feeds_shared[drv];
      }
      if (nl.driver_inv[flat] != 0) {
        nl.driver_inv[flat] = 0;
        if (nl.slots[drv].nq_fanout > 0) {
          --nl.slots[drv].nq_fanout;
        }
      } else if (nl.slots[drv].output_fanout > 0) {
        --nl.slots[drv].output_fanout;
      }
    }
    g.drivers[i] = kNoDriver;
    // Release every extra attachment on this input (polarity-aware) and
    // drop the mark: marks die with their gate's slot.
    const std::size_t flat = tie_flat(slot, i);
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t einv) {
      if (edrv < slot_count(nl) && nl.slots[edrv].live) {
        // Adjacency (016): release this extra edge into this slot.
        (void)adjacency::adj_unlink_edge(nl, edrv, slot, i, 1);
        // Feeder counters (016): extras only exist on marked inputs.
        if (nl.feeds_shared[edrv] > 0) {
          --nl.feeds_shared[edrv];
        }
        if (einv != 0) {
          if (nl.slots[edrv].nq_fanout > 0) {
            --nl.slots[edrv].nq_fanout;
          }
        } else if (nl.slots[edrv].output_fanout > 0) {
          --nl.slots[edrv].output_fanout;
        }
      }
    });
    shared_clear_flat(nl, flat);
  }
  // Release every edge this gate drives (016): walk the removed
  // gate's recorded successors. The head is re-read per edge because
  // unwire may relink via promotion; every iteration unlinks at least
  // one of the slot's chain edges, so the walk terminates with every
  // driven edge released at fanout-proportional cost — no full-pool
  // sweep.
  while (slot < nl.succ_head.size() && nl.succ_head[slot] != kNoDriver) {
    const Slot e = nl.succ_head[slot];
    const Slot s = nl.succ_sink[e];
    const std::uint8_t i = nl.succ_input[e];
    const std::uint8_t x = nl.succ_extra[e];
    if (x != 0) {
      // Extra attachment of the removed gate: release exactly that
      // edge (left-compact; remaining drivers resolve unchanged).
      (void)unwire_driver(nl, slot, s, i);
    } else {
      // Primary attachment of the removed gate: unwire promotes extra
      // 0 when the input holds more drivers (mark preserved).
      (void)unwire(nl, s, i);
    }
  }
  g.live = false;
  g.ignored_mask = 0;
  g.ignore_output = false;
  g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
  g.output_fanout = 0;
  g.nq_fanout = 0;
  // Adjacency (016): the freed slot recycles through the heap.
  detail::heap_push_free(nl, slot);
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
  pool_store::note_edge_event(nl);
  return core::Status::success();
}

/**
 * @brief Remove a gate.
 *
 * Workspace-backed entry: grows the tie-detach survivor scratch to the
 * pool size, then runs the core above. (No noexcept: workspace growth
 * may allocate.)
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] slot Slot to remove.
 * @return kOk on success; kInvalidArgument for out-of-range or dead slots.
 */
[[nodiscard]] inline core::Status remove_gate(Netlist &nl, Slot slot) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return remove_gate(nl, slot, std::span<std::size_t>(nl.ws_survivors.data(), n * kMaxInputs));
}

/**
 * @brief Remove a gate by display name (e.g. "AND1").
 *
 * Stays alias-monomorphic (text path): the scale harness and tests
 * remove by slot directly.
 *
 * @param[out] nl Netlist holding the gate.
 * @param[in] name Display name resolved via resolve_gate.
 * @return kOk on success; kInvalidArgument when the name does not resolve to a live gate.
 */
[[nodiscard]] inline core::Status remove_gate_by_name(Netlist &nl, std::string_view name) noexcept {
  auto gate = resolve_gate(nl, name);
  if (!gate.ok()) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  return remove_gate(nl, gate.value);
}

// Shared slot vocabulary (is_shared_input, shared_extra_count,
// shared_attachment_count) lives in the SharedNet seam
// (shared_net.hpp); this header calls it and never touches the
// representation directly.

namespace detail {

// --- Shared-feeder counters (016, US6) --------------------------------------
// feeds_shared[driver] counts the driver's live edges landing on
// shared-marked inputs. Owned by the PoolStore seam (pool_store.hpp) —
// these forwarders keep the logic::detail::feeds_* names working for the
// mutation paths below; bulk writers recompute via feeds_recompute.

/**
 * @brief Decrement marked-input drivers of one sink (before its rewrite).
 *
 * @param[out] nl Netlist holding feeder counters.
 * @param[in] sink Sink slot being rewritten.
 */
inline void feeds_release_sink(Netlist &nl, Slot sink) noexcept {
  pool_store::detail::feeds_release_sink(nl, sink);
}

/**
 * @brief Increment marked-input drivers of one sink (after its rewrite).
 *
 * @param[out] nl Netlist holding feeder counters.
 * @param[in] sink Sink slot just rewritten.
 */
inline void feeds_acquire_sink(Netlist &nl, Slot sink) noexcept {
  pool_store::detail::feeds_acquire_sink(nl, sink);
}

} // namespace detail

/**
 * @brief Recompute every shared-feeder counter from the tables
 *        (bulk-load/build path). Cold paths only.
 *
 * Forwarder to the PoolStore seam (pool_store::recompute_feeds): the one
 * bulk rebuild is pool_store::rebuild_all().
 *
 * @param[out] nl Netlist whose counters rebuild.
 */
inline void feeds_recompute(Netlist &nl) noexcept {
  pool_store::recompute_feeds(nl);
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
[[nodiscard]] inline core::Status append_shared_extra(Netlist &nl, Slot from_slot, Slot to_slot, std::uint8_t to_input, std::uint8_t inv) noexcept {
  const std::size_t flat = tie_flat(to_slot, to_input);
  const core::Status st = shared_push_extra(nl, flat, from_slot, inv);
  if (!st.ok()) {
    return st; // cap: loud, never truncated
  }
  // Adjacency (016): the extra is a driver-graph edge exactly like a
  // primary (pool-sized capacity: infallible by construction).
  if (!adjacency::adj_insert_edge(nl, from_slot, to_slot, to_input, 1)) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  Gate &from = nl.slots[from_slot];
  // Feeder counters (016): extras only exist on marked inputs.
  ++nl.feeds_shared[from_slot];
  if (inv != 0) {
    ++from.nq_fanout;
  } else {
    ++from.output_fanout;
  }
  pool_store::note_edge_event(nl);
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
[[nodiscard]] inline core::Status mark_shared(Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.slots[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, slot, input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  const std::size_t flat = tie_flat(slot, input);
  if (is_shared_flat(nl, flat)) {
    return core::Status::success(); // idempotent: counters already count this span
  }
  shared_set_mark(nl, flat);
  // Feeder counters (016): pre-existing attachments now land on a marked
  // input (marking an occupied input is legal — the CLI --shared path).
  const Slot drv = nl.slots[slot].drivers[input];
  if (drv != kNoDriver && drv < slot_count(nl) && nl.slots[drv].live) {
    ++nl.feeds_shared[drv];
  }
  for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t) {
    if (edrv < slot_count(nl) && nl.slots[edrv].live) {
      ++nl.feeds_shared[edrv];
    }
  });
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
[[nodiscard]] inline core::Status unshare_input(Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.slots[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::size_t flat = tie_flat(slot, input);
  if (!is_shared_flat(nl, flat)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // not shared
  }
  if (shared_extra_flat(nl, flat) != 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // unwire extras first
  }
  // Feeder counters (016): a remaining primary edge stops landing on a
  // marked input (at most one attachment may exist here).
  const Slot drv = nl.slots[slot].drivers[input];
  if (drv != kNoDriver && drv < slot_count(nl) && nl.slots[drv].live && nl.feeds_shared[drv] > 0) {
    --nl.feeds_shared[drv];
  }
  shared_clear_flat(nl, flat);
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
[[nodiscard]] inline core::Status set_pull(Netlist &nl, Slot slot, std::uint8_t input, std::uint8_t level) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.slots[slot].input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (level != 1 && level != 2) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (is_tied(nl, slot, input)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // tied: untie first
  }
  if ((nl.slots[slot].ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0) {
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
[[nodiscard]] inline core::Status clear_pull(Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (input >= nl.slots[slot].input_count) {
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
 * @brief Set the propagation-delay annotation on a gate (013): whole picoseconds of stored response lag.
 *
 * Stored now, honored later: no evaluation, stepping, truth-table,
 * export, or draw path reads this field (docs/logic.md §12 — the
 * delay-honoring evaluator is the named follow-on). Legal on every
 * live gate kind — combinational and sequential alike; per-kind timing
 * semantics belong to that future evaluator, so the seam performs no
 * kind check. Idempotent: re-setting replaces the value (last write
 * wins, deterministic). Zero is a legal write equal to clearing.
 *
 * @param[out] nl Netlist receiving the annotation.
 * @param[in] slot Gate slot.
 * @param[in] value_ps Whole picoseconds to store.
 * @return kOk on set; kInvalidArgument for dead/out-of-range slots. The netlist is untouched on failure.
 */
[[nodiscard]] inline core::Status set_gate_delay(Netlist &nl, Slot slot, std::uint64_t value_ps) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  nl.slots[slot].propagation_delay_ps = value_ps;
  return core::Status::success();
}

/**
 * @brief Clear the propagation-delay annotation on a gate (013).
 *
 * Refused loudly when the stored value is zero (the `clear_pull`
 * precedent: an un-annotated gate has nothing to clear). The netlist is
 * untouched on failure.
 *
 * @param[out] nl Netlist holding the annotation.
 * @param[in] slot Gate slot.
 * @return kOk on clear; kInvalidArgument for dead/out-of-range slots or un-annotated (zero-valued) gates.
 */
[[nodiscard]] inline core::Status clear_gate_delay(Netlist &nl, Slot slot) noexcept {
  if (slot >= slot_count(nl) || !nl.slots[slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (nl.slots[slot].propagation_delay_ps == 0) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // no delay set
  }
  nl.slots[slot].propagation_delay_ps = 0;
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
 * @param[in] reach_seen Caller-owned scratch (pool-sized visited flags) for
 *            the incremental cycle check.
 * @param[in] reach_stack Caller-owned scratch (pool-sized slots) for the
 *            incremental cycle check.
 * @return kOk on success; kInvalidArgument for dead/out-of-range
 *         slots or inputs, occupied unmarked inputs, tied destinations
 *         (untie first), shared-cap overflow, cycle-closing wires, or
 *         undersized scratch. The netlist is untouched on failure.
 *         Fan-out is unlimited.
 */
[[nodiscard]] inline core::Status wire(Netlist &nl, Slot from_slot, Slot to_slot, std::uint8_t to_input, std::span<std::uint8_t> reach_seen,
                                       std::span<Slot> reach_stack) noexcept {
  if (from_slot >= slot_count(nl) || to_slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &from = nl.slots[from_slot];
  Gate &to = nl.slots[to_slot];
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
  // validate via topo::query(kCombOnly), which excludes exactly these edges), so the
  // incremental check is vacuous there and acceptance never depends on
  // wiring order. The whole-graph validator stays authoritative for
  // combinational destinations (a live-but-uninvolved flop shields
  // nothing).
  if (!is_sequential_kind(to.kind) && topo::reaches(nl, to_slot, from_slot, EdgeFilter::kCombOnly, reach_seen, reach_stack)) {
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
  // Feeder counters (016): the new edge lands on a marked input.
  if (shared_marked) {
    ++nl.feeds_shared[from_slot];
  }
  // Adjacency (016): primary edge (probe ran above on the pre-wire
  // chains; pool-sized capacity: infallible by construction).
  if (!adjacency::adj_insert_edge(nl, from_slot, to_slot, to_input, 0)) {
    return core::Status::failure(core::StatusCode::kInternal);
  }
  pool_store::note_edge_event(nl);
  return core::Status::success();
}

/**
 * @brief Single-driver wire, with the shared-net opt-in (011).
 *
 * Workspace-backed entry: grows the cycle-check scratch to the pool
 * size, then runs the core above. (No noexcept: workspace growth may
 * allocate.)
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
[[nodiscard]] inline core::Status wire(Netlist &nl, Slot from_slot, Slot to_slot, std::uint8_t to_input) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return wire(nl, from_slot, to_slot, to_input, std::span<std::uint8_t>(nl.ws_seen.data(), n), std::span<Slot>(nl.ws_stack.data(), n));
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
[[nodiscard]] inline core::Status unwire(Netlist &nl, Slot to_slot, std::uint8_t to_input) noexcept {
  if (to_slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &to = nl.slots[to_slot];
  if (!to.live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const Slot drv = to.drivers[to_input];
  if (drv == kNoDriver) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  to.drivers[to_input] = kNoDriver;
  // Adjacency (016): release the primary edge (pool-sized: unlink first
  // so the promotion insert below reuses the freed slot).
  (void)adjacency::adj_unlink_edge(nl, drv, to_slot, to_input, 0);
  if (drv < slot_count(nl) && nl.slots[drv].live) {
    // Polarity-aware release: NQ edges decrement nq_fanout, Q edges
    // decrement output_fanout.
    const std::size_t flat = tie_flat(to_slot, to_input);
    // Feeder counters (016): the released edge left a marked input.
    // (Promotion below is edge-neutral: extra out, primary in.)
    if (is_shared_flat(nl, flat) && nl.feeds_shared[drv] > 0) {
      --nl.feeds_shared[drv];
    }
    if (nl.driver_inv[flat] != 0) {
      nl.driver_inv[flat] = 0;
      if (nl.slots[drv].nq_fanout > 0) {
        --nl.slots[drv].nq_fanout;
      }
    } else if (nl.slots[drv].output_fanout > 0) {
      --nl.slots[drv].output_fanout;
    }
    // Promote extra 0 into the primary slot (pinned order: attachment
    // sequences in different orders converge to identical array
    // states). The promoted edge keeps its counted fanout — the source
    // still drives this input.
    if (shared_extra_flat(nl, flat) > 0) {
      Slot promoted = kNoDriver;
      std::uint8_t promoted_inv = 0;
      if (shared_pop_front(nl, flat, promoted, promoted_inv)) {
        to.drivers[to_input] = promoted;
        nl.driver_inv[flat] = promoted_inv;
        // Adjacency (016): the promoted edge moves from extra to
        // primary (same driver and sink, flag flips).
        (void)adjacency::adj_unlink_edge(nl, promoted, to_slot, to_input, 1);
        (void)adjacency::adj_insert_edge(nl, promoted, to_slot, to_input, 0);
      }
    }
  }
  pool_store::note_edge_event(nl);
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
[[nodiscard]] inline core::Status unwire_driver(Netlist &nl, Slot from_slot, Slot to_slot, std::uint8_t to_input) noexcept {
  if (from_slot >= slot_count(nl) || to_slot >= slot_count(nl)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &to = nl.slots[to_slot];
  if (!to.live || !nl.slots[from_slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to_input >= to.input_count) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (to.drivers[to_input] == from_slot) {
    return unwire(nl, to_slot, to_input); // primary edge (promotion included)
  }
  const std::size_t flat = tie_flat(to_slot, to_input);
  const std::uint8_t count = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < count; ++j) {
    if (shared_extra_driver(nl, flat, j) != from_slot) {
      continue;
    }
    // Polarity-aware release of exactly this edge.
    const std::uint8_t einv = shared_extra_inv(nl, flat, j);
    if (einv != 0) {
      if (nl.slots[from_slot].nq_fanout > 0) {
        --nl.slots[from_slot].nq_fanout;
      }
    } else if (nl.slots[from_slot].output_fanout > 0) {
      --nl.slots[from_slot].output_fanout;
    }
    shared_erase_at(nl, flat, j);
    // Adjacency (016): release exactly this extra edge.
    (void)adjacency::adj_unlink_edge(nl, from_slot, to_slot, to_input, 1);
    // Feeder counters (016): extras only exist on marked inputs.
    if (nl.feeds_shared[from_slot] > 0) {
      --nl.feeds_shared[from_slot];
    }
    pool_store::note_edge_event(nl);
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
 * @param[in] reach_seen Caller-owned scratch (pool-sized visited flags) for
 *            the incremental cycle check.
 * @param[in] reach_stack Caller-owned scratch (pool-sized slots) for the
 *            incremental cycle check.
 * @return kOk on success; kInvalidArgument for input-as-source pins, dead slots, NQ sources on combinational gates, occupied or
 *         tied destinations, cycle-closing wires, or undersized scratch.
 */
[[nodiscard]] inline core::Status wire_pins(Netlist &nl, PinRef from, PinRef to, std::span<std::uint8_t> reach_seen, std::span<Slot> reach_stack) noexcept {
  if (!from.is_output || to.is_output) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  if (from.slot >= slot_count(nl) || !nl.slots[from.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // NQ sources exist on sequential gates only (parse_pin gates this, but
  // slot-built refs bypass text — stay loud, never silent).
  if (from.negated && !is_sequential_kind(nl.slots[from.slot].kind)) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  // Shared append detection before the mutation: wire() lands the edge
  // as the last extra on marked occupied inputs (Q fanout, inv 0).
  const bool shared_append = is_shared_input(nl, to.slot, to.input) && nl.slots[to.slot].drivers[to.input] != kNoDriver;
  const core::Status st = wire(nl, from.slot, to.slot, to.input, reach_seen, reach_stack);
  if (!st.ok() || !from.negated) {
    return st;
  }
  if (shared_append) {
    // Move the just-appended extra from the Q fanout to the NQ fanout
    // and mark it inverted (wire() always establishes non-inverted).
    Gate &src = nl.slots[from.slot];
    if (src.output_fanout > 0) {
      --src.output_fanout;
    }
    ++src.nq_fanout;
    const std::size_t flat = tie_flat(to.slot, to.input);
    const std::uint8_t count = shared_extra_flat(nl, flat);
    shared_set_extra_inv(nl, flat, static_cast<std::uint8_t>(count - 1u), 1);
    return core::Status::success();
  }
  // Move the edge from the Q fanout to the NQ fanout and mark the sink
  // inverted (wire() always establishes the non-inverted state).
  Gate &src = nl.slots[from.slot];
  if (src.output_fanout > 0) {
    --src.output_fanout;
  }
  ++src.nq_fanout;
  nl.driver_inv[tie_flat(to.slot, to.input)] = 1;
  return core::Status::success();
}

/**
 * @brief Wire output pin from to input pin to, with NQ polarity.
 *
 * Workspace-backed entry: grows the cycle-check scratch to the pool
 * size, then runs the core above. (No noexcept: workspace growth may
 * allocate.)
 *
 * @param[out] nl Netlist receiving the wire.
 * @param[in] from Output pin (NQ sources exist on sequential gates only).
 * @param[in] to Input pin.
 * @return kOk on success; kInvalidArgument for input-as-source pins, dead slots, NQ sources on combinational gates, occupied or
 *         tied destinations, or cycle-closing wires.
 */
[[nodiscard]] inline core::Status wire_pins(Netlist &nl, PinRef from, PinRef to) {
  const std::size_t n = slot_count(nl);
  ensure_workspace(nl, n);
  return wire_pins(nl, from, to, std::span<std::uint8_t>(nl.ws_seen.data(), n), std::span<Slot>(nl.ws_stack.data(), n));
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
 * Stays alias-monomorphic (text path): the scale harness and tests
 * wire by slot directly.
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
  if (pin.slot >= slot_count(nl) || !nl.slots[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.slots[pin.slot];
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
  if (pin.slot >= slot_count(nl) || !nl.slots[pin.slot].live) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  Gate &g = nl.slots[pin.slot];
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
  for (std::size_t s = 0; s < slot_count(nl); ++s) {
    Gate &g = nl.slots[s];
    g.live = false;
    g.kind = GateKind::kAnd;
    g.counter = 0;
    g.input_count = 2;
    g.ignored_mask = 0;
    g.ignore_output = false;
    g.ignore_nq_output = true; // fresh-state default (NQ hidden unless `use`d)
    g.output_fanout = 0;
    g.nq_fanout = 0;
    nl.feeds_shared[s] = 0; // derived counters die with the slot
    g.propagation_delay_ps = 0;
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      g.drivers[i] = kNoDriver;
    }
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      nl.driver_inv[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
      nl.pull_cfg[static_cast<std::size_t>(s) * kMaxInputs + i] = 0;
    }
    // Shared representation clears through its owning seam.
    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      shared_clear_flat(nl, static_cast<std::size_t>(s) * kMaxInputs + i);
    }
  }
  reset_all_ties(nl);
  for (std::size_t i = 0; i < nl.next_counter.size(); ++i) {
    nl.next_counter[i] = 1;
  }
  // Derived stores (016): every edge is gone; one seam clears the
  // chains, tie member index, free-slot heap, and feeder counters (O(pool)
  // like the slot loop above — reset is already a wholesale pass).
  pool_store::clear_derived(nl);
  nl.live_count = 0;
  pool_store::note_edge_event(nl);
}

/**
 * @brief Parse two input-pin spellings and tie them (text adapter over
 *        the TieSet seam).
 *
 * Stays alias-monomorphic (text path).
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
 * Stays alias-monomorphic (text path).
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
