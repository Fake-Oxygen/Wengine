#pragma once

/**
 * @file
 * InputResolution seam (see GLOSSARY.md) of the logic pipeline: the single
 * owner of "value at (slot, input)" span assembly.
 *
 * One net idea previously had three parallel implementations with
 * different failure vocabularies:
 * `resolve_input` (scalar, loud) and `resolve_word` (bit-parallel) in
 * `block_interface.hpp`, plus `timing_resolve_input` (X/Z-transparent) in
 * `timing.hpp`. Polarity, the pull-map, and the SharedNet span fold were
 * re-coded per path; drift was already visible (inverted-Z folding in the
 * scalar shared span vs the 4-state-preserving transient read; pulled-net
 * branches in different orders; the word path ignoring extras/dead
 * drivers entirely).
 *
 * This header owns, once:
 * - span assembly (primary-first, compact index order, verbatim stored
 *   slots/polarities plus an explicit dead flag),
 * - per-attachment polarity (4-state-preserving; agrees with both
 *   `apply_polarity` and `timing_nq_of` on Low/High),
 * - the pull-map (all-floating span through the destination pull to
 *   idle; driven spans inert),
 * - contention detection (low-vs-high present, independent of the
 *   funnel's X logic),
 * - the ignored-pin predicate.
 *
 * Three narrow adapters consume the seam and keep their own failure
 * vocabularies by design (never unified):
 * - ScalarResolver (loud): dead/dangling-extra/X/Z refuse with `false`;
 * - WordResolver (bit-parallel): single-word fold with bitwise NOT;
 *   nets the single bit cannot carry (>= 2 attachments, live TBUF) are
 *   refused at the word-loop entries via `shared_word_blocked` /
 *   `has_buffer_gates`, never inside the lane fold;
 * - TransientResolver (X/Z-transparent): dead entries are transparent,
 *   X/Z flow through (`HighZ` at a gate input reads as `Unknown`).
 *
 * Capacity (015): dynamic pool — every bound is `slot_count(nl)`, never
 * `kMaxGates`. Allocation-free, exception-free, header-only.
 *
 * Dependency rules (docs/logic.md sections 4, 8): depends only on
 * `types.hpp` plus `shared_net.hpp` (never `netlist.hpp`/`tie_set.hpp`:
 * flat arithmetic is `slot * kMaxInputs + input` inline, so no include
 * cycle). Logic is a standalone domain: never CLI types. Consumers:
 * `block_interface.hpp` (scalar + word adapters), `timing.hpp`
 * (transient adapter), and the `test_logic_input_resolution` property
 * test pinning 0/1 agreement across all three adapters.
 */

#include "wengine/logic/shared_net.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>

