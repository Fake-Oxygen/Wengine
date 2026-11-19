#pragma once

/**
 * @file
 * Machine-readable particle snapshot log: tidy CSV, header written once,
 * co-located replay parser. Depends on core/status and emit/sink only;
 * never on solvers. Consumed by sim run for the particle domain and by
 * the link bridge replaying logs onto the bus.
 */
// Machine-readable snapshot log: tidy CSV, one row per particle per
// snapshot (`step,time,slot,entity_index,entity_generation,x,y,vx,vy`),
// header written once on open. v3 identity columns (PHYSI particle
// identity): fill_snapshot snapshots always carry index+generation and
// round-trip exactly; snapshots without identity (legacy/manual) write the
// kMissingIdentity sentinel (0xFFFFFFFF) for both columns, which parses
// back to unavailable (empty identity vectors) — never a fabricated
// stable ID. Legacy 7-column logs (`step,time,slot,x,y,vx,vy`) still parse
// as unavailable identity. Doubles use max_digits10 precision, so a log
// round-trips exactly and doubles as the deterministic replay format (see
// the future link bridge).
//
// Best-effort observer for the runner loop: I/O errors are sticky
// (status flips to kInternal, further snapshots are skipped, nothing
// throws). `ok()` is a state query (predicate, not an error return);
// `flush()` reports the sticky failure as `core::Status` (unified
// convention). Truncates by default; pass truncate = false to append
// across runs (header is then written only when starting an empty file).

#include "wengine/core/status.hpp"
#include "wengine/emit/sink.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <istream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace wengine::emit {

/**
 * @brief Tidy particle CSV writer: one row per particle per snapshot.
 * @note Best-effort observer: I/O errors are sticky (status flips to
 * kInternal, further snapshots are skipped, nothing throws). The log
 * round-trips exactly (max_digits10) and doubles as the deterministic
 * replay format, but it is output replay, never a restart checkpoint
 * (checkpoints are the versioned binary save_state/load_state).
 */
class CsvSink : public ISink {
public:
  // Sentinel for unavailable particle identity in the CSV columns.
  // Matches world::Entity::kInvalidIndex: never a live slot.
/**
 * @brief Sentinel for unavailable particle identity in the CSV columns.
 * @note Matches world::Entity::kInvalidIndex: never a live slot; parses back to unavailable identity.
 */
  static constexpr std::uint32_t kMissingIdentity = 0xFFFFFFFFu;

/**
 * @brief Open the log file and write the header.
 * @param[in] path destination file.
 * @param[in] truncate true truncates; false appends (the header is then written only when
 * starting an empty file).
 * @note A failed open is a sticky kInternal status, never a throw.
 */
  explicit CsvSink(std::string path, bool truncate = true) : path_(std::move(path)) {
    out_.open(path_, truncate ? std::ios::trunc : std::ios::app);
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
      return;
    }
    out_ << std::setprecision(std::numeric_limits<double>::max_digits10);
    if (truncate || out_.tellp() == std::streampos(0)) {
      out_ << "step,time,slot,entity_index,entity_generation,x,y,vx,vy\n";
    }
    if (!out_) {
      status_ = {core::StatusCode::kInternal};
    }
  }

/**
 * @brief Append one row per particle for the snapshot.
 * @param[in] s emitted snapshot.
 * @note Snapshots without identity write kMissingIdentity for both columns.
 */
  void on_snapshot(const world::Snapshot &s) override {
    if (!status_.ok()) {
      return;
    }
    const bool have_id = s.has_particle_identity();
    for (std::size_t k = 0; k < s.particle_count(); ++k) {
      const std::uint32_t idx = have_id ? s.particle_index[k] : kMissingIdentity;
      const std::uint32_t gen = have_id ? s.particle_generation[k] : kMissingIdentity;
      out_ << s.step_index << ',' << s.sim_time << ',' << k << ',' << idx << ',' << gen
           << ',' << s.pos_x[k] << ',' << s.pos_y[k] << ',' << s.vel_x[k] << ',' << s.vel_y[k] << '\n';
      if (!out_) {
        status_ = {core::StatusCode::kInternal};
        return;
      }
      ++rows_;
    }
  }

