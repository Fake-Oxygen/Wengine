#pragma once

// Human-readable snapshot sink: one line per snapshot on any ostream.
// Best-effort observer for the runner loop (see core/runner.hpp): never
// throws under default stream settings, keeps no state, owns nothing.
// For machine-readable output see emit/csv_sink.hpp.

#include "wengine/emit/sink.hpp"

#include <iostream>
#include <ostream>

namespace wengine::emit {

class ConsoleSink : public ISink {
public:
  explicit ConsoleSink(std::ostream &out = std::cout) : out_(&out) {}

  void on_snapshot(const world::Snapshot &s) override {
    (*out_) << "step " << s.step_index << " t " << s.sim_time << "s particles "
            << s.particle_count() << '\n';
  }

private:
  std::ostream *out_;
};

} // namespace wengine::emit
