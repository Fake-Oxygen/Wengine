#include "wengine/cli/args.hpp"
#include "wengine/cli/commands.hpp"
#include "wengine/solvers/ldo.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace wengine::solvers;
using wengine::cli::ArgCursor;
using wengine::cli::kMaxArgs;
using wengine::cli::tokenize_into;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_reference_anchors() {
  // Hand-derived operating point: VIN=5V, VOUT=3.3V, IOUT=0.5A, Iq=50uA.
  //   headroom = 1.7V, VIN(min) = 3.6V (Vdo 0.3V), IIN = 0.50005A,
  //   Ppass = 0.85W, Pq = 250uW, Ptot = 0.85025W,
  //   POUT = 1.65W, PIN = 2.50025W, TJ = 110.025C (TA 25C, Rth 100C/W).
  const auto r = LdoSolver::solve(5.0, 3.3, 0.5, 50e-6, 0.3, 25.0, 100.0);
  assert(near(r.headroom_v, 1.7));
  assert(near(r.vin_min_v, 3.6));
  assert(near(r.iin_a, 0.50005));
  assert(near(r.p_pass_w, 0.85));
  assert(near(r.p_q_w, 250e-6));
  assert(near(r.p_total_w, 0.85025));
  assert(near(r.pout_w, 1.65));
  assert(near(r.pin_w, 2.50025));
  assert(near(r.eff, 1.65 / 2.50025));
  assert(near(r.eff * r.pin_w, r.pout_w, 1e-12)); // eff is the PIN->POUT ratio
  assert(near(r.tj_c, 110.025));
  assert(r.dropout_ok && r.thermal_ok && r.pass); // no tjmax -> no thermal limit
}

void test_helpers() {
  // Primitives on exact integer points.
  assert(LdoSolver::headroom_v(5.0, 3.0) == 2.0);
  assert(LdoSolver::vin_min_v(3.0, 1.0) == 4.0);
  assert(LdoSolver::input_current_a(2.0, 1.0) == 3.0);
  assert(LdoSolver::pass_power_w(5.0, 3.0, 2.0) == 4.0);
  assert(LdoSolver::quiescent_power_w(5.0, 2.0) == 10.0);
  assert(LdoSolver::total_power_w(5.0, 3.0, 2.0, 1.0) == 9.0);
  assert(LdoSolver::output_power_w(3.0, 2.0) == 6.0);
  assert(LdoSolver::input_power_w(5.0, 2.0, 1.0) == 15.0);
  assert(LdoSolver::junction_temp_c(25.0, 2.0, 10.0) == 45.0);
  // No-load edge: PIN == POUT == 0 defines eff as 1.0; N/A is a display concern.
  assert(LdoSolver::efficiency(3.3, 5.0, 0.0, 0.0) == 1.0);
  // Input power zero with nonzero output is unphysical: +inf, never NaN.
  assert(LdoSolver::efficiency(3.3, 0.0, 0.5, 0.0) ==
         std::numeric_limits<double>::infinity());
  // Dropout boundary is inclusive: headroom == Vdo still regulates.
  {
    const auto r = LdoSolver::solve(3.6, 3.3, 0.1, 0.0, 0.3);
    assert(near(r.headroom_v, 0.3));
    assert(r.dropout_ok && r.pass);
  }
  // Below dropout: FAIL, and the shortfall is visible in the numbers.
  {
    const auto r = LdoSolver::solve(3.4, 3.3, 0.1, 0.0, 0.3);
    assert(!r.dropout_ok && !r.pass);
    assert(near(r.headroom_v, 0.1));
    assert(near(r.vin_min_v, 3.6));
  }
  // Thermal limit honored only when tjmax is supplied.
  {
    const auto ok = LdoSolver::solve(5.0, 3.3, 0.5, 0.0, 0.0, 25.0, 100.0, 125.0);
    assert(near(ok.tj_c, 110.0));
    assert(ok.thermal_ok && ok.pass);
    const auto hot = LdoSolver::solve(5.0, 3.3, 0.5, 0.0, 0.0, 25.0, 100.0, 105.0);
    assert(!hot.thermal_ok && !hot.pass);
  }
}

