// link_bridge: replay bridge between CsvSink logs and the wengine-link bus.
//
//   link_bridge play <csv> <segment> [--domain T] [--rate R]
//     Parse a CsvSink log and publish its snapshots onto a fresh SHM
//     segment. --domain tags the segment (default "projectile"; CSV logs
//     carry no provenance). --rate scales wall-clock pacing against the
//     log's own time column (default 1 = original rate, 0 = unpaced).
//   link_bridge record <segment> <csv> [--frames N] [--timeout-ms T]
//     Subscribe to a live segment and append every received frame to a CSV
//     log in CsvSink format (byte-identical for particle runs).
//
// Makes every logged run replayable for GUI/testing without re-running the
// engine. Empty snapshots have no CSV rows (format limitation) and are
// skipped both directions, consistently. Exit codes mirror the CLI
// convention: 0 ok, 1 runtime/timeout failure, 2 usage error.

#include "wengine/emit/csv_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_publisher.hpp"
#include "wengine/link/shm_reader.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kDefaultTimeoutMs = 5000;
constexpr std::uint64_t kPollMs = 1;
constexpr std::uint64_t kAttachRetryMs = 10;

int usage_error(const std::string &msg) {
  std::cerr << "Error: " << msg << "\n"
            << "Usage:\n"
            << "  link_bridge play <csv> <segment> [--domain T] [--rate R]\n"
            << "  link_bridge record <segment> <csv> [--frames N] [--timeout-ms T]\n";
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

bool parse_rate(const char *text, double &out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char *end = nullptr;
  const double v = std::strtod(text, &end);
  if (end == text || *end != '\0') {
    return false;
  }
  if (!(v >= 0.0) || std::isinf(v)) {
    return false; // rejects negatives and NaN; inf would stall pacing
  }
  out = v;
  return true;
}

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ms(const Clock::time_point &since) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since)
          .count());
}

int run_play(int argc, char **argv) {
  // play <csv> <segment> [--domain T] [--rate R]
  if (argc < 4) {
    return usage_error("play needs <csv> and <segment>");
  }
  const std::string csv_path = argv[2];
  const std::string segment = argv[3];
  if (!wengine::link::valid_segment_name(segment)) {
    return usage_error("invalid segment name (want /name)");
  }
  std::string domain = "projectile";
  double rate = 1.0;
  for (int i = 4; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if ((arg == "--domain" || arg == "--rate") && i + 1 >= argc) {
      return usage_error(std::string(arg) + " needs a value");
    }
    if (arg == "--domain") {
      domain = argv[++i];
    } else if (arg == "--rate") {
      if (!parse_rate(argv[++i], rate)) {
        return usage_error("invalid --rate value (want 0 or > 0)");
      }
    } else {
      return usage_error(std::string("unexpected argument '") + std::string(arg) + "'");
    }
  }

  std::ifstream in(csv_path);
  if (!in) {
    std::cerr << "Error: cannot open CSV file '" << csv_path << "'\n";
    return 1;
  }
  std::vector<wengine::world::Snapshot> snaps;
  std::string error;
  if (!wengine::emit::parse_csv_log(in, snaps, error).ok()) {
    std::cerr << "Error: cannot parse CSV file '" << csv_path << "': " << error << "\n";
    return 1;
  }

  wengine::link::ShmPublisher pub(segment, domain);
  if (!pub.ok()) {
    std::cerr << "Error: cannot open SHM segment '" << segment << "'\n";
    return 1;
  }
  for (std::size_t k = 0; k < snaps.size(); ++k) {
    if (k > 0 && rate > 0.0) {
      const double dt_sim = snaps[k].sim_time - snaps[k - 1].sim_time;
      if (dt_sim > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(dt_sim / rate));
      }
    }
    pub.on_snapshot(snaps[k]);
    if (!pub.ok()) {
      std::cerr << "Error: SHM publish failed on '" << segment << "'\n";
      return 1;
    }
  }
  std::cout << "replayed " << snaps.size() << " snapshots to '" << segment << "'\n";
  return 0;
}

int run_record(int argc, char **argv) {
  // record <segment> <csv> [--frames N] [--timeout-ms T]
  if (argc < 4) {
    return usage_error("record needs <segment> and <csv>");
  }
  const std::string segment = argv[2];
  const std::string csv_path = argv[3];
  if (!wengine::link::valid_segment_name(segment)) {
    return usage_error("invalid segment name (want /name)");
  }
  std::uint64_t want_frames = 0; // 0 = record forever
  std::uint64_t timeout_ms = kDefaultTimeoutMs;
  for (int i = 4; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if ((arg == "--frames" || arg == "--timeout-ms") && i + 1 >= argc) {
      return usage_error(std::string(arg) + " needs a value");
    }
    std::uint64_t v = 0;
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

  wengine::emit::CsvSink csv(csv_path);
  if (!csv.ok()) {
    std::cerr << "Error: cannot open CSV file '" << csv_path << "'\n";
    return 1;
  }
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
      std::cerr << "link_bridge: timed out waiting for segment '" << segment << "'\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kAttachRetryMs));
  }

  std::uint64_t last_seq = 0;
  std::uint64_t shown = 0;
  wengine::link::LagStats lag;
  Clock::time_point last_new = Clock::now();
  for (;;) {
    wengine::link::LinkFrame wire{};
    if (reader->poll_newer(last_seq, wire)) {
      (void)wengine::link::lag_observe(lag, wire.frame_seq);
      wengine::link::Frame frame;
      if (!wengine::link::decode_frame(wire, frame).ok()) {
        std::cerr << "link_bridge: corrupt frame data\n";
        return 1;
      }
      csv.on_snapshot(wengine::link::snapshot_from_frame(frame));
      if (!csv.flush().ok()) {
        std::cerr << "Error: failed writing CSV file '" << csv_path << "'\n";
        return 1;
      }
      last_new = Clock::now();
      if (++shown == want_frames && want_frames != 0) {
        break;
      }
      continue;
    }
    if (timeout_ms != 0 && elapsed_ms(last_new) >= timeout_ms) {
      std::cerr << "link_bridge: timed out waiting for new frames\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
  }
  std::cout << "recorded " << shown << " frames to '" << csv_path << "' (missed "
            << lag.missed << ")\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    return usage_error("missing mode (play|record)");
  }
  const std::string_view mode = argv[1];
  if (mode == "play") {
    return run_play(argc, argv);
  }
  if (mode == "record") {
    return run_record(argc, argv);
  }
  return usage_error(std::string("unknown mode '") + std::string(mode) + "'");
}
