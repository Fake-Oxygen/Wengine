#pragma once

// Machine-readable sequential log (005-US3): tidy CSV, one row per live
// flop per snapshot (`step,time,seq_id,value`), header written once.
// Companion to SignalsCsvSink, which stays signal-shaped: `sim run` writes
// this log when the solver is `logic`, so each domain keeps its exact
// round-trip format instead of one widened schema that breaks both.
//
// `seq_id` is the stable flop serial in Snapshot slot order (serials are
// NOT ascending after slot recycling — validation requires uniqueness
// within a step, preserving writer order); `value` is Q (0/1).
// Doubles use max_digits10 precision (exact round-trip, `==` on re-read).
// Best-effort observer like SignalsCsvSink: sticky status, never throws.

#include "wengine/core/status.hpp"
#include "wengine/emit/csv_sink.hpp" // csv_detail field parsers (shared)
#include "wengine/emit/sink.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <istream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wengine::emit {

class LogicCsvSink : public ISink {
 public:
  explicit LogicCsvSink(std::string path, bool truncate = true) : path_(std::move(path)) {
    out_.open(path_, truncate ? std::ios::trunc : std::ios::app);
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
      return;
    }
    out_ << std::setprecision(std::numeric_limits<double>::max_digits10);
    if (truncate || out_.tellp() == std::streampos(0)) {
      out_ << "step,time,seq_id,value\n";
    }
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
    }
  }

  void on_snapshot(const world::Snapshot &s) override {
    if (!status_.ok()) {
      return;
    }
    for (std::size_t k = 0; k < s.seq_count(); ++k) {
      out_ << s.step_index << ',' << s.sim_time << ',' << s.seq_serial[k] << ','
           << static_cast<unsigned>(s.seq_value[k]) << '\n';
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

// Replay: parse a LogicCsvSink log back into snapshots, one per step in
// file order. Co-located with the writer so format and parser never drift.
// Contract mirrors parse_signals_log: header byte-exact, steps strictly
// increasing, seq ids unique per step (file/slot order preserved — serials
// need not ascend after slot recycling), time identical across a step's
// rows (exact ==), value 0/1. Violations are kInvalidArgument with a
// line-numbered message; `out` is cleared on failure.
[[nodiscard]] inline core::Status
parse_logic_log(std::istream &in, std::vector<world::Snapshot> &out,
                std::string &error) {
  out.clear();
  error.clear();
  const auto invalid = [&](const std::string &msg) {
    out.clear();
    error = msg;
    return core::Status{core::StatusCode::kInvalidArgument};
  };
  std::string line;
  if (!std::getline(in, line)) {
    return invalid("empty log: missing header");
  }
  if (line != "step,time,seq_id,value") {
    return invalid("bad header on line 1: '" + line + "'");
  }
  std::uint64_t line_no = 1;
  bool have_step = false;
  std::uint64_t cur_step = 0;
  world::Snapshot cur;
  while (std::getline(in, line)) {
    ++line_no;
    // Split exactly 4 comma fields.
    std::string fields[4];
    {
      std::size_t begin = 0;
      bool bad = false;
      for (int k = 0; k < 3; ++k) {
        const std::size_t comma = line.find(',', begin);
        if (comma == std::string::npos) {
          bad = true;
          break;
        }
        fields[k] = line.substr(begin, comma - begin);
        begin = comma + 1;
      }
      if (bad || line.find(',', begin) != std::string::npos) {
        return invalid("malformed row on line " + std::to_string(line_no));
      }
      fields[3] = line.substr(begin);
    }
    std::uint64_t step = 0;
    std::uint64_t id = 0;
    std::uint64_t value = 0;
    double time = 0.0;
    if (!csv_detail::parse_u64_field(fields[0], step) ||
        !csv_detail::parse_f64_field(fields[1], time) ||
        !csv_detail::parse_u64_field(fields[2], id) ||
        !csv_detail::parse_u64_field(fields[3], value) || value > 1) {
      return invalid("malformed row on line " + std::to_string(line_no));
    }
    if (!have_step || step != cur_step) {
      if (have_step) {
        if (step <= cur_step) {
          return invalid("step not increasing on line " + std::to_string(line_no));
        }
        out.push_back(cur);
        cur = world::Snapshot{};
      }
      cur_step = step;
      have_step = true;
      cur.step_index = step;
      cur.sim_time = time;
    } else {
      if (time != cur.sim_time) {
        return invalid("time differs within step on line " + std::to_string(line_no));
      }
      // Slot order, not sorted order: serials need not ascend after slot
      // recycling, so validate uniqueness within the step and preserve
      // file order. Flop counts are small (linear scan).
      for (std::uint64_t seen : cur.seq_serial) {
        if (seen == id) {
          return invalid("duplicate seq id on line " + std::to_string(line_no));
        }
      }
    }
    cur.seq_serial.push_back(id);
    cur.seq_value.push_back(static_cast<std::uint8_t>(value));
  }
  if (have_step) {
    out.push_back(cur);
  }
  return core::Status::success();
}

} // namespace wengine::emit
