#include "wengine/cli/commands.hpp"
#include "wengine/cli/status.hpp"
#include "wengine/solvers/ohm_law.hpp"

#include <array>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::commands {

namespace {

enum class Kind : int { kBare, kVoltage, kCurrent, kResistance };

[[nodiscard]] constexpr bool ends_with(std::string_view s, std::string_view suf) noexcept {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// Suffix-based classification. Disjoint sets: *V -> U, *A -> I,
// *Ohm/*ohm/*R -> R, else bare number.
[[nodiscard]] constexpr Kind classify(std::string_view s) noexcept {
  if (s.empty()) {
    return Kind::kBare;
  }
  if (ends_with(s, "V")) {
    return Kind::kVoltage;
  }
  if (ends_with(s, "A")) {
    return Kind::kCurrent;
  }
  if (ends_with(s, "Ohm") || ends_with(s, "ohm") || ends_with(s, "R")) {
    return Kind::kResistance;
  }
  return Kind::kBare;
}

} // namespace

int OhmSpec::run(ArgCursor &args, std::ostream &out) {
  if (args.take_flag({"--help", "-h"})) {
    out << kName << " - " << kDescription << "\nUsage:\n" << kUsage << '\n';
    return core::kExitOk;
  }

  std::optional<double> u_si;
  std::optional<double> i_si;
  std::optional<double> r_si;

  // -- named options (all forms: --u V | --u=V | -U V) ------------------
  if (auto o = args.take_option({"--voltage", "--volt", "--u", "-U"})) {
    auto p = parse_voltage(*o);
    if (!p.ok()) {
      out << "Error: invalid voltage '" << *o << "' (" << p.error << ")\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    u_si = p.value;
  }
  if (auto o = args.take_option({"--current", "--i", "-I"})) {
    auto p = parse_current(*o);
    if (!p.ok()) {
      out << "Error: invalid current '" << *o << "' (" << p.error << ")\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    i_si = p.value;
  }
  if (auto o = args.take_option({"--resistance", "--r", "-R"})) {
    auto p = parse_resistance(*o);
    if (!p.ok()) {
      out << "Error: invalid resistance '" << *o << "' (" << p.error << ")\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    r_si = p.value;
  }

  std::optional<std::string_view> from = args.take_option({"--from"});
  std::string_view mode{};
  if (from) {
    mode = *from;
    if (mode != "ui" && mode != "iu" && mode != "ur" && mode != "ru" &&
        mode != "ir" && mode != "ri") {
      out << "Error: --from must be one of ui|ur|ir (got '" << mode << "')\n";
      return core::kExitUsage;
    }
  }

  // -- positional tokens (auto-classified by suffix) ---------------------
  std::array<std::string_view, 3> pos{};
  std::size_t n_pos = 0;
  while (auto p = args.next_positional()) {
    if (n_pos >= pos.size()) {
      out << "Error: too many positional arguments (max 3: U I R)\n";
      out << kUsage << '\n';
      return core::kExitUsage;
    }
    pos[n_pos++] = *p;
  }

  // Any leftover tokens are unknown options (e.g. --typo).
  if (args.has_remaining()) {
    return cli::unexpected_error(out, *args.first_remaining(), kUsage);
  }

  auto assign_bare = [&](std::string_view tok) -> bool {
    auto p = parse_double(tok);
    if (!p.ok()) {
      out << "Error: invalid number '" << tok << "'\n";
      return false;
    }
    // Order depends on --from, else fill U, then I, then R.
    if (!mode.empty()) {
      char first = mode[0];
      char second = mode[1];
      auto slot = [&](char m) -> std::optional<double> * {
        if (m == 'u') {
          return &u_si;
        }
        if (m == 'i') {
          return &i_si;
        }
        return &r_si;
      };
      if (!*slot(first)) {
        **slot(first) = p.value;
        return true;
      }
      if (!*slot(second)) {
        **slot(second) = p.value;
        return true;
      }
      out << "Error: duplicate value '" << tok << "'\n";
      return false;
    }
    if (!u_si) {
      u_si = p.value;
      return true;
    }
    if (!i_si) {
      i_si = p.value;
      return true;
    }
    if (!r_si) {
      r_si = p.value;
      return true;
    }
    out << "Error: duplicate value '" << tok << "'\n";
    return false;
  };

  for (std::size_t k = 0; k < n_pos; ++k) {
    const std::string_view tok = pos[k];
    switch (classify(tok)) {
    case Kind::kVoltage: {
      if (u_si) {
        out << "Error: duplicate voltage '" << tok << "'\n";
        return core::kExitUsage;
      }
      auto p = parse_voltage(tok);
      if (!p.ok()) {
        out << "Error: invalid voltage '" << tok << "'\n";
        return core::kExitUsage;
      }
      u_si = p.value;
      break;
    }
    case Kind::kCurrent: {
      if (i_si) {
        out << "Error: duplicate current '" << tok << "'\n";
        return core::kExitUsage;
      }
      auto p = parse_current(tok);
      if (!p.ok()) {
        out << "Error: invalid current '" << tok << "'\n";
        return core::kExitUsage;
      }
      i_si = p.value;
      break;
    }
    case Kind::kResistance: {
      if (r_si) {
        out << "Error: duplicate resistance '" << tok << "'\n";
        return core::kExitUsage;
      }
      auto p = parse_resistance(tok);
      if (!p.ok()) {
        out << "Error: invalid resistance '" << tok << "'\n";
        return core::kExitUsage;
      }
      r_si = p.value;
      break;
    }
    case Kind::kBare:
      if (!assign_bare(tok)) {
        return core::kExitUsage;
      }
      break;
    }
  }

  const int given = (u_si ? 1 : 0) + (i_si ? 1 : 0) + (r_si ? 1 : 0);
  if (given != 2) {
    out << "Error: provide exactly 2 of U/I/R (got " << given << ")\n";
    out << kUsage << '\n';
    return core::kExitUsage;
  }

  using V = units::Value<units::ElectricPotential>;
  using I = units::Value<units::ElectricCurrent>;
  using R = units::Value<units::ElectricResistance>;

  solvers::ResistorState st{};
  if (u_si && i_si) {
    st = solvers::OhmLawSolver::solve_from_ui(V{*u_si}, I{*i_si});
  } else if (u_si && r_si) {
    if (*r_si == 0.0) {
      out << "Error: resistance must be non-zero\n";
      return core::kExitUsage;
    }
    st = solvers::OhmLawSolver::solve_from_ur(V{*u_si}, R{*r_si});
  } else {
    st = solvers::OhmLawSolver::solve_from_ir(I{*i_si}, R{*r_si});
  }

  out << "U = " << st.voltage.get() << " V\n";
  out << "I = " << st.current.get() << " A\n";
  out << "R = " << st.resistance.get() << " Ohm\n";
  out << "P = " << st.power.get() << " W\n";
  return core::kExitOk;
}

namespace {

constexpr std::array<std::string_view, 13> kOhmOptions{{
    "--voltage", "--volt", "--u", "-U", "--current", "--i", "-I", "--resistance", "--r",
    "-R", "--from", "--help", "-h",
}};

constexpr std::array<std::string_view, 6> kFromValues{{
    "ui", "ur", "ir", "iu", "ru", "ri",
}};

void push_matches(std::string_view partial,
                  std::span<const std::string_view> options, std::vector<std::string> &out) {
  for (std::string_view o : options) {
    if (o.substr(0, partial.size()) == partial) {
      out.emplace_back(o);
    }
  }
}

} // namespace

void OhmSpec::complete(std::span<const std::string_view> prior_args, std::string_view partial,
                       std::vector<std::string> &out) {
  // Values for a preceding "--from".
  if (!prior_args.empty() && prior_args.back() == "--from") {
    push_matches(partial, kFromValues, out);
    return;
  }
  // "--from=<prefix>" form.
  constexpr std::string_view kFromEq = "--from=";
  if (partial.substr(0, kFromEq.size()) == kFromEq) {
    const std::string_view sub = partial.substr(kFromEq.size());
    for (std::string_view v : kFromValues) {
      if (v.substr(0, sub.size()) == sub) {
        out.emplace_back(std::string{kFromEq} + std::string{v});
      }
    }
    return;
  }
  if (!partial.empty() && partial.front() == '-') {
    push_matches(partial, kOhmOptions, out);
  }
  // Positional bare/suffixed numbers: nothing sensible to complete.
}

} // namespace wengine::cli::commands
