#pragma once

// Machine-readable snapshot log: tidy CSV, one row per particle per
// snapshot (`step,time,slot,x,y,vx,vy`), header written once on open.
// Doubles use max_digits10 precision, so a log round-trips exactly and
// doubles as the deterministic replay format (see the future link bridge).
//
// Best-effort observer for the runner loop: I/O errors are sticky
// (status flips to kInternal, further snapshots are skipped, nothing
// throws). `ok()` is a state query (predicate, not an error return);
// `flush()` reports the sticky failure as `core::Status` (unified
// convention). Truncates by default; pass truncate = false to append
// across runs (header is then written only when starting an empty file).

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>
#include <utility>

namespace wengine::emit {

class CsvSink : public ISink {
public:
  explicit CsvSink(std::string path, bool truncate = true) : path_(std::move(path)) {
    out_.open(path_, truncate ? std::ios::trunc : std::ios::app);
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
      return;
    }
    out_ << std::setprecision(std::numeric_limits<double>::max_digits10);
    if (truncate || out_.tellp() == std::streampos(0)) {
      out_ << "step,time,slot,x,y,vx,vy\n";
    }
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
    }
  }

  void on_snapshot(const world::Snapshot &s) override {
    if (!status_.ok()) {
      return;
    }
    for (std::size_t k = 0; k < s.particle_count(); ++k) {
      out_ << s.step_index << ',' << s.sim_time << ',' << k << ',' << s.pos_x[k] << ','
           << s.pos_y[k] << ',' << s.vel_x[k] << ',' << s.vel_y[k] << '\n';
      if (!out_) {
        status_ = {core::StatusCode::kInternal};
        return;
      }
      ++rows_;
    }
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] core::Status status() const noexcept { return status_; }
  [[nodiscard]] std::size_t rows_written() const noexcept { return rows_; }
  [[nodiscard]] const std::string &path() const noexcept { return path_; }

  // Push buffered rows to the file (for tailing; also happens on close).
  // Returns the sticky status (kInternal once I/O has failed).
  core::Status flush() {
    if (!status_.ok()) {
      return status_;
    }
    out_.flush();
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
    }
    return status_;
  }

private:
  std::string path_;
  std::ofstream out_;
  core::Status status_{};
  std::size_t rows_{0};
};

} // namespace wengine::emit
