#pragma once

/**
 * @file
 * SharedNet seam (see GLOSSARY.md) of the logic pipeline: the multi-driver
 * representation. Only this header may read/write
 * Netlist::shared_meta, Netlist::extra_drivers, or Netlist::extra_inv
 * directly; everyone else (netlist mutation, topo, eval, optimizer
 * passes, CLI, YAML store, tests) calls the functions below.
 *
 * Bit layout (011): shared_meta[flat] byte value 0 is unmarked (the fast
 * test on both eval paths); bit0 is the shared mark, bits1..2 hold the
 * extra-attachment count 0..kMaxSharedExtra. Extra source slots live
 * dense at flat * kMaxSharedExtra + j for 0 <= j < count, with
 * per-extra NQ polarity at the same index in extra_inv. The primary
 * attachment stays in Gate::drivers (kNoDriver semantics preserved).
 *
 * Dynamic pool: every bound below is the pool's current size
 * (slot_count), never the kMaxGates loud cap — scans and inits cost
 * what the pool holds.
 *
 * Mirrors the TieSet discipline (tie_set.hpp): a widened kMaxSharedExtra
 * is a one-file change here (plus the fixed array bounds in types.hpp).
 *
 * Dependency rules (docs/logic.md section 4): header-only,
 * allocation-free, exception-free; depends only on types.hpp plus
 * core/status (never netlist.hpp/tie_set.hpp: flat arithmetic is
 * slot * kMaxInputs + input inline, so no include cycle). Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: netlist.hpp mutation, topo.hpp, block_interface.hpp, the
 * optimize passes/barrier, the CLI list/wire verbs, yaml_store.cpp, and
 * the test_logic* suite.
 */

#include "wengine/core/status.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic {

namespace shared_detail {
// Bit-layout constants: the only place that names bit positions. A
// widened kMaxSharedExtra changes these (and the fixed array bounds in
// types.hpp) — nowhere else names shifts or masks.
inline constexpr std::uint8_t kMarkBit = 0x01u;
inline constexpr unsigned kCountShift = 1u;
inline constexpr std::uint8_t kCountMask = 0x03u;
static_assert(kMaxSharedExtra <= 3, "extra count must fit bits1..2");
} // namespace shared_detail

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
  if (flat >= slot_count(nl) * kMaxInputs) {
    return false;
  }
  return (nl.shared_meta[flat] & shared_detail::kMarkBit) != 0;
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
  if (flat >= slot_count(nl) * kMaxInputs) {
    return 0;
  }
  return static_cast<std::uint8_t>((nl.shared_meta[flat] >> shared_detail::kCountShift) & shared_detail::kCountMask);
}

/**
 * @brief True when the destination input is marked as a shared
 *        (multi-driver) net (011).
 *
 * Out-of-range pins read unmarked (never shared); the mark is per
 * input, explicit, and inspectable.
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return True when the input carries the shared mark.
 */
[[nodiscard]] inline bool is_shared_input(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return false;
  }
  return is_shared_flat(nl, static_cast<std::size_t>(slot) * kMaxInputs + input);
}

/**
 * @brief Extra-attachment count on a shared input, 0..kMaxSharedExtra
 *        (011).
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return Extra count (0 when unmarked or out of range).
 */
[[nodiscard]] inline std::uint8_t shared_extra_count(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return 0;
  }
  return shared_extra_flat(nl, static_cast<std::size_t>(slot) * kMaxInputs + input);
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
[[nodiscard]] inline std::uint8_t shared_attachment_count(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs || !nl.slots[slot].live) {
    return 0;
  }
  const std::uint8_t primary = (input < nl.slots[slot].input_count && nl.slots[slot].drivers[input] != kNoDriver) ? 1 : 0;
  return static_cast<std::uint8_t>(primary + shared_extra_count(nl, slot, input));
}

/**
 * @brief Extra driver slot at index j (011).
 *
 * @param[in] nl Netlist holding the extra table.
 * @param[in] flat Flat pin index.
 * @param[in] j Extra index (must be < shared_extra_flat).
 * @return Stored driver slot, or kNoDriver when flat/j is out of range.
 */
