#pragma once

/**
 * @file
 * Cross-thread cancel plus progress hooks for the TUI async worker.
 * The CLI owns these atoms (cli to solvers, never the reverse): `sim run`
 * and `sig run` chunked loops poll cancel_requested() at step boundaries
 * and report progress_* each chunk, so the ncurses loop stays responsive
 * while a worker thread runs Shell::execute. Headless runs never set the
 * flag, so their behavior is unchanged (fast batches simply check an
 * always-false atomic). All state is atomic (thread-safe); signal
 * handlers set their own separate flags while the combined stop predicate
 * reads both. No I/O here; the UI thread reads progress_* while the
 * worker writes. Consumers: the `sim`/`sig` batch drivers and the TUI
 * async layer. Dependency direction: cli to solvers, never the reverse.
 */

// Cross-thread cancel + progress hooks for the TUI async worker (TUI-5).
//
// The CLI owns these atoms (cli -> solvers, never the reverse): `sim run`
// and `sig run` chunked loops poll `cancel_requested()` at step boundaries
// and report `progress_*` each chunk, so the ncurses loop stays responsive
// while a worker thread runs `Shell::execute`. Headless runs never set the
// flag, so their behavior is unchanged (fast batches simply check an
// always-false atomic).
//
// Thread-safety: all state is `std::atomic` (TSAN-clean). Signal handlers
// set their own `sig_atomic_t` flags; the combined stop predicate reads
// both. No I/O here; progress display reads `progress_*` from the UI
// thread only.

#include <atomic>
#include <cstdint>

namespace wengine::cli {

// --- cancel ---------------------------------------------------------------

/**
 * @brief Cancel flag polled by chunked `sim run` / `sig run` loops.
 * @note Relaxed atomic; headless runs never set it (always-false check).
 */
inline std::atomic<bool> g_tui_cancel_requested{false};

/**
 * @brief Poll the cancel flag (step boundaries only, never mid-step).
 * @return True once request_cancel() ran and clear_cancel() has not.
 */
[[nodiscard]] inline bool cancel_requested() noexcept {
  return g_tui_cancel_requested.load(std::memory_order_relaxed);
}

/**
 * @brief Request cancellation (the worker stops at the next boundary).
 */
inline void request_cancel() noexcept {
  g_tui_cancel_requested.store(true, std::memory_order_relaxed);
}

/**
 * @brief Clear the cancel flag (arms the next run).
 */
inline void clear_cancel() noexcept {
  g_tui_cancel_requested.store(false, std::memory_order_relaxed);
}

// --- progress ---------------------------------------------------------------
// `progress_begin(total)` arms the display, `progress_update(done)`
// advances it, `progress_end()` disarms. `total == UINT64_MAX` marks an
// unbounded (`--forever`) run: the UI shows `done` without a denominator.
/**
 * @brief Progress display armed flag (UI thread reads, worker writes).
 */
inline std::atomic<bool> g_progress_active{false};
/**
 * @brief Completed steps shown so far (monotonic within a run).
 */
inline std::atomic<std::uint64_t> g_progress_done{0};
/**
 * @brief Step budget shown as the denominator (`UINT64_MAX` marks an
 *        unbounded `--forever` run: the UI shows `done` alone).
 */
inline std::atomic<std::uint64_t> g_progress_total{0};

/**
 * @brief Arm the progress display for a run.
 * @param[in] total Step budget, or `UINT64_MAX` for an unbounded run.
 */
inline void progress_begin(std::uint64_t total) noexcept {
  g_progress_total.store(total, std::memory_order_relaxed);
  g_progress_done.store(0, std::memory_order_relaxed);
  g_progress_active.store(true, std::memory_order_relaxed);
}

/**
 * @brief Advance the displayed step count.
 * @param[in] done Completed steps so far.
 */
inline void progress_update(std::uint64_t done) noexcept {
  g_progress_done.store(done, std::memory_order_relaxed);
}

/**
 * @brief Disarm the progress display (end of run).
 */
inline void progress_end() noexcept {
  g_progress_active.store(false, std::memory_order_relaxed);
}

/**
 * @brief Whether the progress display is armed.
 * @return True between progress_begin and progress_end.
 */
[[nodiscard]] inline bool progress_active() noexcept {
  return g_progress_active.load(std::memory_order_relaxed);
}

/**
 * @brief Displayed completed-step count.
 * @return Steps reported by the latest progress_update.
 */
[[nodiscard]] inline std::uint64_t progress_done() noexcept {
  return g_progress_done.load(std::memory_order_relaxed);
}

/**
 * @brief Displayed step budget.
 * @return Total passed to progress_begin (`UINT64_MAX` for unbounded).
 */
[[nodiscard]] inline std::uint64_t progress_total() noexcept {
  return g_progress_total.load(std::memory_order_relaxed);
}

} // namespace wengine::cli
