#pragma once

/**
 * @file
 * Starter template for IC-specific closed-form solvers. Documents the
 * pattern every `include/wengine/solvers/ic/<chip>.hpp` must follow
 * (AGENTS.md section 9, "New IC-specific solver"): the same rules as a
 * generic closed-form solver — pure, header-only, constexpr + noexcept
 * where actually appropriate, SI quantities across subsystem
 * boundaries, [[nodiscard]] results, static_asserts where exactly
 * representable, and tests against an independently derived closed
 * form or trusted reference (never goldens copied from the
 * implementation) — plus the mandatory datasheet reference block
 * (title, revision, section/equation numbers) quoted verbatim at the
 * top of every copy. Never include CLI types here (cli -> solvers,
 * never the reverse): the CLI glue lives in
 * include/wengine/cli/ic/&lt;chip&gt;.hpp plus
 * src/cli/commands/ic/&lt;chip&gt;.cpp, registered in
 * wengine/cli/ic_registry.hpp, and dispatched through cli/ic_dispatch.hpp
 * under the single top-level `ic` command. Consumed by copying: this
 * file intentionally defines nothing.
 */

// Starter template for IC-specific closed-form solvers.
//
// This file intentionally defines nothing: copy it to `<chip>.hpp`
// (lowercase part number, e.g. `ne555.hpp`), rename the namespace/struct,
// and implement the datasheet formulas. Full rules: AGENTS.md section 9
// ("New IC-specific solver").
//
// Paste this skeleton into the copy:
//
//   #pragma once
//
//   // <Part> <function> solver.
//   //
//   // Reference: <datasheet title>, rev <rev>, section <sec> (<equation>).
//   //   <formula in SI units, with a worked example and trusted value>
//   //
//   // All functions are pure, header-only, constexpr + noexcept.
//   // Input validation (ranges, > 0, ...) is the caller's job (CLI layer).
//
//   #include "wengine/units/units.hpp"
//
//   namespace wengine::solvers::ic {
//
//   class <Part>Solver {
//   public:
//     <Part>Solver() = delete;
//
//     [[nodiscard]] static constexpr double solve_<what>(
//         units::Value<units::...> ...) noexcept {
//       ...
//     }
//   };
//
//   } // namespace wengine::solvers::ic
//
// Requirements for the copy: SI quantities across subsystem boundaries,
// `[[nodiscard]]` on results, `static_assert`s where exactly
// representable, tests against an independently derived closed form or
// trusted reference (never goldens copied from the implementation), and
// the datasheet citation above. Never include CLI types here
// (`cli -> solvers`, never the reverse); the CLI glue lives in
// `include/wengine/cli/ic/<chip>.hpp` + `src/cli/commands/ic/<chip>.cpp`
// and registers in `wengine/cli/ic_registry.hpp`.
