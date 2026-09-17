#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/solvers/s0_pulse.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <span>
#include <sstream>
#include <string>

using namespace wengine::solvers;
using wengine::cli::ArgCursor;
using wengine::cli::kMaxArgs;
using wengine::cli::tokenize_into;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_conversions() {
  // 1000 imp/kWh <-> 1 imp/Wh <-> 3600 J/imp <-> 1 Wh/imp.
  assert(near(S0PulseSolver::imp_per_kwh_to_imp_per_wh(1000.0), 1.0));
  assert(near(S0PulseSolver::imp_per_kwh_to_imp_per_j(1000.0), 1000.0 / 3600000.0));
  assert(near(S0PulseSolver::imp_per_kwh_to_imp_per_ws(1000.0),
              S0PulseSolver::imp_per_kwh_to_imp_per_j(1000.0)));
  assert(near(S0PulseSolver::imp_per_kwh_to_j_per_imp(1000.0), 3600.0));
  assert(near(S0PulseSolver::imp_per_kwh_to_wh_per_imp(1000.0), 1.0));
  assert(near(S0PulseSolver::imp_per_kwh_to_kwh_per_imp(1000.0), 0.001));

  // Energy bridges.
  assert(near(S0PulseSolver::kwh_to_joules(1.0), 3600000.0));
  assert(near(S0PulseSolver::joules_to_kwh(3600000.0), 1.0));
  assert(near(S0PulseSolver::wh_to_joules(1.0), 3600.0));
  // Wh/s -> W: 1 Wh/s == 3600 W.
  assert(near(S0PulseSolver::wh_per_s_to_w(1.0), 3600.0));
  assert(near(S0PulseSolver::w_to_wh_per_s(3600.0), 1.0));
}

void test_forward_inverse() {
  // 2 kW @ 1000 imp/kWh -> f = 2000*1000/3.6e6 Hz, T = 1.8 s.
  assert(near(S0PulseSolver::frequency_hz(2000.0, 1000.0), 2000.0 * 1000.0 / 3600000.0));
  assert(near(S0PulseSolver::period_s(2000.0, 1000.0), 1.8));
  // Inverse: f -> P round-trip.
  assert(near(S0PulseSolver::power_w_from_frequency(0.5555555555555556, 1000.0), 2000.0,
              1e-6));
  // Pulses in 1 h at 2 kW: E = 2 kWh -> 2000 imp.
  assert(near(S0PulseSolver::pulses_from_power_time(2000.0, 3600.0, 1000.0), 2000.0));
  assert(near(S0PulseSolver::pulses_from_energy_kwh(2.0, 1000.0), 2000.0));
  assert(near(S0PulseSolver::pulses_from_energy_j(7200000.0, 1000.0), 2000.0));
  // Inverse: pulses -> energy.
  assert(near(S0PulseSolver::energy_kwh_from_pulses(500.0, 1000.0), 0.5));
  assert(near(S0PulseSolver::energy_j_from_pulses(1000.0, 1000.0), 3600000.0));
  // Time for 1000 imp at 2 kW: half an hour.
  assert(near(S0PulseSolver::time_s_for_pulses(1000.0, 2000.0, 1000.0), 1800.0));
  // U*I with power factor.
  assert(near(S0PulseSolver::power_w_from_ui(230.0, 10.0), 2300.0));
  assert(near(S0PulseSolver::power_w_from_ui(230.0, 10.0, 0.9), 2070.0));

  // Compile-time proof (key for perf: solver is constexpr).
  static_assert(S0PulseSolver::frequency_hz(2000.0, 1000.0) ==
                2000.0 * 1000.0 / 3600000.0);
  static_assert(S0PulseSolver::imp_per_kwh_to_wh_per_imp(1000.0) == 1.0);
  static_assert(S0PulseSolver::pulses_from_energy_kwh(2.0, 1000.0) == 2000.0);
}

