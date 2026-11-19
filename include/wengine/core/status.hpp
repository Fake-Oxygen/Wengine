#pragma once

/**
 * @file
 * The engine-wide error vocabulary: StatusCode/Status (void fallible
 * results) and Result<T> (value-or-error), plus the single CLI/process
 * exit-code mapping. Engine and solver code never throws and never
 * formats heap strings — failure crosses boundaries as a Status code
 * with, at most, a zero-copy string-literal view. Every module consumes
 * this header (solvers, world, emit, link); it must never grow I/O,
 * allocation, or frontend-specific knowledge, and exit codes belong
 * only at the CLI/process boundary via to_exit_code().
 */

#include <string_view>

namespace wengine::core {

/**
 * @brief Kind of a Status: the whole codebase's failure taxonomy.
 *
 * @note Hot-path safe: plain enum values, no payload, no allocation.
 */
// Lightweight status for hot paths (no exceptions, no heap strings —
// messages are zero-copy string-literal views).
enum class StatusCode : int {
  /**
   * @brief Success; the default-constructed state of every operation.
   */
  kOk = 0,
  /**
   * @brief Caller misuse: a bad, missing, or rejected argument (the
   *        default for parsing and validation failures; CLI usage
   *        error, exit code 2).
   */
  kInvalidArgument = 1,
  /**
   * @brief A value is outside the supported range (clock or index
   *        exhaustion, pool bounds); CLI usage error, exit code 2.
   */
  kOutOfRange = 2,
  /**
   * @brief The requested capability exists in the interface but is not
   *        built in this configuration.
   */
  kNotImplemented = 3,
  /**
   * @brief Environment/infrastructure failure on the engine's side
   *        (I/O, stream errors); CLI runtime failure, exit code 1.
   */
  kInternal = 4,
};

/**
 * @brief Void fallible result: the engine's only error channel.
 *
 * @note Unified error convention for the whole codebase:
 *       - Status      — void fallible result (engine, I/O, persistence).
 *       - Result<T>   — value-or-error (parsing, lookups); same
 *                       StatusCode vocabulary plus a zero-copy
 *                       string_view message (string literal, no
 *                       allocation, hot-path safe).
 *       - std::optional — ONLY for absence ("not provided", EOF),
 *                       never failure.
 *       - bool        — ONLY for predicates / state queries (flags,
 *                       ok() polls), never for I/O failure.
 *       - int rc      — ONLY at the CLI/process boundary, produced by
 *                       to_exit_code(Status): 0 ok, 2 usage
 *                       (kInvalidArgument/kOutOfRange), 1 runtime/check
 *                       failure, -1 is the REPL exit signal (not a
 *                       Status).
 *       - Solver inf/0 sentinels + feasible flags are domain values,
 *       not errors: solvers stay pure constexpr/noexcept, callers
 *       validate via Status/Result at the CLI layer.
 *       - Exceptions are never thrown by engine/CLI code; Shell keeps
 *       one defensive catch mapping to kInternal as a last resort.
 */
// Unified error convention for the whole codebase.
//
//   * `Status`       — void fallible result (engine, I/O, persistence).
//   * `Result<T>`    — value-or-error (parsing, lookups). Same `StatusCode`
//                      vocabulary plus a zero-copy `string_view` message
//                      (string literal, no allocation, hot-path safe).
//   * `std::optional`— ONLY for absence ("not provided", EOF), never failure.
//   * `bool`         — ONLY for predicates / state queries (flags, `feasible`,
//                      `ok()` polls), never for I/O failure.
//   * `int` rc       — ONLY at the CLI/process boundary, produced by
//                      `to_exit_code(Status)`: 0 ok, 2 usage
//                      (`kInvalidArgument`/`kOutOfRange`), 1 runtime/check
//                      failure, -1 is the REPL exit signal (not a Status).
//   * Solver `inf`/`0` sentinels + `feasible` flags are domain values, not
//     errors: solvers stay pure constexpr/noexcept, callers validate via
//     Status/Result at the CLI layer.
//   * Exceptions are never thrown by engine/CLI code; `Shell` keeps one
//     defensive `catch` mapping to `kInternal` as a last resort.
struct Status {
  /**
   * @brief The status code; kOk by default (a default Status is a
   *        success).
   */
  StatusCode code{StatusCode::kOk};

