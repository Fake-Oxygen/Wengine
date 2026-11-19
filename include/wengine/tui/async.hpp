#pragma once

/**
 * @file
 * Async execution model around one CLI run (TUI-5): a single worker
 * thread runs one submitted CLI line off the UI thread so long sim/sig
 * batches keep input live with progress and a step-boundary Ctrl-C
 * cancel. The TUI stays a pure front end — it owns presentation, never
 * simulation state: the worker executes `cli::Shell::execute` verbatim on
 * its own `Shell`, so output stays byte-identical to the REPL/headless
 * CLI by construction. Consumers: `src/tui/submit.cpp` (input line,
 * forms, batches all submit through this), `tests/test_tui_async.cpp`.
 * ncurses-free and unit-testable without a terminal.
 *
 * Contract (D-TUI-1 preserved): the worker runs `cli::Shell::execute`
 * verbatim on its own `Shell` (no shared executor state), so output stays
 * byte-identical to the CLI by construction. The UI thread never touches
 * the worker's `Shell` or output stream: it only sets the global
 * `cli::request_cancel()` flag and reads the global `cli::progress_*`
 * atoms (both TSAN-clean) plus this task's atomics.
 *
 * - `start(line)` fails (false) while busy: the caller reports loudly and
 *   keeps the pending input.
 * - `request_cancel()` is Ctrl-C: the CLI chunk loops stop at the next step
 *   boundary, flush/close sinks normally, and report the partial prefix.
 * - `try_take` is the non-blocking poll for the frame loop (100 ms tick);
 *   `wait_take` is the blocking join for quit/shutdown.
 * - Only one task at a time (process-global cancel/progress atoms).
 */

#include "wengine/cli/cancel.hpp"
#include "wengine/cli/shell.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace wengine::tui {

/**
 * @brief Outcome of one asynchronously executed CLI line.
 */
struct AsyncResult {
  /**
   * @brief CLI exit code as returned by `cli::Shell::execute`:
   *        0 ok, 1 runtime/check failure, 2 usage error, -1 exit signal.
   */
  int rc = 0;
  /**
   * @brief Exactly what the CLI printed for the line (captured stream).
   */
  std::string text;
  /**
   * @brief The submitted line (kept for status/scrollback echo).
   */
  std::string line;
  /**
   * @brief Whether Ctrl-C requested the step-boundary stop; the CLI still
   *        flushed and closed sinks normally and printed its partial
   *        prefix before returning.
   */
  bool cancelled = false;
};

/**
 * @brief One-task async worker: a single background thread plus its own
 *        CLI shell.
 *
 * @note Single task at a time — the cancel/progress atoms
 *       (`wengine/cli/cancel.hpp`) are process-global, so a second
 *       concurrent run would corrupt their accounting. Not copyable and
 *       not movable: the worker captures `this`.
 */
class AsyncTask {
public:
  /**
   * @brief Idle task with no worker thread yet.
   */
  AsyncTask() = default;

  /**
   * @brief Cancel-then-join so shutdown never hangs on `--forever`.
   *
   * @note Requests the step-boundary stop first, then joins. Normal
   *       completion joins immediately; a `--forever` run stops at its
   *       next step boundary with sinks flushed/closed normally.
   */
  ~AsyncTask() {
    // Shutdown must never hang on `--forever`: request the step-boundary
    // stop first, then join. Normal completion joins immediately.
    if (thread_.joinable()) {
      cli::request_cancel();
      thread_.join();
    }
  }

  /**
   * @brief Copy construction is deleted (the worker captures `this`).
   */
  AsyncTask(const AsyncTask &) = delete;
  /**
   * @brief Copy assignment is deleted (the worker captures `this`).
   */
  AsyncTask &operator=(const AsyncTask &) = delete;
  /**
   * @brief Move construction is deleted (the worker captures `this`).
   */
  AsyncTask(AsyncTask &&) = delete;
  /**
   * @brief Move assignment is deleted (the worker captures `this`).
   */
  AsyncTask &operator=(AsyncTask &&) = delete;

  /**
   * @brief Whether a task is running (or finished but not yet taken).
   *
   * @return True while a started task has not been joined by
   *         `try_take`/`wait_take`; `start` fails while this is true.
   */
  [[nodiscard]] bool busy() const noexcept {
    return busy_.load(std::memory_order_acquire);
  }