void test_typed_api() {
  using namespace wengine::units::literals;
  // Frequency from power.
  auto f = S0PulseSolver::frequency(2000.0_W, 1000.0);
  assert(near(f.get(), 2000.0 * 1000.0 / 3600000.0));
  // Power back from frequency.
  auto p = S0PulseSolver::power(f, 1000.0);
  assert(near(p.get(), 2000.0, 1e-6));
  // Energy in 2000 pulses @1000 imp/kWh = 2 kWh = 7.2 MJ.
  auto e = S0PulseSolver::energy(2000.0_imp, 1000.0);
  assert(near(e.get(), 7200000.0));
  auto n = S0PulseSolver::pulses(7.2e6_J, 1000.0);
  assert(near(n.get(), 2000.0));
  auto n2 = S0PulseSolver::pulses(2000.0_W, 3600.0_s, 1000.0);
  assert(near(n2.get(), 2000.0));
}

void test_full_result() {
  const auto r = S0PulseSolver::solve_full(2000.0, 3600.0, 1000.0);
  assert(near(r.power_w, 2000.0));
  assert(near(r.energy_kwh, 2.0));
  assert(near(r.energy_j, 7200000.0));
  assert(near(r.pulses, 2000.0));
  assert(near(r.j_per_imp, 3600.0));
  const auto r2 = S0PulseSolver::solve_full_from_ui(230.0, 10.0, 1.0, 3600.0, 1000.0);
  assert(near(r2.power_w, 2300.0));
  assert(near(r2.energy_kwh, 2.3));
}

std::pair<int, std::string> run_s0(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::S0Spec::run(cur, out);
  return {rc, out.str()};
}

void test_cli() {
  {
    auto [rc, s] = run_s0("convert --const 1000imp/kWh");
    assert(rc == 0);
    assert(s.find("imp/Wh") != std::string::npos);
    assert(s.find("imp/J") != std::string::npos);
  }
  {
    // imp/Wh and imp/Ws input forms convert to the same base.
    auto [rc, s] = run_s0("convert --const 1imp/Wh");
    assert(rc == 0 && s.find("1000 imp/kWh") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("rate --power 2kW --const 1000imp/kWh");
    assert(rc == 0 && s.find("Hz") != std::string::npos);
  }
  {
    // Inverse: frequency -> power.
    auto [rc, s] = run_s0("power --freq 0.5555555556Hz --const 1000imp/kWh");
    assert(rc == 0 && s.find("2000") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("energy --pulses 500 --const 1000imp/kWh");
    assert(rc == 0 && s.find("0.5 kWh") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("pulses --power 2kW --time 1h --const 1000imp/kWh");
    assert(rc == 0 && s.find("2000 imp") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("pulses --energy 2kWh --const 1000imp/kWh");
    assert(rc == 0 && s.find("2000 imp") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("full --u 230V --i 10A --const 1000imp/kWh --time 1h");
    assert(rc == 0 && s.find("2300") != std::string::npos);
  }
  {
    // Mixed named + positional: missing slots fill in order.
    auto [rc, s] = run_s0("full --u 230V --const 1000imp/kWh 10A");
    assert(rc == 0 && s.find("2300") != std::string::npos);
  }
  {
    // Positional forms.
    auto [rc, s] = run_s0("rate 2kW --const 1000imp/kWh");
    assert(rc == 0 && s.find("Hz") != std::string::npos);
  }
  {
    auto [rc, s] = run_s0("pulses 2kW 1h --const 1000imp/kWh");
    assert(rc == 0 && s.find("2000 imp") != std::string::npos);
  }
  {
    // Stray positionals are errors, never silently ignored.
    auto [rc, s] = run_s0("convert --const 1000imp/kWh bogus");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_s0("rate --power 2kW --const 1000imp/kWh extra");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_s0("rate --power 2kW");
    assert(rc == 2); // missing --const
  }
  {
    auto [rc, s] = run_s0("--help");
    assert(rc == 0 && s.find("Usage") != std::string::npos);
  }
}

} // namespace

int main() {
  test_conversions();
  test_forward_inverse();
  test_typed_api();
  test_full_result();
  test_cli();
  std::cout << "test_s0: OK\n";
  return 0;
}