[[nodiscard]] inline Slot shared_extra_driver(const Netlist &nl, std::size_t flat, std::uint8_t j) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs || j >= shared_extra_flat(nl, flat)) {
    return kNoDriver;
  }
  return nl.extra_drivers[flat * kMaxSharedExtra + j];
}

/**
 * @brief Per-extra NQ polarity at index j (011): nonzero inverts that
 *        attachment's value before the resolve_net fold.
 *
 * @param[in] nl Netlist holding the polarity table.
 * @param[in] flat Flat pin index.
 * @param[in] j Extra index (must be < shared_extra_flat).
 * @return Stored polarity (0 when flat/j is out of range).
 */
[[nodiscard]] inline std::uint8_t shared_extra_inv(const Netlist &nl, std::size_t flat, std::uint8_t j) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs || j >= shared_extra_flat(nl, flat)) {
    return 0;
  }
  return nl.extra_inv[flat * kMaxSharedExtra + j];
}

/**
 * @brief Visit every extra attachment on a flat input in compact index
 *        order (011): fn(j, driver, inv) for j in 0..count-1.
 *
 * The single iteration vocabulary behind topo's Kahn drain, eval's
 * span assembly, the optimizer scans, list, and YAML save — stride and
 * count math live here, nowhere else.
 *
 * @tparam Fn Callable invoked as fn(j, driver, inv).
 * @param[in] nl Netlist holding the extra tables.
 * @param[in] flat Flat pin index.
 * @param[in] fn Callable invoked as fn(j, driver, inv).
 */
template <typename Fn>
inline void for_each_extra(const Netlist &nl, std::size_t flat, Fn &&fn) noexcept {
  const std::uint8_t n = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < n; ++j) {
    fn(j, nl.extra_drivers[flat * kMaxSharedExtra + j], nl.extra_inv[flat * kMaxSharedExtra + j]);
  }
}

/**
 * @brief True when cur drives flat through any extra attachment (011).
 *
 * @param[in] nl Netlist holding the extra table.
 * @param[in] flat Flat pin index.
 * @param[in] cur Driver slot to look for.
 * @return True when some extra j holds cur.
 */
[[nodiscard]] inline bool shared_uses_driver_flat(const Netlist &nl, std::size_t flat, Slot cur) noexcept {
  const std::uint8_t n = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < n; ++j) {
    if (nl.extra_drivers[flat * kMaxSharedExtra + j] == cur) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Set the shared mark bit, preserving the extra count (011).
 *        Idempotent.
 *
 * Rep-only: validation (live/tied/range) stays at the netlist seam
 * (mark_shared); this is the single writer of the mark bit.
 *
 * @param[out] nl Netlist receiving the mark.
 * @param[in] flat Flat pin index.
 */
inline void shared_set_mark(Netlist &nl, std::size_t flat) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs) {
    return;
  }
  nl.shared_meta[flat] = static_cast<std::uint8_t>(nl.shared_meta[flat] | shared_detail::kMarkBit);
}

/**
 * @brief Set per-extra NQ polarity (011): the single writer of
 *        extra_inv entries (wire_pins NQ fixup).
 *
 * @param[out] nl Netlist holding the polarity table.
 * @param[in] flat Flat pin index.
 * @param[in] j Extra index (< current count).
 * @param[in] inv Polarity to store (nonzero = inverted).
 */
inline void shared_set_extra_inv(Netlist &nl, std::size_t flat, std::uint8_t j, std::uint8_t inv) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs || j >= shared_extra_flat(nl, flat)) {
    return;
  }
  nl.extra_inv[flat * kMaxSharedExtra + j] = inv;
}

