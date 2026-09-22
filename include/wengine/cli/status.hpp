#pragma once

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

inline int usage_error(std::ostream &out, std::string_view msg) {
  out << "Error: " << msg << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

inline int runtime_error(std::ostream &out, std::string_view msg) {
  out << "Error: " << msg << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInternal});
}

// Usage error plus the command's usage text (the common command pattern).
inline int usage_error(std::ostream &out, std::string_view msg, std::string_view usage) {
  out << "Error: " << msg << '\n';
  out << usage << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

// Stray/unknown positional: same exit code, distinct message.
inline int unexpected_error(std::ostream &out, std::string_view tok,
                            std::string_view usage) {
  out << "Error: unexpected argument '" << tok << "'\n";
  out << usage << '\n';
  return core::to_exit_code(core::Status{core::StatusCode::kInvalidArgument});
}

inline int from_status(std::ostream &out, core::Status st, std::string_view msg) {
  if (!st.ok()) {
    out << "Error: " << msg << '\n';
  }
  return core::to_exit_code(st);
}

} // namespace wengine::cli
