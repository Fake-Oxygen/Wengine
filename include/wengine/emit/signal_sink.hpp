#pragma once

// Machine-readable signal log (D2b-A): tidy CSV, one row per live source
// per snapshot (`step,time,sig_id,value,quantity`), header written once.
// Companion to CsvSink, which stays particle-shaped: `sim run` writes this
// log when the solver is `signal`, so each domain keeps its exact
// round-trip format instead of one widened schema that breaks both.
//
// Doubles use max_digits10 precision (exact round-trip, `==` on re-read);
// `sig_id` is the stable source serial in Snapshot slot order (serials are
// NOT ascending after slot recycling — validation requires uniqueness
// within a step, preserving writer order).
// Best-effort observer like CsvSink: sticky status, never throws.

#include "wengine/core/status.hpp"
#include "wengine/emit/csv_sink.hpp" // csv_detail field parsers (shared)
#include "wengine/emit/sink.hpp"
#include "wengine/signal/types.hpp"

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

class SignalsCsvSink : public ISink {
public:
  explicit SignalsCsvSink(std::string path, bool truncate = true) : path_(std::move(path)) {
    out_.open(path_, truncate ? std::ios::trunc : std::ios::app);
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
      return;
    }
    out_ << std::setprecision(std::numeric_limits<double>::max_digits10);
    if (truncate || out_.tellp() == std::streampos(0)) {
      out_ << "step,time,sig_id,value,quantity\n";
    }
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
    }
  }

  void on_snapshot(const world::Snapshot &s) override {
    if (!status_.ok()) {
      return;
    }
    for (std::size_t k = 0; k < s.signal_count(); ++k) {
      const auto qty = static_cast<signal::QuantityId>(s.sig_quantity[k]);
      out_ << s.step_index << ',' << s.sim_time << ',' << s.sig_serial[k] << ','
           << s.sig_value[k] << ',' << signal::quantity_suffix(qty) << '\n';
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

// Replay: parse a SignalsCsvSink log back into snapshots, one per step in
// file order. Co-located with the writer so format and parser never drift.
// Contract mirrors parse_csv_log: header byte-exact, steps strictly
// increasing, sig ids unique per step (file/slot order preserved — serials
// need not ascend after slot recycling), time identical across a step's
// rows (exact ==). Violations are kInvalidArgument with a line-numbered
// message; `out` is cleared on failure.
[[nodiscard]] inline core::Status
parse_signals_log(std::istream &in, std::vector<world::Snapshot> &out,
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
  if (line != "step,time,sig_id,value,quantity") {
    return invalid("bad header on line 1: '" + line + "'");
  }
  std::uint64_t line_no = 1;
  bool have_step = false;
  std::uint64_t cur_step = 0;
  world::Snapshot cur;
  while (std::getline(in, line)) {
    ++line_no;
    // Split exactly 5 comma fields.
    std::string fields[5];
    {
      std::size_t begin = 0;
      bool bad = false;
      for (int k = 0; k < 4; ++k) {
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
      fields[4] = line.substr(begin);
    }
    std::uint64_t step = 0;
    std::uint64_t id = 0;
    double time = 0.0;
    double value = 0.0;
    if (!csv_detail::parse_u64_field(fields[0], step) ||
        !csv_detail::parse_f64_field(fields[1], time) ||
        !csv_detail::parse_u64_field(fields[2], id) ||
        !csv_detail::parse_f64_field(fields[3], value)) {
      return invalid("malformed row on line " + std::to_string(line_no));
    }
    const auto qty = signal::parse_quantity(fields[4]);
    if (!qty.ok()) {
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
      // recycling (e.g. [2,1]), so validate uniqueness within the step and
      // preserve file order. Signal counts are small (linear scan).
      for (std::uint64_t seen : cur.sig_serial) {
        if (seen == id) {
          return invalid("duplicate sig id on line " + std::to_string(line_no));
        }
      }
    }
    cur.sig_serial.push_back(id);
    cur.sig_value.push_back(value);
    cur.sig_quantity.push_back(static_cast<std::uint8_t>(qty.value));
  }
  if (have_step) {
    out.push_back(cur);
  }
  return core::Status::success();
}

} // namespace wengine::emit