/**
 * @brief Append one extra driver (011): cap-checked dense push plus
 *        count bump. The single writer of the count-increment path.
 *
 * Rep-only: fanout accounting and dirty marking stay at the netlist
 * seam (detail::append_shared_extra); validation (mark set, primary
 * occupied, live cycle-checked edge) stays with the caller.
 *
 * @param[out] nl Netlist receiving the attachment.
 * @param[in] flat Flat pin index.
 * @param[in] driver Driver slot to store.
 * @param[in] inv Per-attachment NQ polarity (1 = inverted).
 * @return kOk on append; kInvalidArgument past the cap or on an
 *         out-of-range flat.
 */
[[nodiscard]] inline core::Status shared_push_extra(Netlist &nl, std::size_t flat, Slot driver, std::uint8_t inv) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs) {
    return core::Status::failure(core::StatusCode::kInvalidArgument);
  }
  const std::uint8_t count = static_cast<std::uint8_t>((nl.shared_meta[flat] >> shared_detail::kCountShift) & shared_detail::kCountMask);
  if (count >= kMaxSharedExtra) {
    return core::Status::failure(core::StatusCode::kInvalidArgument); // cap 4: loud, never truncated
  }
  nl.extra_drivers[flat * kMaxSharedExtra + count] = driver;
  nl.extra_inv[flat * kMaxSharedExtra + count] = inv;
  nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & shared_detail::kMarkBit) | ((count + 1u) << shared_detail::kCountShift));
  return core::Status::success();
}

/**
 * @brief Remove the extra at compact index j, left-shifting the tail
 *        (011). The single writer of the count-decrement path.
 *
 * Rep-only: fanout release stays with the caller (which knows the
 * polarity accounting); the promoted-primary path uses
 * shared_pop_front_extra below.
 *
 * @param[out] nl Netlist holding the extras.
 * @param[in] flat Flat pin index.
 * @param[in] j Compact index to remove.
 * @return True on removal; false for out-of-range flat/j.
 */
inline bool shared_erase_at(Netlist &nl, std::size_t flat, std::uint8_t j) noexcept {
  const std::uint8_t count = shared_extra_flat(nl, flat);
  if (flat >= slot_count(nl) * kMaxInputs || j >= count || count == 0) {
    return false;
  }
  for (std::uint8_t k = j; k < static_cast<std::uint8_t>(count - 1u); ++k) {
    nl.extra_drivers[flat * kMaxSharedExtra + k] = nl.extra_drivers[flat * kMaxSharedExtra + static_cast<std::size_t>(k) + 1];
    nl.extra_inv[flat * kMaxSharedExtra + k] = nl.extra_inv[flat * kMaxSharedExtra + static_cast<std::size_t>(k) + 1];
  }
  const std::size_t tail = flat * kMaxSharedExtra + (count - 1u);
  nl.extra_drivers[tail] = 0;
  nl.extra_inv[tail] = 0;
  nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & shared_detail::kMarkBit) | ((count - 1u) << shared_detail::kCountShift));
  return true;
}

/**
 * @brief Remove the first extra holding driver (011): the named-edge
 *        release behind unwire_driver and remove_gate's sweep.
 *
 * @param[out] nl Netlist holding the extras.
 * @param[in] flat Flat pin index.
 * @param[in] driver Driver slot to release.
 * @return True when an entry was removed; false when not attached.
 */
inline bool shared_erase_driver(Netlist &nl, std::size_t flat, Slot driver) noexcept {
  const std::uint8_t count = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < count; ++j) {
    if (nl.extra_drivers[flat * kMaxSharedExtra + j] == driver) {
      return shared_erase_at(nl, flat, j);
    }
  }
  return false;
}

/**
 * @brief Pop the front extra (011): returns extra 0 and left-compacts
 *        the remainder. The promotion path behind unwire() (the promoted
 *        edge keeps its counted fanout — the source still drives this
 *        input).
 *
 * @param[out] nl Netlist holding the extras.
 * @param[in] flat Flat pin index.
 * @param[out] driver_out Front driver slot.
 * @param[out] inv_out Front polarity.
 * @return True on pop; false when count is 0 or flat is out of range.
 */
