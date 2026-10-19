#pragma once

// External frame sources for apps/gui: (b) SHM subscriber mode attached to
// a headless publisher, (c) CSV replay playback of a CsvSink or
// SignalsCsvSink log. (a) in-process interactive runs stay in SimController,
// untouched.
//
// Both classes are engine-only (no ImGui, SDL, or Vulkan): views consume
// their link::Frame output exactly like in-process frames, so graphs, 2D
// canvas, and 3D viewport follow external runs with zero view changes.
// Isolation contract (task requirement): reader lag and sink failure live
// entirely on the display side — ShmSource owns no Runner and no sim
// state, CsvReplay owns parsed snapshots only. A lagging subscriber skips
// frames (drop-oldest, LagStats-observed); it can never slow, stall, or
// perturb the publisher or any sim.
//
// SHM notes: attach is lazy (ensure_attached each UI tick), so the GUI
// auto-connects when the headless run starts; missing/unready segments
// read as "waiting", refusals/errors as messages with the reader's own
// status. The wire Frame decode is solver-agnostic, so SHM replay of
// signal runs needs no new code (same shared Frame path as TCP/UDP).
// CSV notes: logs carry no provenance, so replay sniffs the header —
// particle logs adapt through the particle adapter, signal logs through
// the signal adapter (explicit set_adapter after load still overrides).
// Playback is paced by the log's own time deltas x rate, parks at the end
// like SimController parks at its step budget, and Reset + Play replays
// deterministically for identical wall-tick sequences. The signal parser
// preserves file (slot) order with per-step uniqueness, so slot-recycled
// serial orders replay exactly like fresh-serial logs.

#include "wengine/core/status.hpp"
#include "wengine/emit/csv_sink.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/link/shm_reader.hpp"
#include "wengine/world/state.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::gui {

// Frame-source selector shared by main.cpp (engine behavior) and the
// source panel (UI). InProcess keeps today's exact behavior.
enum class SourceMode : std::uint8_t { kInProcess, kShm, kCsv };

class ShmSource {
 public:
  ShmSource() = default;

  // (Re)target a segment; changing the name drops the attach, cursor,
  // frame, lag, and error. Empty name = idle. Call every tick with the
  // panel text; same-name calls are no-ops.
  void set_segment(std::string_view name) {
    if (std::string_view(segment_) == name) {
      return;
    }
    segment_ = std::string(name);
    reader_.reset();
    cursor_ = 0;
    frame_ = link::Frame{};
    has_frame_ = false;
    lag_ = link::LagStats{};
    error_.clear();
  }

  // Attach when needed. Missing/unready segments stay pending (success,
  // attached() false, no error — the UI shows "waiting"); refusals and
  // syscall errors set error_ and return the reader's own status.
  [[nodiscard]] core::Status ensure_attached() {
    if (reader_.has_value() && reader_->ok()) {
      error_.clear();
      return core::Status::success();
    }
    if (segment_.empty()) {
      reader_.reset();
      error_.clear();
      return core::Status::success();
    }
    if (!link::valid_segment_name(segment_)) {
      reader_.reset();
      error_ = "invalid segment name (want /name)";
      return {core::StatusCode::kInvalidArgument};
    }
    reader_.emplace(segment_);
    if (reader_->ok()) {
      error_.clear();
      return core::Status::success();
    }
    const link::AttachState st = reader_->state();
    const link::HeaderReport rep = reader_->report();
    const core::Status rst = reader_->status();
    const int err = reader_->last_errno();
    reader_.reset();
    if (st == link::AttachState::kMissing || st == link::AttachState::kUnready) {
      error_.clear();
      return core::Status::success();
    }
    if (st == link::AttachState::kRefused) {
      error_ = link::describe_refusal(segment_, rep);
    } else if (err != 0) {
      error_ = "cannot attach to segment '" + segment_ + "': " + std::strerror(err);
    } else {
      error_ = "cannot attach to segment '" + segment_ + "'";
    }
    return rst;
  }

  // Poll the newest frame; true when a new one arrived (frame() updated,
  // lag observed). Latest-only: skipped spans count as missed, display
  // never back-pressures the publisher.
  bool poll() {
    (void)ensure_attached();
    if (!attached()) {
      return false;
    }
    link::LinkFrame wire{};
    if (!reader_->poll_newer(cursor_, wire)) {
      return false;
    }
    (void)link::lag_observe(lag_, wire.frame_seq);
    link::Frame decoded;
    if (!link::decode_frame(wire, decoded).ok()) {
      error_ = "corrupt frame data";
      return false;
    }
    frame_ = std::move(decoded);
    has_frame_ = true;
    error_.clear();
    return true;
  }

  // Drop the attach and cursor (keeps the segment name): re-attaches on
  // the next poll. Needed when the publisher restarts under the SAME name
  // — the old mapping keeps showing the previous run's frozen frames and
  // the cursor would otherwise never advance past them (POSIX fd
  // semantics: the name points at a new object we never opened). The
  // shared history auto-clears on the new run's step regression.
  void reconnect() {
    reader_.reset();
    cursor_ = 0;
    frame_ = link::Frame{};
    has_frame_ = false;
    lag_ = link::LagStats{};
    error_.clear();
  }

