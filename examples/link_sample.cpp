// link_sample: minimal external-app reader for the wengine-link SHM bus.
//
// Attaches read-only to a publisher segment (e.g. from
// `PhysicsSimApp sim run --emit-shm <name>`) and prints each new frame as
// it arrives: sequence, step, time, item/channel counts, exact missed-
// frame lag, then every channel as name=value.
//
// Usage:
//   link_sample <segment> [--frames N] [--timeout-ms T]
//     <segment>    publisher segment name, e.g. /wengine-demo
//     --frames N   print N new frames, then exit 0 (0 = subscribe forever)
//     --timeout-ms T  give up after T ms without a new frame (0 = never)
// Exit codes mirror the CLI convention: 0 ok, 1 timeout/attach failure,
// 2 usage error. Human-readable output only; the exact machine log stays
// CSV (CsvSink). Start order is free: the sample waits for the segment to
// appear until the timeout expires.
//
// STOPPING (receive-count, not final-sequence): --frames N counts frames
// this reader CONSUMES, not frames the publisher emitted. Under drop-oldest
// loss one poll jumps straight to the newest frame, so consumed < emitted
// and an exact published count may be unreachable — the run then ends in
// an idle timeout that prints a final `timeout shown=... missed=...
// last_seq=...` summary (exit 1), never silence.
//
// LIFETIME: the segment exists only while the publisher lives (it unlinks
// on exit), so a short flood can finish before attach and late attach
// after exit is impossible by design (`timed out waiting for segment`).
// An attached reader keeps its mapping after the publisher exits and still
// drains the retained final frames. A same-name republish is invisible to
// an attached reader (the old mapping parks on the old run's frozen final
// frame); restart the reader to see the new run — explicit reconnect,
// mirroring gui::ShmSource::reconnect().

#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_reader.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::uint64_t kDefaultTimeoutMs = 15000;
constexpr std::uint64_t kPollMs = 1;
constexpr std::uint64_t kAttachRetryMs = 1;

int usage_error(const std::string &msg) {
  std::cerr << "Error: " << msg << "\n"
            << "Usage: link_sample <segment> [--frames N] [--timeout-ms T]\n";
  return 2;
}

// Strict full-string unsigned parse; false on any trailing garbage.
bool parse_u64(const char *text, std::uint64_t &out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(v);
  return true;
}

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ms(const Clock::time_point &since) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since)
          .count());
}

void print_frame(const wengine::link::LinkFrame &wire,
                 const wengine::link::Frame &frame, std::uint64_t missed) {
  std::cout << "seq=" << wire.frame_seq << " step=" << wire.step_index
            << " t=" << wire.sim_time << "s items=" << frame.items.size()
            << " channels=" << frame.channels.size() << " missed=" << missed;
  for (const auto &c : frame.channels) {
    std::cout << " " << c.name << "=" << c.value;
  }
  if (!frame.items.empty()) {
    const auto &p = frame.items.front();
    std::cout << " p0=(" << p.x << "," << p.y << "," << p.z << ")";
  }
  std::cout << "\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return usage_error("missing <segment>");
  }
  const std::string segment = argv[1];
  if (!wengine::link::valid_segment_name(segment)) {
    return usage_error("invalid segment name (want /name)");
  }
  std::uint64_t want_frames = 0; // 0 = subscribe forever
  std::uint64_t timeout_ms = kDefaultTimeoutMs;
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    std::uint64_t v = 0;
    if ((arg == "--frames" || arg == "--timeout-ms") && i + 1 >= argc) {
      return usage_error(std::string(arg) + " needs a value");
    }
    if (arg == "--frames") {
      if (!parse_u64(argv[++i], v)) {
        return usage_error("invalid --frames value");
      }
      want_frames = v;
    } else if (arg == "--timeout-ms") {
      if (!parse_u64(argv[++i], v)) {
        return usage_error("invalid --timeout-ms value");
      }
      timeout_ms = v;
    } else {
      return usage_error(std::string("unexpected argument '") + std::string(arg) + "'");
    }
  }

  // Wait for the publisher: start order between sim and sample is free.
  // Missing/unready segments retry until the timeout; refusals and
  // syscall errors fail fast with a clear message (never a timeout).
  const Clock::time_point start = Clock::now();
  std::optional<wengine::link::ShmReader> reader;
  for (;;) {
    reader.emplace(segment);
    if (reader->ok()) {
      break;
    }
    const wengine::link::AttachState st = reader->state();
    const wengine::link::HeaderReport rep = reader->report();
    const int err = reader->last_errno();
    reader.reset();
    using wengine::link::AttachState;
    if (st == AttachState::kRefused) {
      std::cerr << "Error: " << wengine::link::describe_refusal(segment, rep) << "\n";
      return 1;
    }
    if (st != AttachState::kMissing && st != AttachState::kUnready) {
      if (err != 0) {
        std::cerr << "Error: cannot attach to segment '" << segment
                  << "': " << std::strerror(err) << "\n";
      } else {
        std::cerr << "Error: cannot attach to segment '" << segment << "'\n";
      }
      return 1;
    }
    if (timeout_ms != 0 && elapsed_ms(start) >= timeout_ms) {
      std::cerr << "link_sample: timed out waiting for segment '" << segment
                << "'\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kAttachRetryMs));
  }

  std::uint64_t last_seq = 0;
  std::uint64_t shown = 0;
  std::uint64_t missed_total = 0;
  std::uint64_t max_gap = 0;
  Clock::time_point last_new = Clock::now();
  for (;;) {
    wengine::link::LinkFrame wire{};
    const std::uint64_t prev = last_seq;
    if (reader->poll_newer(last_seq, wire)) {
      const std::uint64_t missed = wengine::link::reader_missed(prev, wire.frame_seq);
      missed_total += missed;
      if (missed > max_gap) {
        max_gap = missed;
      }
      wengine::link::Frame frame;
      if (!wengine::link::decode_frame(wire, frame).ok()) {
        std::cerr << "link_sample: corrupt frame data\n";
        return 1;
      }
      print_frame(wire, frame, missed);
      last_new = Clock::now();
      if (++shown == want_frames && want_frames != 0) {
        return 0;
      }
      continue;
    }
    if (timeout_ms != 0 && elapsed_ms(last_new) >= timeout_ms) {
      // Idle end-of-run: same reporting contract as link_fastread — say
      // where the cursor parked (unreachable receive count, exited
      // publisher, or stale same-name mapping) before failing.
      std::cout << "timeout shown=" << shown << " missed=" << missed_total
                << " max_gap=" << max_gap << " last_seq=" << last_seq << "\n";
      std::cerr << "link_sample: timed out waiting for new frames\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
}
