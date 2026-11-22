#pragma once

/**
 * @file
 * Optimizer option vocabulary (levels, targets, parse) plus the shared
 * failure vocabulary (BuildError): the shared vocabulary seam of the
 * optimize stage of the logic pipeline (netlist parse -> state -> eval
 * -> layout/topo -> optimize). Extracted from optimize.hpp so
 * pass/emit/cleanup headers stay standalone without a circular
 * include.
 *
 * Dependency rules: header-only, depends only on core/status. Logic is
 * a standalone domain — never depends on CLI types. Consumers:
 * optimize.hpp (the entry point), every optimize_* pass header, the
 * CLI `optimize` verb, and the logic optimizer tests.
 */

#include "wengine/core/status.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::logic {

/**
 * @brief Optimization level: which passes run (docs/logic.md section 6).
 *
 * kO0 folds constants and eliminates dead gates (never grows the net);
 * kO1 adds De Morgan bubble-push canonical form (still mixed gates);
 * kO2 converts fully to the single target kind.
 */
enum class OptLevel : std::uint8_t {
  /** @brief Constant folding + identity + DCE + exact small-block simplification. */
  kO0 = 0,
  /** @brief O0 plus De Morgan fusion to canonical form (mixed gates). */
  kO1 = 1,
  /** @brief O1 plus full NAND-only/NOR-only conversion. */
  kO2 = 2,
};

/**
 * @brief O2 conversion target vocabulary: which single kind the whole
 *        net is rebuilt from.
 */
enum class OptTarget : std::uint8_t {
  /** @brief Convert everything to NAND gates (SOP-style resynthesis). */
  kNand = 0,
  /** @brief Convert everything to NOR gates (POS-style resynthesis). */
  kNor = 1,
};

/**
 * @brief One optimizer invocation's options: level plus the O2 target
 *        kind.
 */
struct OptimizeOptions {
  /** @brief Pass level to run (O0/O1/O2; see OptLevel). */
  OptLevel level = OptLevel::kO0;
  /** @brief Single-kind target for O2; ignored at O0/O1. */
  OptTarget target = OptTarget::kNand; // read only at O2
};

/**
 * @brief Parse an optimization level.
 *
 * Case-insensitive "O0|O1|O2" (exactly two characters).
 *
 * @param[in] text CLI spelling to parse.
 * @return kOk with the level on success; kInvalidArgument with a
 *         want-list message otherwise.
 */
[[nodiscard]] inline core::Result<OptLevel> parse_opt_level(std::string_view text) noexcept {
  if (text.size() == 2 && (text[0] == 'O' || text[0] == 'o') && text[1] >= '0' && text[1] <= '2') {
    const auto v = static_cast<std::uint8_t>(text[1] - '0');
    return core::Result<OptLevel>::success(static_cast<OptLevel>(v));
  }
  return core::Result<OptLevel>::failure("bad opt level: want O0|O1|O2");
}

/**
 * @brief Parse an O2 conversion target.
 *
 * Case-insensitive "nand"|"nor".
 *
 * @param[in] text CLI spelling to parse.
 * @return kOk with the target on success; kInvalidArgument with a
 *         want-list message otherwise.
 */
[[nodiscard]] inline core::Result<OptTarget> parse_opt_target(std::string_view text) noexcept {
  if (text.size() == 4) {
    const bool nand = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'A' || text[1] == 'a') &&
                      (text[2] == 'N' || text[2] == 'n') && (text[3] == 'D' || text[3] == 'd');
    if (nand) {
      return core::Result<OptTarget>::success(OptTarget::kNand);
    }
  } else if (text.size() == 3) {
    const bool nor = (text[0] == 'N' || text[0] == 'n') && (text[1] == 'O' || text[1] == 'o') && (text[2] == 'R' || text[2] == 'r');
    if (nor) {
      return core::Result<OptTarget>::success(OptTarget::kNor);
    }
  }
  return core::Result<OptTarget>::failure("bad opt target: want nand|nor");
}

} // namespace wengine::logic

namespace wengine::logic::detail {

/**
 * @brief Failure modes of the O2 fresh build (mapped to loud messages
 *        once, in optimize(), so every phase stays a plain predicate).
 *
 * Capacity-free: failure modes never scale. Lives here (shared
 * vocabulary) so the Pass fixpoint driver in optimize_pass.hpp can
 * report the same modes without including the O2 build header.
 */
enum class BuildError : std::uint8_t {
  /** @brief No failure. */
  kNone = 0,
  /** @brief Gate budget exceeded (kMaxGates). */
  kBudget = 1, // gate budget exceeded (kMaxGates)
  /** @brief Internal wiring error (emit-map gap, bad indices). */
  kWiring = 2, // internal wiring error (emit-map gap, bad indices)
  /** @brief Topology error inside post-cleanup. */
  kTopo = 3,   // topology error inside post-cleanup
  /** @brief Internal iteration bound. */
  kIter = 4,   // internal iteration bound
};

/**
 * @brief Loud message for a fresh-build failure mode (reported once by
 *        optimize()).
 *
 * Capacity-free: messages never scale.
 *
 * @param[in] e Failure mode to name.
 * @return String-literal message (zero-copy, no allocation).
 */
[[nodiscard]] inline const char *build_error_message(BuildError e) noexcept {
  switch (e) {
  case BuildError::kNone:
    return "optimize: topology error";
  case BuildError::kBudget:
    return "optimize: gate budget exceeded (kMaxGates)";
  case BuildError::kWiring:
    return "optimize: internal wiring error";
  case BuildError::kTopo:
    return "optimize: topology error";
  case BuildError::kIter:
    return "optimize: internal iteration bound";
  }
  return "optimize: topology error";
}

} // namespace wengine::logic::detail
