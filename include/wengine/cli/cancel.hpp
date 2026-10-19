#pragma once

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

inline std::atomic<bool> g_tui_cancel_requested{false};

[[nodiscard]] inline bool cancel_requested() noexcept {
  return g_tui_cancel_requested.load(std::memory_order_relaxed);
}

inline void request_cancel() noexcept {
  g_tui_cancel_requested.store(true, std::memory_order_relaxed);
}

inline void clear_cancel() noexcept {
  g_tui_cancel_requested.store(false, std::memory_order_relaxed);
}

// --- progress ---------------------------------------------------------------
// `progress_begin(total)` arms the display, `progress_update(done)`
// advances it, `progress_end()` disarms. `total == UINT64_MAX` marks an
// unbounded (`--forever`) run: the UI shows `done` without a denominator.

inline std::atomic<bool> g_progress_active{false};
inline std::atomic<std::uint64_t> g_progress_done{0};
inline std::atomic<std::uint64_t> g_progress_total{0};

inline void progress_begin(std::uint64_t total) noexcept {
  g_progress_total.store(total, std::memory_order_relaxed);
  g_progress_done.store(0, std::memory_order_relaxed);
  g_progress_active.store(true, std::memory_order_relaxed);
}

inline void progress_update(std::uint64_t done) noexcept {
  g_progress_done.store(done, std::memory_order_relaxed);
}

inline void progress_end() noexcept {
  g_progress_active.store(false, std::memory_order_relaxed);
}

[[nodiscard]] inline bool progress_active() noexcept {
  return g_progress_active.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t progress_done() noexcept {
  return g_progress_done.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t progress_total() noexcept {
  return g_progress_total.load(std::memory_order_relaxed);
}

} // namespace wengine::cli