/** @brief Sticky health flag. @return true while no I/O failure occurred (state query, not an error return). */
  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
/** @brief Sticky status. @return kOk, or kInternal once I/O has failed. */
  [[nodiscard]] core::Status status() const noexcept { return status_; }
/** @brief Data rows appended, ever. @return Count of written particle rows. */
  [[nodiscard]] std::size_t rows_written() const noexcept { return rows_; }
/** @brief Destination file. @return The path passed to the constructor. */
  [[nodiscard]] const std::string &path() const noexcept { return path_; }

  // Push buffered rows to the file (for tailing; also happens on close).
  // Returns the sticky status (kInternal once I/O has failed).
/**
 * @brief Push buffered rows to the file (for tailing; also happens on close).
 * @return The sticky status (kInternal once I/O has failed).
 */
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

// Replay: parse a CsvSink log back into snapshots, one per step in file
// order (see examples/link_bridge.cpp, which plays these onto the
// wengine-link bus). Co-located with the writer so format and parser can
// never drift apart: v3 identity header plus legacy support, column order
// and dense slot layout below mirror on_snapshot exactly.
//
// Contract: header is v3 or legacy (see below); steps strictly increasing;
// slots dense 0..n-1 in row order per step; time identical across a step's
// rows (exact ==: same text parses to the same double). Empty snapshots
// have no rows in this format and therefore do not survive a CSV round
// trip — parse yields only steps present in the file (documented
// limitation, consistent both directions). Any violation is
// kInvalidArgument with a human line-numbered message in `error` (edge
// code may allocate; the sim hot path never calls this). On failure `out`
// is cleared, like load_state. Doubles use strtod full-consumption, so
// max_digits10 logs round-trip exactly (bitwise, not approximate).
namespace csv_detail {

// Strict unsigned: digits only (strtoull alone would accept '-' wrap and
// '+' prefixes, which this log format never emits).
/**
 * @brief Strict unsigned field parse: digits only (strtoull alone would accept '-' wrap and '+' prefixes).
 * @param[in] text field text.
 * @param[out] v parsed value on true.
 * @return true on success, false otherwise.
 */
[[nodiscard]] inline bool parse_u64_field(const std::string &text,
                                          std::uint64_t &v) noexcept {
  if (text.empty() || text.front() < '0' || text.front() > '9') {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long r = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  v = static_cast<std::uint64_t>(r);
  return true;
}

/**
 * @brief Strict double field parse via full-consumption strtod.
 * @param[in] text field text.
 * @param[out] v parsed value on true.
 * @return true on success, false otherwise.
 */
[[nodiscard]] inline bool parse_f64_field(const std::string &text,
                                          double &v) noexcept {
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double r = std::strtod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    return false;
  }
  v = r;
  return true;
}

// Split a row into fields; returns field count (7 legacy or 9 v3) or 0 on
// any other shape (views share no tolerance here: loud beats lossy).
/**
 * @brief Split a CSV row into fields (7 legacy or 9 v3); any other shape is loud, not tolerated.
 * @param[in] line raw row text.
 * @param[out] fields parsed fields.
 * @return Field count (7 or 9), or 0 on any other shape.
 */
[[nodiscard]] inline std::size_t split_row(const std::string &line, std::string fields[9]) {
  std::size_t begin = 0;
  std::size_t n = 0;
  while (n < 9) {
    const std::size_t comma = line.find(',', begin);
    if (comma == std::string::npos) {
      break;
    }
    fields[n++] = line.substr(begin, comma - begin);
    begin = comma + 1;
  }
  if (n > 8) {
    return 0;
  }
  fields[n++] = line.substr(begin);
  if (n != 7 && n != 9) {
    return 0;
  }
  return n;
}

} // namespace csv_detail

