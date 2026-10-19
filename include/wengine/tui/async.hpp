#pragma once

// Async worker for the TUI (TUI-5): runs one CLI line off the UI thread so
// long sim/sig batches keep input live with progress + step-boundary cancel.
//
// Contract (D-TUI-1 preserved): the worker runs `cli::Shell::execute`
// verbatim on its own `Shell` (no shared executor state), so output stays
// byte-identical to the CLI by construction. The UI thread never touches
// the worker's `Shell` or output stream: it only sets the global
// `cli::request_cancel()` flag and reads the global `cli::progress_*`
// atoms (both TSAN-clean) plus this task's atomics.
//
// - `start(line)` fails (false) while busy: the caller reports loudly and
//   keeps the pending input.
// - `request_cancel()` is Ctrl-C: the CLI chunk loops stop at the next step
//   boundary, flush/close sinks normally, and report the partial prefix.
// - `try_take` is the non-blocking poll for the frame loop (100 ms tick);
//   `wait_take` is the blocking join for quit/shutdown.
// - Only one task at a time (process-global cancel/progress atoms).
// - ncurses-free and unit-testable (see `tests/test_tui_async.cpp`).

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

struct AsyncResult {
  int rc = 0;
  std::string text;
  std::string line;
  bool cancelled = false;
};

class AsyncTask {
public:
  AsyncTask() = default;
  ~AsyncTask() {
    // Shutdown must never hang on `--forever`: request the step-boundary
    // stop first, then join. Normal completion joins immediately.
    if (thread_.joinable()) {
      cli::request_cancel();
      thread_.join();
    }
  }

  AsyncTask(const AsyncTask &) = delete;
  AsyncTask &operator=(const AsyncTask &) = delete;
  AsyncTask(AsyncTask &&) = delete;
  AsyncTask &operator=(AsyncTask &&) = delete;

  [[nodiscard]] bool busy() const noexcept {
    return busy_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool done() const noexcept {
    return done_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool cancel_requested() const noexcept {
    return cancel_local_.load(std::memory_order_relaxed);
  }

  // Calling thread only: the stored line for progress display.
  [[nodiscard]] std::string line() const {
    return line_;
  }

  // Calling thread only: seconds since `start` (progress display).
  [[nodiscard]] double elapsed_s() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now() - start_time_).count();
  }

  // Start `line` on the worker. False when busy (caller reports loudly).
  // Clears any stale cancel/progress state first.
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

  void request_cancel() noexcept {
    cancel_local_.store(true, std::memory_order_relaxed);
    cli::request_cancel();
  }

  // Non-blocking poll for the frame loop: true + `out` when a finished
  // task was joined. False while idle or still running.
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

  // Blocking join for quit/shutdown (caller already requested cancel).
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