void test_typed_api() {
  using namespace wengine::units::literals;
  // Microamp literal + parser spot-checks (Iq lives in uA on real LDOs).
  // Note: 50*1e-6 and the 0.00005 literal can differ by 1 ulp, so near().
  assert(near((50.0_uA).get(), (0.00005_A).get(), 1e-15));
  assert(near((50.0_uA).get(), 50e-6, 1e-15));
  assert(near(wengine::cli::parse_current("50uA").value, 50e-6));
  assert(near(wengine::cli::parse_current("2mA").value, 0.002));
  auto h = LdoSolver::headroom(5.0_V, 3.3_V);
  assert(near(h.get(), 1.7));
  auto m = LdoSolver::vin_min(3.0_V, 1.0_V);
  assert(m.get() == 4.0);
  auto iin = LdoSolver::input_current(0.5_A, 50.0_uA);
  assert(near(iin.get(), 0.50005));
  auto pp = LdoSolver::pass_power(5.0_V, 3.0_V, 2.0_A);
  assert(pp.get() == 4.0);
  auto po = LdoSolver::output_power(3.0_V, 2.0_A);
  assert(po.get() == 6.0);
}

std::pair<int, std::string> run_ldo(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  std::span<const std::string_view> args{buf.data(), n};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = wengine::cli::commands::LdoSpec::run(cur, out);
  return {rc, out.str()};
}

void test_cli() {
  {
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V --iout 500mA");
    assert(rc == 0);
    assert(s.find("headroom = 1.7") != std::string::npos);
    assert(s.find("P total = 0.85") != std::string::npos);
    assert(s.find("eff = ") != std::string::npos);
    assert(s.find("summary: PASS") != std::string::npos);
  }
  {
    // Full reference point through the CLI, incl. microamp Iq.
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V --iout 500mA --iq 50uA");
    assert(rc == 0 && s.find("P total = 0.85025") != std::string::npos);
    assert(s.find("IIN = 0.50005") != std::string::npos);
  }
  {
    // Dropout violation reports FAIL with rc 1.
    auto [rc, s] = run_ldo("--vin 3.4V --vout 3.3V --iout 100mA --vdo 0.3V");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
    assert(s.find("min VIN 3.6") != std::string::npos);
  }
  {
    // Thermal violation reports FAIL with rc 1 (no Iq: TJ = 25 + 85 = 110C).
    auto [rc, s] =
        run_ldo("--vin 5V --vout 3.3V --iout 500mA --ta 25 --rth 100 --tjmax 105");
    assert(rc == 1 && s.find("TJ = 110") != std::string::npos);
    assert(s.find("summary: FAIL") != std::string::npos);
  }
  {
    // Same point under a looser limit passes.
    auto [rc, s] =
        run_ldo("--vin 5V --vout 3.3V --iout 500mA --ta 25 --rth 100 --tjmax 125");
    assert(rc == 0 && s.find("summary: PASS") != std::string::npos);
  }
  {
    // No-load point: zero dissipation, unity edge efficiency, still PASS.
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V --iout 0A");
    assert(rc == 0 && s.find("P total = 0") != std::string::npos);
    assert(s.find("eff = N/A") != std::string::npos);
  }
  {
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V");
    assert(rc == 2); // missing --iout
  }
  {
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V --iout 500mA 3.3V");
    assert(rc == 2); // stray positional (named-options only)
  }
  {
    auto [rc, s] = run_ldo("--vin 0V --vout 3.3V --iout 500mA");
    assert(rc == 2); // VIN must be > 0
  }
  {
    auto [rc, s] = run_ldo("--vin 5V --vout 3.3V --iout 500mA --bogus 1");
    assert(rc == 2); // unknown option never silently ignored
  }
  {
    auto [rc, s] = run_ldo("--help");
    assert(rc == 0 && s.find("headroom") != std::string::npos);
  }
}

void test_completion() {
  using wengine::cli::commands::LdoSpec;
  auto complete = [](std::string_view partial) {
    std::array<std::string_view, 0> prior{};
    std::vector<std::string> out;
    LdoSpec::complete(prior, partial, out);
    return out;
  };
  auto has = [](const std::vector<std::string> &v, std::string_view s) {
    for (const auto &e : v) {
      if (e == s) {
        return true;
      }
    }
    return false;
  };
  {
    const auto c = complete("--");
    assert(has(c, "--vin") && has(c, "--vout") && has(c, "--iout") && has(c, "--iq") &&
           has(c, "--vdo") && has(c, "--ta") && has(c, "--rth") && has(c, "--tjmax") &&
           has(c, "--help"));
  }
  {
    const auto c = complete("--v");
    assert(c.size() == 3 && has(c, "--vin") && has(c, "--vout") && has(c, "--vdo"));
  }
  {
    // Numeric values: nothing to complete.
    assert(complete("5V").empty());
    assert(complete("").empty());
  }
}

} // namespace

int main() {
  test_reference_anchors();
  test_helpers();
  test_typed_api();
  test_cli();
  test_completion();
  std::cout << "test_ldo: OK\n";
  return 0;
}
