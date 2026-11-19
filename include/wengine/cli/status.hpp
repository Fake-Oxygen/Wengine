#pragma once

/**
 * @file
 * Single CLI error-reporting helpers: the one place failure text and
 * process exit codes are produced. Commands never hardcode `return 1` /
 * `return 2` with ad-hoc printing; every failure flows through here so
 * the exit-code vocabulary stays exactly core::to_exit_code: 0 success,
 * 2 usage error (kInvalidArgument/kOutOfRange), 1 runtime/check failure,
 * and -1 the internal REPL exit signal (produced by Shell for
 * exit/quit, never by these helpers). Consumers: every command
 * implementation under src/cli/commands/, the Shell/REPL loop, and the
 * `ic` two-level dispatch. Dependency direction: cli -> core, never the
 * reverse.
 */

// Single CLI error-reporting helper (unified convention).
//
// All commands report failures through these — never by hardcoding `return 2`
// / `return 1` with ad-hoc printing. Mapping:
//   usage_error     -> Status{kInvalidArgument} -> exit 2 (bad args)
//   runtime_error   -> Status{kInternal}        -> exit 1 (check/IO failure)
//   from_status     -> generic Status -> exit via core::to_exit_code
// Commands still return `int` only because the process boundary needs it;
// the `int` is always produced here, never invented at call sites.

#include "wengine/core/status.hpp"

#include <ostream>
#include <string_view>

namespace wengine::cli {

/**
 * @brief Report a usage error (bad arguments) and yield its exit code.
 * @param[out] out Stream receiving the `Error: ...` line.
 * @param[in] msg One-line failure reason (a string literal; no
 *                formatting happens here).
 * @return 2 (kExitUsage): the usage-error exit code.
 * @note Maps Status{kInvalidArgument} through core::to_exit_code.
 */
inline int usage_error(std::ostream &out, std::string_view msg) {
  out << "Error: " << msg << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

/**
 * @brief Report a runtime/check failure and yield its exit code.
 * @param[out] out Stream receiving the `Error: ...` line.
 * @param[in] msg One-line failure reason (a string literal).
 * @return 1 (kExitRuntime): the runtime/check-failure exit code.
 * @note Maps Status{kInternal} through core::to_exit_code.
 */
inline int runtime_error(std::ostream &out, std::string_view msg) {
  out << "Error: " << msg << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInternal});
}

// Usage error plus the command's usage text (the common command pattern).
/**
 * @brief Report a usage error, then print the command's usage text.
 * @param[out] out Stream receiving the `Error: ...` line and usage.
 * @param[in] msg One-line failure reason (a string literal).
 * @param[in] usage The command's usage text (usually its kUsage).
 * @return 2 (kExitUsage): the usage-error exit code.
 */
inline int usage_error(std::ostream &out, std::string_view msg, std::string_view usage) {
  out << "Error: " << msg << '\n';
  out << usage << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

// Stray/unknown positional: same exit code, distinct message.
/**
 * @brief Report a stray/unknown positional argument as a usage error.
 * @param[out] out Stream receiving the error and usage text.
 * @param[in] tok The unexpected token verbatim (echoed in the message).
 * @param[in] usage The command's usage text (usually its kUsage).
 * @return 2 (kExitUsage): the usage-error exit code.
 * @note Stray positionals are never silently ignored; this is the loud
 *       refusal the command contract requires.
 */
inline int unexpected_error(std::ostream &out, std::string_view tok,
                            std::string_view usage) {
  out << "Error: unexpected argument '" << tok << "'\n";
  out << usage << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

/**
 * @brief Map a generic core::Status to its process exit code.
 * @param[out] out Stream receiving `Error: ...` on failure (silent on
 *                 success).
 * @param[in] st The status to map (any code).
 * @param[in] msg Failure text printed only when st is not ok.
 * @return core::to_exit_code(st): 0 (kExitOk) on success; 2 (kExitUsage)
 *         for kInvalidArgument/kOutOfRange; 1 (kExitRuntime) otherwise.
 * @note The single generic bridge for engine Status values; commands
 *       with structured messages prefer the dedicated helpers above.
 */
inline int from_status(std::ostream &out, core::Status st, std::string_view msg) {
  if (!st.ok()) {
    out << "Error: " << msg << '\n';
  }
  return core::to_exit_code(st);
}

} // namespace wengine::cli
