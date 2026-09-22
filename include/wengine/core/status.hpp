#pragma once

#include <string_view>

namespace wengine::core {

// Lightweight status for hot paths (no exceptions, no heap strings —
// messages are zero-copy string-literal views).
enum class StatusCode : int {
  kOk = 0,
  kInvalidArgument = 1,
  kOutOfRange = 2,
  kNotImplemented = 3,
  kInternal = 4,
};

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
  StatusCode code{StatusCode::kOk};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return code == StatusCode::kOk;
  }
  [[nodiscard]] static constexpr Status success() noexcept { return {}; }
  [[nodiscard]] static constexpr Status failure(StatusCode c) noexcept {
    return {c};
  }
};

// Value-or-error: the single value-carrying error type. `error` is always
// a string literal (zero-copy view, no allocation) — safe in constexpr and
// noexcept contexts. Default code on failure is `kInvalidArgument` (parsing
// and CLI validation); pass another code for I/O (`kInternal`) etc.
template <typename T>
struct Result {
  Status status{StatusCode::kOk};
  T value{};
  std::string_view error{};

  [[nodiscard]] constexpr bool ok() const noexcept { return status.ok(); }

  [[nodiscard]] static constexpr Result success(T v) noexcept { return {{}, v, {}}; }
  [[nodiscard]] static constexpr Result
  failure(std::string_view msg,
          StatusCode code = StatusCode::kInvalidArgument) noexcept {
    return {{code}, T{}, msg};
  }
};

// CLI/process exit-code mapping (single place — commands must not hardcode
// 0/1/2). Mirrors AGENTS.md: 0 ok, 1 runtime/check failure, 2 usage error.
inline constexpr int kExitOk = 0;
inline constexpr int kExitRuntime = 1;
inline constexpr int kExitUsage = 2;

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