  /**
   * @brief Success predicate.
   *
   * @return true when code is kOk.
   */
  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == StatusCode::kOk;
  }
  /**
   * @brief Factory for the success value.
   *
   * @return Status carrying kOk.
   */
  [[nodiscard]] static constexpr Status success() noexcept { return {}; }
  /**
   * @brief Factory for a failure value.
   *
   * @param[in] c failure code to carry.
   * @return Status carrying c.
   */
  [[nodiscard]] static constexpr Status failure(StatusCode c) noexcept {
    return {c};
  }
};

/**
 * @brief Value-or-error: the single value-carrying error type.
 *
 * @note `error` is always a string literal (zero-copy view, no
 *       allocation) — safe in constexpr and noexcept contexts. Default
 *       code on failure is kInvalidArgument (parsing and CLI
 *       validation); pass another code for I/O (kInternal) etc.
 */
// Value-or-error: the single value-carrying error type. `error` is always
// a string literal (zero-copy view, no allocation) — safe in constexpr and
// noexcept contexts. Default code on failure is `kInvalidArgument` (parsing
// and CLI validation); pass another code for I/O (`kInternal`) etc.
template <typename T>
struct Result {
  /**
   * @brief Carried status; kOk by default.
   */
  Status status{StatusCode::kOk};
  /**
   * @brief Carried value; default-constructed T{} on failure.
   */
  T value{};
  /**
   * @brief Zero-copy failure message (string literal view; empty on
   *        success).
   */
  std::string_view error{};

  /**
   * @brief Success predicate.
   *
   * @return true when status is kOk and value is valid.
   */
  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }

  /**
   * @brief Factory for a successful result.
   *
   * @param[in] v value to carry.
   * @return Result with kOk status carrying v.
   */
  [[nodiscard]] static constexpr Result success(T v) noexcept { return {{}, v, {}}; }
  /**
   * @brief Factory for a failed result.
   *
   * @param[in] msg failure message as a string literal (zero-copy view; the caller must keep the literal alive).
   * @param[in] code failure code; defaults to kInvalidArgument (parsing/CLI validation) — pass kInternal for I/O etc.
   * @return Result carrying code and msg with a default-constructed value.
   */
  [[nodiscard]] static constexpr Result failure(std::string_view msg, StatusCode code = StatusCode::kInvalidArgument) noexcept {
    return {{code}, T{}, msg};
  }
};

/**
 * @brief CLI/process exit code for a successful run.
 */
// CLI/process exit-code mapping (single place — commands must not hardcode
// 0/1/2). Mirrors AGENTS.md: 0 ok, 1 runtime/check failure, 2 usage error.
inline constexpr int kExitOk = 0;
/**
 * @brief CLI/process exit code for a runtime/check failure.
 */
inline constexpr int kExitRuntime = 1;
/**
 * @brief CLI/process exit code for a usage error.
 */
inline constexpr int kExitUsage = 2;

/**
 * @brief Map a Status to the CLI/process exit code.
 *
 * @param[in] st status produced anywhere in the engine.
 * @return kExitOk when st is kOk; kExitUsage for kInvalidArgument or
 *         kOutOfRange (caller misuse); kExitRuntime for anything else.
 * @note The -1 REPL exit signal is not a Status and never flows through
 *       here; commands must not hardcode 0/1/2 anywhere else.
 */
[[nodiscard]] constexpr int to_exit_code(Status st) noexcept {
  if (st.ok()) {
    return kExitOk;
  }
  if (st.code == StatusCode::kInvalidArgument || st.code == StatusCode::kOutOfRange) {
    return kExitUsage;
  }
  return kExitRuntime;
}

} // namespace wengine::core
