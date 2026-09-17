#include "wengine/cli/commands.hpp"
#include "wengine/solvers/ohm_law.hpp"

#include <ostream>
#include <span>

namespace wengine::cli::commands {

int ohm(std::span<const std::string> /*args*/, std::ostream &out) noexcept {
  using namespace wengine::units::literals;
  const auto result = solvers::OhmLawSolver::solve_from_ui(12.4_V, 0.54_A);

  out << "U = " << result.voltage.get() << '\n';
  out << "I = " << result.current.get() << '\n';
  out << "P = " << result.power.get() << '\n';
  out << "R = " << result.resistance.get() << '\n';
  return 0;
}

} // namespace wengine::cli::commands
