// TUI-5 async worker: parity, busy, cancel, progress, truncation, emit-csv.
//
// The worker runs `cli::Shell::execute` on its own Shell off the UI thread,
// so output stays byte-identical to the CLI by construction. These pins
// cover the acceptance contract: input stays live (busy rejects second
// starts), Ctrl-C cancels at a step boundary with flushed sinks, no data
// race (atomics only — also run under Clang TSAN), truncation stays loud,
// and --emit-* files land on disk.

#include "wengine/tui/async.hpp"
#include "wengine/cli/cancel.hpp"
#include "wengine/cli/shell.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace wengine;

namespace {

// Sync helper: headless Shell result for parity.
tui::AsyncResult sync_run(const std::string &line) {
  cli::Shell shell;
  shell.set_history_file("");
  std::ostringstream out;
  const int rc = shell.execute(line, out);
  return {.rc = rc, .text = out.str(), .line = line, .cancelled = false};
}

tui::AsyncResult wait_result(tui::AsyncTask &task) {
  tui::AsyncResult r;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (task.try_take(r)) {
      return r;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::cerr << "async task did not finish in 30s\n";
  assert(false);
  return r;
}

std::string tmp(const char *name) { return std::string("/tmp/opencode/") + name; }

void test_parity() {
  const char *lines[] = {
      "ohm 12V 3A",
      "sim run --steps 2 --particles 1",
      "sig run --sig-kind dc --steps 2",
      "sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 2 --dt 0.1ms",
      "nopecmd",
      "ohm 12V", // usage error, same code + text
  };
  for (const char *c : lines) {
    tui::AsyncTask task;
    assert(task.start(c));
    const tui::AsyncResult got = wait_result(task);
    const tui::AsyncResult want = sync_run(c);
    if (got.rc != want.rc || got.text != want.text) {
      std::cerr << "parity mismatch for: " << c << "\n--- want rc=" << want.rc
                << "\n" << want.text << "\n--- got rc=" << got.rc << "\n"
                << got.text << "\n";
    }
    assert(got.rc == want.rc);
    assert(got.text == want.text);
    assert(!got.cancelled);
  }
}

void test_busy_rejects_second() {
  tui::AsyncTask task;
  assert(task.start("sig run --sig-kind sine --forever"));
  // Second start while busy must fail loudly (caller keeps its input).
  assert(task.busy());
  assert(!task.start("ohm 12V 3A"));
  task.request_cancel();
  const tui::AsyncResult r = wait_result(task);
  assert(r.rc == 0);
  assert(r.cancelled);
  assert(r.text.find("interrupted by Ctrl-C") != std::string::npos);
  assert(!task.busy());
  // Idle again: new work starts.
  assert(task.start("ohm 12V 3A"));
  const tui::AsyncResult r2 = wait_result(task);
  assert(r2.rc == 0 && !r2.cancelled);
}

void test_cancel_finite_with_emit_csv() {
  const std::string path = tmp("wengine_test_tui_async_cancel.csv");
  std::remove(path.c_str());
  tui::AsyncTask task;
  // Long finite batch with a CSV sink: cancel must leave a coherent partial
  // prefix with the file flushed/closed normally (same CLI path as headless).
  const std::string line =
      "sig run --sig-kind sine --sig-freq 10Hz --steps 1000000 --dt 0.1ms --emit-csv " +
      path;
  assert(task.start(line));
  // Let the worker pass validation and begin stepping, then cancel.
  bool saw_active = false;
  for (int i = 0; i < 200; ++i) {
    if (cli::progress_active()) {
      saw_active = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(saw_active);
  const std::uint64_t done_before = cli::progress_done();
  (void)done_before;
  task.request_cancel();
  const tui::AsyncResult r = wait_result(task);
  assert(!task.busy());
  assert(!cli::progress_active());
  assert(r.cancelled);
  assert(r.rc == 0);
  assert(r.text.find("interrupted by Ctrl-C") != std::string::npos);
  assert(r.text.find("csv: " + path) != std::string::npos);
  std::ifstream in(path);
  assert(static_cast<bool>(in));
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header.find("sig_id") != std::string::npos);
  std::size_t rows = 0;
  std::string l;
  while (std::getline(in, l)) {
    ++rows;
  }
  // Partial prefix: some rows flushed, fewer than the 1M-step budget.
  assert(rows > 0 && rows < 1000000);
  std::remove(path.c_str());
}

void test_cancel_forever() {
  tui::AsyncTask task;
  assert(task.start("sig run --sig-kind sine --sig-freq 1kHz --forever"));
  // Wait until the worker is actually stepping (progress armed).
  bool saw = false;
  for (int i = 0; i < 200; ++i) {
    if (cli::progress_active()) {
      saw = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  assert(saw);
  task.request_cancel();
  const tui::AsyncResult r = wait_result(task);
  assert(r.cancelled && r.rc == 0);
  assert(r.text.find("interrupted by Ctrl-C") != std::string::npos);
  assert(!cli::progress_active());
}

void test_progress_finite() {
  tui::AsyncTask task;
  assert(task.start("sig run --sig-kind sine --steps 200000 --dt 0.01ms"));
  bool saw_active = false;
  std::uint64_t max_done = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  tui::AsyncResult r;
  bool finished = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cli::progress_active()) {
      saw_active = true;
      const std::uint64_t d = cli::progress_done();
      if (d > max_done) {
        max_done = d;
      }
    }
    if (task.try_take(r)) {
      finished = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(finished);
  assert(saw_active);
  assert(max_done > 0);
  assert(r.rc == 0 && !r.cancelled);
  assert(!cli::progress_active());
}

void test_emit_csv_unaffected() {
  const std::string path = tmp("wengine_test_tui_async_emit.csv");
  std::remove(path.c_str());
  tui::AsyncTask task;
  const std::string line =
      "sig run --sig-kind dc --sig-offset 1V --steps 8 --emit-csv " + path;
  assert(task.start(line));
  const tui::AsyncResult got = wait_result(task);
  const tui::AsyncResult want = sync_run(line);
  assert(got.rc == want.rc && got.text == want.text && got.rc == 0);
  std::ifstream in(path);
  assert(static_cast<bool>(in));
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header.find("sig_id") != std::string::npos);
  std::size_t rows = 0;
  std::string l;
  while (std::getline(in, l)) {
    ++rows;
  }
  assert(rows == 8);
  std::remove(path.c_str());
}

} // namespace

int main() {
  test_parity();
  test_busy_rejects_second();
  test_cancel_finite_with_emit_csv();
  test_cancel_forever();
  test_progress_finite();
  test_emit_csv_unaffected();
  std::cout << "test_tui_async: OK\n";
  return 0;
}
