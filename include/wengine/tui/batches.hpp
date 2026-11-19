#pragma once

/**
 * @file
 * F4 run-builder registry. Builder data is defined one command per file
 * under `batches/`; register a new builder here and keep the registry
 * order stable. The TUI is a front end: builders compose one argv line
 * each for the shared CLI path (`sim run` / `sig run`), never parsing or
 * validating anything themselves. Consumers: the F4 builder browser in
 * `src/tui/panes_batches.cpp`, `tests/test_tui_batches.cpp`.
 */

#include "wengine/tui/batches/types.hpp"

#include "wengine/tui/batches/sim.hpp"
#include "wengine/tui/batches/sig.hpp"

#include <array>
#include <vector>

namespace wengine::tui::batches {

/**
 * @brief Factory building one run builder definition.
 *
 * @return The builder definition for one command (`sim` or `sig`).
 */
using BatchFactory = BatchDef (*)();

/**
 * @brief Single registration point, in the stable order shown by the F4
 *        browser.
 */
inline constexpr std::array<BatchFactory, kBatchCount> kBatchRegistry{{
    &definitions::sim,
    &definitions::sig,
}};

/**
 * @brief Instantiate every registered run builder.
 *
 * @return One BatchDef per registry entry, in F4 browser order.
 */
[[nodiscard]] inline std::vector<BatchDef> batch_defs() {
  std::vector<BatchDef> defs;
  defs.reserve(kBatchRegistry.size());
  for (const BatchFactory make_batch : kBatchRegistry) {
    defs.push_back(make_batch());
  }
  return defs;
}

} // namespace wengine::tui::batches