/**
 * @brief Replay a CsvSink log back into snapshots, one per step in file order.
 * @param[in] in log stream (v3 identity header or legacy 7-column header).
 * @param[out] out parsed snapshots; cleared on failure.
 * @param[out] error human line-numbered message on failure (edge code may allocate; the sim hot path never calls this).
 * @return kOk, or kInvalidArgument for a bad header, non-increasing steps, non-dense slots,
 * time differing within a step, or malformed rows.
 * @note Steps strictly increase; slots are dense 0..n-1 in row order per step; time is
 * identical across a step's rows (exact ==: same text parses to the same double).
 * Empty snapshots have no rows and do not survive a CSV round trip (documented limitation).
 * Doubles use max_digits10 full consumption, so logs round-trip exactly (bitwise, not approximate).
 */
[[nodiscard]] inline core::Status parse_csv_log(std::istream &in, std::vector<world::Snapshot> &out, std::string &error) {
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
  const bool is_v3 = (line == "step,time,slot,entity_index,entity_generation,x,y,vx,vy");
  const bool is_legacy = (line == "step,time,slot,x,y,vx,vy");
  if (!is_v3 && !is_legacy) {
    return invalid("bad header on line 1: '" + line + "'");
  }
  std::uint64_t line_no = 1;
  bool have_step = false;
  std::uint64_t cur_step = 0;
  world::Snapshot cur;
  bool cur_has_id = true;
  auto flush_step = [&]() {
    if (!cur_has_id) {
      cur.particle_index.clear();
      cur.particle_generation.clear();
    }
  };
  while (std::getline(in, line)) {
    ++line_no;
    std::string fields[9];
    const std::size_t nfields = csv_detail::split_row(line, fields);
    if (nfields == 0) {
      return invalid("malformed row on line " + std::to_string(line_no));
    }
    if ((is_v3 && nfields != 9) || (is_legacy && nfields != 7)) {
      return invalid("malformed row on line " + std::to_string(line_no));
    }
    std::uint64_t step = 0;
    std::uint64_t slot = 0;
    std::uint64_t eidx = 0;
    std::uint64_t egen = 0;
    double time = 0.0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    bool ok = csv_detail::parse_u64_field(fields[0], step) && csv_detail::parse_f64_field(fields[1], time) && csv_detail::parse_u64_field(fields[2], slot);
    std::size_t vbase = 3;
    if (ok && is_v3) {
      ok = csv_detail::parse_u64_field(fields[3], eidx) && csv_detail::parse_u64_field(fields[4], egen) && eidx <= 0xFFFFFFFFull && egen <= 0xFFFFFFFFull;
      vbase = 5;
    }
    if (ok) {
      ok = csv_detail::parse_f64_field(fields[vbase + 0], x) &&
           csv_detail::parse_f64_field(fields[vbase + 1], y) &&
           csv_detail::parse_f64_field(fields[vbase + 2], vx) &&
           csv_detail::parse_f64_field(fields[vbase + 3], vy);
    }
    if (!ok) {
      return invalid("malformed row on line " + std::to_string(line_no));
    }
    if (!have_step || step != cur_step) {
      if (have_step) {
        if (step <= cur_step) {
          return invalid("step not increasing on line " + std::to_string(line_no));
        }
        flush_step();
        out.push_back(cur);
        cur = world::Snapshot{};
      }
      cur_step = step;
      have_step = true;
      cur.step_index = step;
      cur.sim_time = time;
      cur_has_id = is_v3;
    } else if (time != cur.sim_time) {
      return invalid("time differs within step on line " + std::to_string(line_no));
    }
    if (slot != cur.pos_x.size()) {
      return invalid("non-dense slot on line " + std::to_string(line_no));
    }
    cur.pos_x.push_back(x);
    cur.pos_y.push_back(y);
    cur.vel_x.push_back(vx);
    cur.vel_y.push_back(vy);
    if (is_v3) {
      constexpr std::uint64_t kMissing = 0xFFFFFFFFull;
      if (eidx == kMissing || egen == kMissing) {
        cur_has_id = false;
      } else if (cur_has_id) {
        cur.particle_index.push_back(static_cast<std::uint32_t>(eidx));
        cur.particle_generation.push_back(static_cast<std::uint32_t>(egen));
      }
    }
  }
  if (have_step) {
    flush_step();
    out.push_back(cur);
  }
  return core::Status::success();
}

} // namespace wengine::emit
