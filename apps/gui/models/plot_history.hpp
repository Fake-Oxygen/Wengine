#pragma once

// Ring-buffered snapshot history: an emit::ISink observer feeding the
// graphs panel. Bounded (drop-oldest, matching the project policy for
// visualization consumers), cleared automatically when a fresh run starts
// (an incoming step that does not advance the series means reset +
// rebuild replayed step 0).
//
// Best-effort observer: on_snapshot never lets an exception reach the
// runner loop, so a starving/failed GUI can never perturb sim timing.
// Engine-only includes; unit-tested, including exactness against CsvSink
// output for identical snapshot streams.

#include "wengine/emit/sink.hpp"
#include "wengine/world/state.hpp"

#include <cstddef>
#include <vector>

namespace wengine::gui {

class PlotHistory : public emit::ISink {
 public:
  static constexpr std::size_t kCapacity = 1024;

  void on_snapshot(const world::Snapshot &s) override {
    try {
      if (!snaps_.empty() && s.step_index <= snaps_.back().step_index) {
        snaps_.clear();
      }
      if (snaps_.size() >= kCapacity) {
        snaps_.erase(snaps_.begin());
      }
      snaps_.push_back(s);
    } catch (...) {
      // Best-effort: drop the sample, never break the simulation loop.
    }
  }

  void clear() noexcept { snaps_.clear(); }
  [[nodiscard]] bool empty() const noexcept { return snaps_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return snaps_.size(); }
  [[nodiscard]] const std::vector<world::Snapshot> &snapshots() const noexcept {
    return snaps_;
  }

 private:
  std::vector<world::Snapshot> snaps_;
};

} // namespace wengine::gui