namespace wengine::logic::input_res {

/**
 * @brief Value-agnostic driver span for one sink input (011/012).
 *
 * Primary-first, compact index order: entry 0 is the primary driver
 * (when present), entries 1..n-1 are shared extras in stored order.
 * Slots and polarities are verbatim stored values — liveness is
 * reported, never filtered, so each adapter applies its own dead
 * policy loudly (scalar), transparently (transient), or via the
 * topology refusal (word, whose entry is pre-validated).
 */
struct InputSpan {
  /** @brief Stored driver slots (primary first, then extras). */
  Slot drv[1 + kMaxSharedExtra]{};
  /** @brief Stored per-attachment NQ polarity (nonzero = inverted). */
  std::uint8_t inv[1 + kMaxSharedExtra]{};
  /** @brief Attachment count (0..1+kMaxSharedExtra). */
  std::size_t n = 0;
  /** @brief True when any stored driver is out-of-range or dead. */
  bool has_dead = false;
  /** @brief True when the destination input carries the shared mark. */
  bool is_shared = false;
  /** @brief Flat pin index (slot * kMaxInputs + input). */
  std::size_t flat = 0;
};

/**
 * @brief Assemble the driver span for one sink input, once.
 *
 * The single span-assembly vocabulary behind all three adapters:
 * shared destinations contribute the primary (with `driver_inv`) plus
 * every extra (with `extra_inv`) in compact index order; unmarked
 * destinations contribute the primary alone. Stored slots/polarities
 * are copied verbatim; dead/out-of-range drivers set `has_dead` but
 * stay in the span so adapters decide loudly vs transparently.
 *
 * @param[in] nl Netlist holding drivers, marks, and polarities.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @param[out] out Assembled span (verbatim slots/invs + dead flag).
 */
inline void assemble_input_span(const Netlist &nl, Slot slot, std::uint8_t input, InputSpan &out) noexcept {
  out.n = 0;
  out.has_dead = false;
  out.is_shared = false;
  out.flat = tie_flat(slot, input);
  const std::size_t nslots = slot_count(nl);
  if (slot >= nslots || input >= kMaxInputs) {
    out.has_dead = true;
    return;
  }
  const std::size_t flat = out.flat;
  const bool shared = is_shared_flat(nl, flat);
  out.is_shared = shared;
  const Gate &g = nl.slots[slot];
  if (!g.live) {
    out.has_dead = true;
    return;
  }
  if (shared) {
    const Slot primary = (input < g.input_count) ? g.drivers[input] : kNoDriver;
    if (primary != kNoDriver) {
      std::uint8_t pinv = 0;
      if (flat < nl.driver_inv.size()) {
        pinv = nl.driver_inv[flat];
      }
      out.drv[out.n] = primary;
      out.inv[out.n] = pinv;
      ++out.n;
      if (primary >= nslots || !nl.slots[primary].live) {
        out.has_dead = true;
      }
    }
    for_each_extra(nl, flat, [&](std::uint8_t, Slot edrv, std::uint8_t einv) {
      if (out.n >= 1 + kMaxSharedExtra) {
        return;
      }
      out.drv[out.n] = edrv;
      out.inv[out.n] = einv;
      ++out.n;
      if (edrv >= nslots || !nl.slots[edrv].live) {
        out.has_dead = true;
      }
    });
    return;
  }
  // Unmarked: the primary alone (extras never attach without the mark;
  // defensively ignored here even if present).
  const Slot primary = (input < g.input_count) ? g.drivers[input] : kNoDriver;
  if (primary == kNoDriver) {
    return;
  }
  std::uint8_t pinv = 0;
  if (flat < nl.driver_inv.size()) {
    pinv = nl.driver_inv[flat];
  }
  out.drv[0] = primary;
  out.inv[0] = pinv;
  out.n = 1;
  if (primary >= nslots || !nl.slots[primary].live) {
    out.has_dead = true;
  }
}

/**
 * @brief Per-attachment polarity, 4-state-preserving (011/012).
 *
 * The single polarity vocabulary behind all three adapters. On
 * Low/High agrees exactly with both `apply_polarity` (scalar) and
 * `timing_nq_of` (transient); on X/HighZ passes the value through
 * (the old scalar span folded inverted-Z to High — the drift this
 * seam fixes; the funnel treats Z as transparent and X as dominant,
 * and only consumers give the sunk value meaning).
 *
 * @param[in] v Attachment value.
 * @param[in] inv Nonzero inverts Low<->High, passes X/HighZ through.
 * @return Polarized value.
 */
[[nodiscard]] inline LogicValue apply_span_polarity(LogicValue v, std::uint8_t inv) noexcept {
  if (inv == 0) {
    return v;
  }
  if (v == LogicValue::kHigh) {
    return LogicValue::kLow;
  }
  if (v == LogicValue::kLow) {
    return LogicValue::kHigh;
  }
  return v;
}

/**
 * @brief Bit-parallel twin of `apply_span_polarity`: NQ inversion in
 *        the word domain.
 *
 * @param[in] w 64-lane word.
 * @param[in] inv Nonzero inverts (bitwise NOT), zero passes through.
 * @return Polarized word.
 */
[[nodiscard]] inline std::uint64_t apply_word_polarity(std::uint64_t w, std::uint8_t inv) noexcept {
  return (inv != 0) ? ~w : w;
}

/**
 * @brief Pull-map for one sunk net value, owned once (012).
 *
 * An all-floating span (`kHighZ` after the funnel) resolves through
 * the destination input's pull to its idle level (up -> `kHigh`,
 * down -> `kLow`, none -> `kHighZ`). Driven spans (agreement,
 * contention, unknown) are inert under every pull setting.
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] flat Flat pin index of the destination input.
 * @param[in] sunk Funnel output to map.
 * @return Idle level for floating+pulled nets, otherwise `sunk`.
 */
[[nodiscard]] inline LogicValue pull_map_idle(const Netlist &nl, std::size_t flat, LogicValue sunk) noexcept {
  if (sunk == LogicValue::kHighZ && is_pulled_flat(nl, flat)) {
    return (pull_level_flat(nl, flat) == 1) ? LogicValue::kHigh : LogicValue::kLow;
  }
  return sunk;
}

/**
 * @brief Word-domain pull-map: idle word for a pulled destination.
 *
 * The bit-parallel twin of `pull_map_idle` for the terminated-dangling
 * case (012): pull-up reads the all-ones word, pull-down the all-zero
 * word. Call only when `is_pulled_flat(nl, flat)` holds.
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] flat Flat pin index of the destination input.
 * @return All-ones for pull-up, all-zero for pull-down/none.
 */
[[nodiscard]] inline std::uint64_t pull_idle_word(const Netlist &nl, std::size_t flat) noexcept {
  return (pull_level_flat(nl, flat) == 1) ? ~static_cast<std::uint64_t>(0ULL) : static_cast<std::uint64_t>(0ULL);
}

/**
 * @brief Contention detection for one assembled value span, owned
 *        once (011).
 *
 * A span presenting both polarized low and high increments
 * `SolveDiag::per_net[flat]` by exactly one — computed at span
 * assembly, independent of the funnel's X logic (future X/Z-presenting
 * drivers must not false-count).
 *
 * @param[in] span Polarized attachment values.
 * @param[in] n Span length.
 * @return True when the span presents both Low and High.
 */
[[nodiscard]] inline bool span_has_contention(const LogicValue *span, std::size_t n) noexcept {
  bool saw_low = false;
  bool saw_high = false;
  for (std::size_t k = 0; k < n; ++k) {
    saw_low = saw_low || (span[k] == LogicValue::kLow);
    saw_high = saw_high || (span[k] == LogicValue::kHigh);
  }
  return saw_low && saw_high;
}

/**
 * @brief Ignored-pin predicate for one sink input, owned once.
 *
 * @param[in] nl Netlist holding the ignore mask.
 * @param[in] slot Gate slot of the sink.
 * @param[in] input 0-based sink input index.
 * @return True when the pin is live, configured, and ignored.
 */
[[nodiscard]] inline bool input_is_ignored(const Netlist &nl, Slot slot, std::uint8_t input) noexcept {
  if (slot >= slot_count(nl) || input >= kMaxInputs) {
    return false;
  }
  const Gate &g = nl.slots[slot];
  if (!g.live || input >= g.input_count) {
    return false;
  }
  return (g.ignored_mask & static_cast<std::uint8_t>(1u << input)) != 0;
}

} // namespace wengine::logic::input_res