  /**
   * @brief Whether the worker finished but the result is untaken.
   *
   * @return True once the worker stored its result and is waiting for a
   *         `try_take`/`wait_take` join; false while idle or running.
   */
  [[nodiscard]] bool done() const noexcept {
    return done_.load(std::memory_order_acquire);
  }

  /**
   * @brief Whether Ctrl-C was requested for this task.
   *
   * @return True after `request_cancel` on this task and before the next
   *         `start` clears it.
   */
  [[nodiscard]] bool cancel_requested() const noexcept {
    return cancel_local_.load(std::memory_order_relaxed);
  }

  /**
   * @brief The stored line for progress display.
   *
   * @return Copy of the line passed to the last successful `start`.
   * @note Calling thread only (the UI thread): `line_` is plain string
   *       state guarded by the single-task contract, not an atom.
   */
  [[nodiscard]] std::string line() const {
    return line_;
  }

  /**
   * @brief Seconds since the last successful `start`.
   *
   * @return Elapsed wall-clock seconds measured on `std::chrono::steady_clock`
   *         (monotonic; immune to wall-clock adjustments).
   * @note Calling thread only, for the status bar's elapsed display.
   */
  [[nodiscard]] double elapsed_s() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now() - start_time_).count();
  }

  /**
   * @brief Start `line` on the worker.
   *
   * @param[in] line One CLI command line to execute verbatim.
   * @return True when the task started; false while busy (the caller
   *         reports loudly and keeps the pending input).
   * @note Clears any stale cancel/progress state first so a previous
   *       Ctrl-C cannot leak into the new run.
   */
  bool start(std::string line) {
    if (busy_.load(std::memory_order_acquire)) {
      return false;
    }
    cli::clear_cancel();
    cli::progress_end();
    cancel_local_.store(false, std::memory_order_relaxed);
    line_ = line;
    start_time_ = std::chrono::steady_clock::now();
    done_.store(false, std::memory_order_release);
    busy_.store(true, std::memory_order_release);
    thread_ = std::thread([line_copy = std::move(line), this] {
      cli::Shell shell;
      shell.set_history_file("");
      std::ostringstream out;
      const int rc = shell.execute(line_copy, out);
      const bool was_cancel = cli::cancel_requested();
      {
        const std::lock_guard<std::mutex> lk(mu_);
        result_.rc = rc;
        result_.text = out.str();
        result_.line = line_copy;
        result_.cancelled = was_cancel;
      }
      done_.store(true, std::memory_order_release);
    });
    return true;
  }

  /**
   * @brief Request the Ctrl-C stop for the running task.
   *
   * @note This is Ctrl-C: the CLI chunk loops stop at the next step
   *       boundary, flush/close sinks normally, and report the partial
   *       `interrupted by Ctrl-C` prefix. No-op when idle.
   */
  void request_cancel() noexcept {
    cancel_local_.store(true, std::memory_order_relaxed);
    cli::request_cancel();
  }

  /**
   * @brief Non-blocking poll of the frame loop (100 ms tick).
   *
   * @param[out] out Receives the finished task's result when true.
   * @return True plus `out` when a finished task was joined; false while
   *         idle or still running (nothing is written to `out` then).
   */
  bool try_take(AsyncResult &out) {
    if (!busy_.load(std::memory_order_acquire)) {
      return false;
    }
    if (!done_.load(std::memory_order_acquire)) {
      return false;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    {
      const std::lock_guard<std::mutex> lk(mu_);
      out = result_;
    }
    done_.store(false, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
    return true;
  }

  /**
   * @brief Blocking join for quit/shutdown.
   *
   * @param[out] out Receives the joined task's result when true.
   * @return True plus `out` when a task was joined (waits for completion);
   *         false when idle.
   * @note Call after `request_cancel` so a `--forever` run cannot hang
   *       the join: the worker stops at its next step boundary.
   */
  bool wait_take(AsyncResult &out) {
    if (!busy_.load(std::memory_order_acquire)) {
      return false;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    {
      const std::lock_guard<std::mutex> lk(mu_);
      out = result_;
    }
    done_.store(false, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
    return true;
  }

private:
  std::thread thread_;
  mutable std::mutex mu_;
  AsyncResult result_;
  std::string line_;
  std::chrono::steady_clock::time_point start_time_{};
  std::atomic<bool> busy_{false};
  std::atomic<bool> done_{false};
  std::atomic<bool> cancel_local_{false};
};

} // namespace wengine::tui
