// link_fastread: minimum-loss subscriber for the wengine-link SHM bus.
//
// Same wire contract as link_sample (attach, poll newest, exact lag
// accounting), minus everything that costs per-frame: no per-frame
// printing (usually the dominant reader cost — one status line per
// --status-every frames plus a final summary instead), one reused decode
// buffer (vector capacity is reused after warmup, so steady-state reading
// allocates nothing), and the reader itself copies only the newest slot
// per poll. Sleep applies only when starved; while frames flow the loop
// busy-polls.
//
// Honest limits: against an unpaced batch publisher (~10^5 frames/s into
// an 8-deep ring) no reader can keep up — loss is then policy, reported
// exactly. Zero loss is achievable whenever the publish rate stays below
// the poll rate, i.e. every paced/interactive use: `sim run --emit-every
// N`, bridge playback, or the GUI-driven publisher.
//
// STOPPING (receive-count, not final-sequence): --frames N counts frames
// this reader CONSUMES, not frames the publisher emitted. Under drop-oldest
// loss one poll jumps straight to the newest frame, so consumed < emitted
// and an exact published count may be unreachable — the run then ends in
// an idle timeout that prints the same loss summary as the done line
// (received/missed/max_gap/last_seq, exit 1), never silence. For exact
// capture use paced rates or per-sink strides so nothing is skipped.
//
// LIFETIME: the segment exists only while the publisher lives (it unlinks
// on exit). A short flood can finish before this reader attaches — late
// attach after exit is impossible by design, and reports `timed out
// waiting for segment`. An attached reader keeps its mapping after the
// publisher exits and still drains the retained final frames. A same-name
// republish is invisible to an attached reader (POSIX fd semantics: the
// old mapping parks on the old run's frozen final frame, reported as "no
// new frames"); restart the reader to see the new run — explicit
// reconnect, mirroring gui::ShmSource::reconnect().
//
// Usage:
//   link_fastread <segment> [--frames N] [--timeout-ms T] [--status-every M]
// Exit codes mirror the CLI convention: 0 ok, 1 timeout/attach failure,
// 2 usage error.

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

constexpr std::uint64_t kDefaultTimeoutMs = 5000;
constexpr std::uint64_t kPollMs = 1;
// Missing/unready attach retry: 1 ms like link_sample. Short floods live
// only a few ms, so every millisecond of attach latency is a wider blind
// window (PHYSI-43); the wait stays bounded by --timeout-ms.
constexpr std::uint64_t kAttachRetryMs = 1;
constexpr std::uint64_t kDefaultStatusEvery = 1000;

int usage_error(const std::string &msg) {
  std::cerr << "Error: " << msg << "\n"
            << "Usage: link_fastread <segment> [--frames N] [--timeout-ms T]\n"
            << "                              [--status-every M]\n";
  return 2;
}

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

// Frames/s over a span; sub-tick spans report the bare count (never divide
// by zero — tiny scripted runs finish inside one clock tick).
std::uint64_t rate_per_s(std::uint64_t n, double secs) {
  if (!(secs > 0.0)) {
    return n;
  }
  return static_cast<std::uint64_t>(static_cast<double>(n) / secs);
}

// Final accounting line, shared by the done and idle-timeout exits so a
// stall never goes silent: received/missed/max_gap/last_seq distinguish an
// unreachable receive-count target (cursor parked on the published final
// frame, missed > 0) from a genuinely starved bus (cursor never advanced).
void print_summary(const char *tag, std::uint64_t received,
                   std::uint64_t missed, std::uint64_t max_gap,
                   std::uint64_t last_seq, double secs) {
  const double loss_pct =
      received + missed == 0
          ? 0.0
          : 100.0 * static_cast<double>(missed) /
                static_cast<double>(received + missed);
  std::cout << tag << " received=" << received << " missed=" << missed
            << " max_gap=" << max_gap << " last_seq=" << last_seq
            << " loss=" << loss_pct << "% rate=" << rate_per_s(received, secs)
            << "/s\n";
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
  std::uint64_t status_every = kDefaultStatusEvery; // 0 = summary only
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    std::uint64_t v = 0;
    if ((arg == "--frames" || arg == "--timeout-ms" || arg == "--status-every") &&
        i + 1 >= argc) {
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
    } else if (arg == "--status-every") {
      if (!parse_u64(argv[++i], v)) {
        return usage_error("invalid --status-every value");
      }
      status_every = v;
    } else {
      return usage_error(std::string("unexpected argument '") + std::string(arg) + "'");
    }
  }

  // Wait for the publisher: start order between sim and reader is free.
  // Missing/unready retry until the timeout; refusals and syscall errors
  // fail fast with a clear message (never a timeout).
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
      std::cerr << "link_fastread: timed out waiting for segment '" << segment
                << "'\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kAttachRetryMs));
  }

  // Reused across polls: vector/string capacity survives after warmup, so
  // steady-state reading performs no allocations.
  wengine::link::Frame frame;
  std::uint64_t last_seq = 0;
  std::uint64_t received = 0;
  wengine::link::LagStats lag;
  const Clock::time_point flow_start = Clock::now();
  Clock::time_point last_new = Clock::now();
  for (;;) {
    wengine::link::LinkFrame wire{};
    if (reader->poll_newer(last_seq, wire)) {
      (void)wengine::link::lag_observe(lag, wire.frame_seq);
      if (!wengine::link::decode_frame(wire, frame).ok()) {
        std::cerr << "link_fastread: corrupt frame data\n";
        return 1;
      }
      ++received;
      last_new = Clock::now();
      if (status_every != 0 && received % status_every == 0) {
        const double secs =
            std::chrono::duration<double>(last_new - flow_start).count();
        std::cout << "received=" << received << " missed=" << lag.missed
                  << " max_gap=" << lag.max_gap << " last_seq=" << last_seq << " rate="
                  << rate_per_s(received, secs) << "/s\n";
      }
      if (received == want_frames && want_frames != 0) {
        break;
      }
      continue;
    }
    if (timeout_ms != 0 && elapsed_ms(last_new) >= timeout_ms) {
      // Idle end-of-run: the receive-count target was unreachable (loss
      // skipped it, the publisher exited, or the mapping is a stale
      // same-name run). Report where the cursor parked before failing.
      const double total_secs =
          std::chrono::duration<double>(Clock::now() - flow_start).count();
      print_summary("timeout", received, lag.missed, lag.max_gap, last_seq,
                    total_secs);
      std::cerr << "link_fastread: timed out waiting for new frames\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
  const double total_secs =
      std::chrono::duration<double>(Clock::now() - flow_start).count();
  print_summary("done", received, lag.missed, lag.max_gap, last_seq,
                total_secs);
  return 0;
}
