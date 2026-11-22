#pragma once

/**
 * @file
 * SkipPolicy for the logic optimizer: the one module behind every
 * pass-skip rule. Before this seam, the same freeze predicates
 * (drives_shared / has_marked_input / has_pulled_input / feeds_seq /
 * TBUF) were copied at ~10 call sites across optimize_passes.hpp and
 * optimize_o2_dedup.hpp — each new vocabulary (011 marks, 012 pulls)
 * re-spread the same guards by hand. Passes now name one composite per
 * frozen shape; new vocabulary lands here once.
 *
 * Atoms (thin aliases over the barrier seam, which stays the definition
 * site): marked_input / pulled_input / shared_driver / seq_user /
 * buffer_kind. Composites (each names one call-site shape):
 * sink_locked (no fold/drop/retag through inputs), driver_locked
 * (never rewire/remove), bypass_locked, fuse_locked, merge_locked.
 *
 * Dependency rules: header-only, allocation-free, exception-free,
 * noexcept throughout. Depends only on optimize_barrier.hpp (never
 * netlist.hpp directly — the barrier owns the predicates). Logic is a
 * standalone domain: never depends on CLI types. Consumers:
 * optimize_passes.hpp, optimize_o2_dedup.hpp.
 */

#include "wengine/logic/optimize_barrier.hpp"

namespace wengine::logic::detail::skip {

// --- Atoms (barrier aliases; the barrier stays the definition site) ---------

/**
 * @brief True when slot s has any shared-marked input (011 span).
 *
 * @param[in] nl Netlist holding the mark table.
 * @param[in] s Slot to test.
 * @return True when any configured input of s carries the mark.
 */
[[nodiscard]] inline bool marked_input(const Netlist &nl, Slot s) noexcept {
  return has_marked_input(nl, s);
}

/**
 * @brief True when slot s has any pulled input (012).
 *
 * @param[in] nl Netlist holding the pull table.
 * @param[in] s Slot to test.
 * @return True when any configured input of s carries a pull.
 */
[[nodiscard]] inline bool pulled_input(const Netlist &nl, Slot s) noexcept {
  return has_pulled_input(nl, s);
}

/**
 * @brief True when slot s drives any shared-marked input (011).
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] s Slot to test.
 * @return True when s feeds a shared input (any attachment).
 */
[[nodiscard]] inline bool shared_driver(const Netlist &nl, Slot s) noexcept {
  return drives_shared(nl, s);
}

/**
 * @brief True when a live sequential gate reads slot s (barrier bit).
 *
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *            the net has no sequential gates).
 * @param[in] s Slot to test.
 * @return True when feeds_seq names s a sequential user.
 */
[[nodiscard]] inline bool seq_user(const bool *feeds_seq, Slot s) noexcept {
  return feeds_seq != nullptr && feeds_seq[s];
}

/**
 * @brief True for the enable-gated buffer kind (load-bearing 2-input
 *        shape: index-shifting transforms would retag a fan-in-invalid
 *        1-input buffer).
 *
 * @param[in] k Kind to test.
 * @return True for kTbuf.
 */
[[nodiscard]] constexpr bool buffer_kind(GateKind k) noexcept {
  return k == GateKind::kTbuf;
}

// --- Composites (one per frozen call-site shape) -----------------------------

/**
 * @brief Sink-side freeze: no fold, drop, or retag through s's inputs
 *        (index shifts would desynchronize the flat-keyed extras and
 *        pulls — the 011/012 rationale, one predicate).
 *
 * @param[in] nl Netlist holding marks and pulls.
 * @param[in] s Slot to test.
 * @return True when s has a marked or pulled input.
 */
[[nodiscard]] inline bool sink_locked(const Netlist &nl, Slot s) noexcept {
  return marked_input(nl, s) || pulled_input(nl, s);
}

/**
 * @brief Driver-side freeze: never rewire or remove s (rewire_users
 *        only moves primaries, so shared spans would silently shrink;
 *        sequential inputs are pinned).
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] s Slot to test.
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *            the net has no sequential gates).
 * @return True when s feeds a sequential gate or a shared span.
 */
[[nodiscard]] inline bool driver_locked(const Netlist &nl, Slot s, const bool *feeds_seq) noexcept {
  return seq_user(feeds_seq, s) || shared_driver(nl, s);
}

/**
 * @brief Bypass freeze for gate s: the exact pre-seam rule (opaque
 *        barrier + 011 driver-side freeze + 012 pulled-output mapping).
 *
 * @param[in] nl Netlist holding gates, marks, and pulls.
 * @param[in] s Candidate bypass gate.
 * @param[in] feeds_seq Precomputed feeds-sequential bits (nullptr when
 *            the net has no sequential gates).
 * @return True when s must not bypass.
 */
[[nodiscard]] inline bool bypass_locked(const Netlist &nl, Slot s, const bool *feeds_seq) noexcept {
  return seq_user(feeds_seq, s) || shared_driver(nl, s) || pulled_input(nl, s);
}

/**
 * @brief Fusion freeze for the (NOT s, driver h) pair: the exact
 *        pre-seam rule (011 driver/sink pair + 012 pulled pair — the
 *        driver-array copy moves primaries without their flat-keyed
 *        pulls and extras).
 *
 * @param[in] nl Netlist holding marks and pulls.
 * @param[in] s Fusing NOT gate.
 * @param[in] h Fusion driver.
 * @return True when the pair must not fuse.
 */
[[nodiscard]] inline bool fuse_locked(const Netlist &nl, Slot s, Slot h) noexcept {
  return shared_driver(nl, s) || marked_input(nl, s) || marked_input(nl, h) || pulled_input(nl, s) ||
         pulled_input(nl, h);
}

/**
 * @brief Dedup-merge freeze for the (keeper s, victim t) pair: never
 *        merge through a shared span (unreachable today — O2 refuses
 *        marked netlists upfront — but structural).
 *
 * @param[in] nl Netlist holding drivers and extras.
 * @param[in] s Merge keeper (lower slot).
 * @param[in] t Merge victim (higher slot).
 * @return True when the pair must not merge.
 */
[[nodiscard]] inline bool merge_locked(const Netlist &nl, Slot s, Slot t) noexcept {
  return shared_driver(nl, t) || shared_driver(nl, s);
}

} // namespace wengine::logic::detail::skip