inline bool shared_pop_front(Netlist &nl, std::size_t flat, Slot &driver_out, std::uint8_t &inv_out) noexcept {
  const std::uint8_t count = shared_extra_flat(nl, flat);
  if (flat >= slot_count(nl) * kMaxInputs || count == 0) {
    return false;
  }
  driver_out = nl.extra_drivers[flat * kMaxSharedExtra + 0];
  inv_out = nl.extra_inv[flat * kMaxSharedExtra + 0];
  for (std::uint8_t j = 0; j < static_cast<std::uint8_t>(count - 1u); ++j) {
    nl.extra_drivers[flat * kMaxSharedExtra + j] = nl.extra_drivers[flat * kMaxSharedExtra + static_cast<std::size_t>(j) + 1];
    nl.extra_inv[flat * kMaxSharedExtra + j] = nl.extra_inv[flat * kMaxSharedExtra + static_cast<std::size_t>(j) + 1];
  }
  const std::size_t tail = flat * kMaxSharedExtra + (count - 1u);
  nl.extra_drivers[tail] = 0;
  nl.extra_inv[tail] = 0;
  nl.shared_meta[flat] = static_cast<std::uint8_t>((nl.shared_meta[flat] & shared_detail::kMarkBit) | ((count - 1u) << shared_detail::kCountShift));
  return true;
}

/**
 * @brief Drop every extra on a flat, zeroing entries and the count
 *        while preserving the mark (011).
 *
 * Rep-only bulk release behind remove_gate's per-input drain (the
 * caller releases each extra's fanout polarity-aware first, e.g. via
 * for_each_extra).
 *
 * @param[out] nl Netlist holding the extras.
 * @param[in] flat Flat pin index.
 */
inline void shared_clear_extras(Netlist &nl, std::size_t flat) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs) {
    return;
  }
  const std::uint8_t count = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < count; ++j) {
    nl.extra_drivers[flat * kMaxSharedExtra + j] = 0;
    nl.extra_inv[flat * kMaxSharedExtra + j] = 0;
  }
  nl.shared_meta[flat] = static_cast<std::uint8_t>(nl.shared_meta[flat] & shared_detail::kMarkBit);
}

/**
 * @brief Clear the whole shared representation (011): every mark,
 *        count, and extra entry. The reset_netlist path.
 *
 * @param[out] nl Netlist whose shared state is cleared.
 */
inline void shared_clear_all(Netlist &nl) noexcept {
  for (std::size_t f = 0; f < slot_count(nl) * kMaxInputs; ++f) {
    nl.shared_meta[f] = 0;
  }
  for (std::size_t e = 0; e < slot_count(nl) * kMaxInputs * kMaxSharedExtra; ++e) {
    nl.extra_drivers[e] = 0;
    nl.extra_inv[e] = 0;
  }
}

/**
 * @brief Clear the mark and every extra on one flat (011): the slot-death
 *        path behind remove_gate (marks die with their gate's slot).
 *
 * @param[out] nl Netlist holding the shared state.
 * @param[in] flat Flat pin index.
 */
inline void shared_clear_flat(Netlist &nl, std::size_t flat) noexcept {
  if (flat >= slot_count(nl) * kMaxInputs) {
    return;
  }
  const std::uint8_t count = shared_extra_flat(nl, flat);
  for (std::uint8_t j = 0; j < count; ++j) {
    nl.extra_drivers[flat * kMaxSharedExtra + j] = 0;
    nl.extra_inv[flat * kMaxSharedExtra + j] = 0;
  }
  nl.shared_meta[flat] = 0;
}

/**
 * @brief Structural equality of the shared representation (save/load
 *        round-trip): marks, counts, and every extra entry.
 *
 * @param[in] a First netlist.
 * @param[in] b Second netlist.
 * @return True when every shared entry agrees.
 */
[[nodiscard]] inline bool shared_equal(const Netlist &a, const Netlist &b) noexcept {
  return a.shared_meta == b.shared_meta && a.extra_drivers == b.extra_drivers && a.extra_inv == b.extra_inv;
}

} // namespace wengine::logic
