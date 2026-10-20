#pragma once

// F4 run-builder registry. Builder data is defined one command per file
// under batches/; register a new builder here and keep the registry order
// stable.

#include "wengine/tui/batches/types.hpp"

#include "wengine/tui/batches/sim.hpp"
#include "wengine/tui/batches/sig.hpp"

#include <array>
#include <vector>

namespace wengine::tui::batches {

using BatchFactory = BatchDef (*)();

// Single registration point, in the stable order shown by the F4 browser.
inline constexpr std::array<BatchFactory, kBatchCount> kBatchRegistry{{
    &definitions::sim,
    &definitions::sig,
}};

[[nodiscard]] inline std::vector<BatchDef> batch_defs() {
  std::vector<BatchDef> defs;
  defs.reserve(kBatchRegistry.size());
  for (const BatchFactory make_batch : kBatchRegistry) {
    defs.push_back(make_batch());
  }
  return defs;
}

} // namespace wengine::tui::batches