  [[nodiscard]] bool attached() const noexcept {
    return reader_.has_value() && reader_->ok();
  }
  [[nodiscard]] bool has_frame() const noexcept { return has_frame_; }
  [[nodiscard]] const link::Frame &frame() const noexcept { return frame_; }
  [[nodiscard]] const link::LagStats &lag() const noexcept { return lag_; }
  [[nodiscard]] const std::string &segment() const noexcept { return segment_; }
  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

 private:
  std::string segment_;
  std::optional<link::ShmReader> reader_;
  std::uint64_t cursor_ = 0;
  link::Frame frame_;
  bool has_frame_ = false;
  link::LagStats lag_{};
  std::string error_;
};

class CsvReplay {
 public:
  CsvReplay() = default;

  // Load from an open stream (or directly from a file: GUI owns file I/O,
  // so load_file keeps the "cannot open" error beside the parse errors).
  // Replaces any previous log, parks at snapshot 0, stops playback. The
  // header selects the parser + adapter (particle vs signal logs); empty
  // signal ticks replay as count-only frames, exactly like live ones.
  [[nodiscard]] core::Status load(std::istream &in) {
    std::ostringstream raw;
    raw << in.rdbuf();
    if (!in && !in.eof()) {
      clear();
      error_ = "cannot read log";
      last_ = {core::StatusCode::kInternal};
      return last_;
    }
    const std::string text = raw.str();
    const std::string header = text.substr(0, text.find('\n'));
    const bool is_signal = header == "step,time,sig_id,value,quantity";
    std::vector<world::Snapshot> snaps;
    std::string error;
    core::Status st{core::StatusCode::kInternal};
    if (is_signal) {
      std::istringstream sig_in(text);
      st = emit::parse_signals_log(sig_in, snaps, error);
    } else {
      std::istringstream csv_in(text);
      st = emit::parse_csv_log(csv_in, snaps, error);
    }
    if (!st.ok()) {
      clear();
      error_ = error;
      last_ = st;
      return st;
    }
    snaps_ = std::move(snaps);
    adapter_ = is_signal ? &link::frame_from_signal_snapshot : &link::frame_from_snapshot;
    cursor_ = 0;
    acc_ = 0.0;
    playing_ = false;
    error_.clear();
    last_ = core::Status::success();
    refresh();
    return last_;
  }

  [[nodiscard]] core::Status load_file(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
      clear();
      error_ = "cannot open file '" + path + "'";
      last_ = {core::StatusCode::kInternal};
      return last_;
    }
    return load(in);
  }

  void set_adapter(link::FrameAdapter adapter) {
    if (adapter != nullptr) {
      adapter_ = adapter;
      refresh();
    }
  }

  // Wall-clock pacing into snapshot time (play mode): folds wall_dt into
  // fixed log-time deltas x rate_, like Runner folds wall time into dt.
  // Parks at the final snapshot and auto-pauses, mirroring SimController's
  // budget park; Reset + Play replays from the top.
  void advance_frame(double wall_dt_seconds) {
    if (!playing_ || snaps_.empty() || !(wall_dt_seconds >= 0.0)) {
      return;
    }
    acc_ += wall_dt_seconds;
    while (cursor_ + 1 < snaps_.size()) {
      const double delta =
          (snaps_[cursor_ + 1].sim_time - snaps_[cursor_].sim_time) / rate_;
      if (!(delta > 0.0)) {
        ++cursor_; // degenerate log timing: step without consuming
        continue;
      }
      if (acc_ < delta) {
        break;
      }
      acc_ -= delta;
      ++cursor_;
    }
    if (cursor_ + 1 >= snaps_.size()) {
      playing_ = false;
    }
    refresh();
  }

  // Exact single-step; parked at the end it holds the final snapshot.
  void step_once() {
    if (cursor_ + 1 < snaps_.size()) {
      ++cursor_;
      acc_ = 0.0;
      refresh();
    }
  }

  void reset() {
    cursor_ = 0;
    acc_ = 0.0;
    refresh();
  }

  void set_playing(bool playing) noexcept { playing_ = playing; }
  [[nodiscard]] bool playing() const noexcept { return playing_; }
  void set_rate(double rate) {
    if (rate > 0.0) {
      rate_ = rate; // non-positive rates ignored (keeps current pacing)
    }
  }
  [[nodiscard]] double rate() const noexcept { return rate_; }

  [[nodiscard]] bool has_frame() const noexcept { return has_frame_; }
  [[nodiscard]] const link::Frame &frame() const noexcept { return frame_; }
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
  [[nodiscard]] std::size_t count() const noexcept { return snaps_.size(); }
  [[nodiscard]] bool ok() const noexcept { return last_.ok(); }
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

 private:
  void clear() noexcept {
    snaps_.clear();
    cursor_ = 0;
    acc_ = 0.0;
    playing_ = false;
    frame_ = link::Frame{};
    has_frame_ = false;
  }

  void refresh() {
    if (snaps_.empty()) {
      frame_ = link::Frame{};
      has_frame_ = false;
      return;
    }
    frame_ = adapter_(snaps_[cursor_]);
    has_frame_ = true;
  }

  std::vector<world::Snapshot> snaps_;
  std::size_t cursor_ = 0;
  double acc_ = 0.0; // wall seconds banked toward the next snapshot delta
  double rate_ = 1.0; // playback speed vs log time (must stay > 0)
  bool playing_ = false;
  link::FrameAdapter adapter_ = &link::frame_from_snapshot;
  link::Frame frame_;
  bool has_frame_ = false;
  core::Status last_{};
  std::string error_;
};

} // namespace wengine::gui
