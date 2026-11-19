#pragma once

/**
 * @file
 * Human-readable snapshot sink: one line per snapshot on any ostream.
 * Best-effort observer for the runner loop; keeps no state, owns nothing.
 * Depends on emit/sink only. Consumed by the CLI for human output; machine
 * readers use emit/csv_sink.hpp instead.
 */
// Human-readable snapshot sink: one line per snapshot on any ostream.
// Best-effort observer for the runner loop (see core/runner.hpp): never
// throws under default stream settings, keeps no state, owns nothing.
// For machine-readable output see emit/csv_sink.hpp.

#include "wengine/emit/sink.hpp"

#include <iostream>
#include <ostream>

namespace wengine::emit {

/**
 * @brief Prints one "step <i> t <s>s particles <n>" line per snapshot.
 * @note Never throws under default stream settings; owns nothing (the
 * stream must outlive the sink).
 */
class ConsoleSink : public ISink {
public:
/**
 * @brief Attach to a stream.
 * @param[in,out] out destination stream (must outlive the sink; never owned).
 */
  explicit ConsoleSink(std::ostream &out = std::cout) : out_(&out) {}

/**
 * @brief Print one line for the snapshot.
 * @param[in] s emitted snapshot.
 */
  void on_snapshot(const world::Snapshot &s) override {
    (*out_) << "step " << s.step_index << " t " << s.sim_time << "s particles "
            << s.particle_count() << '\n';
  }

private:
  std::ostream *out_;
};

} // namespace wengine::emit
