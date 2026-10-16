#include "wengine/cli/command_registry.hpp"
#include "wengine/cli/ic/ap63205.hpp"
#include "wengine/cli/ic_dispatch.hpp"
#include "wengine/cli/ic_registry.hpp"
#include "wengine/cli/shell.hpp"
#include "wengine/solvers/ic/ap63205.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wengine::cli;
using S = wengine::solvers::ic::Ap63205Solver;

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

void test_constants() {
  static_assert(S::kVoutV == 5.0);
  static_assert(S::kFswHz == 1100000.0);
  static_assert(S::kMaxLoadA == 2.0);
  static_assert(S::kVinMinV == 3.8);
  static_assert(S::kVinMaxV == 32.0);
  static_assert(S::default_ripple_a(0.5) == 1.0);
  assert(S::default_ripple_a(0.4) == 0.8);
}

void test_inductor() {
  // Eq.7 golden: VIN=12V, dIL=0.8A -> 5*7/(12*0.8*1.1e6) = 3.3143939uH.
  assert(near(S::inductor_h(12.0, 5.0, 0.8, 1100000.0), 3.3143939e-6, 1e-12));
  // 30% ripple -> 4.4191919uH, the Table 3 4.7uH pick.
  assert(near(S::inductor_h(12.0, 5.0, 0.6, 1100000.0), 4.4191919e-6, 1e-12));
  // Eq.8 golden: 2A + 0.8/2 = 2.4A peak.
  assert(near(S::peak_current_a(2.0, 0.8), 2.4));
  assert(near(S::on_time_s(12.0, 5.0, 1100000.0), 378.7878788e-9, 1e-12));

  const auto r = S::solve_inductor(12.0, 2.0, 0.8);
  assert(near(r.l_h, 3.3143939e-6, 1e-12));
  assert(near(r.peak_a, 2.4));
  assert(r.peak_ok && r.ton_ok); // 2.4A <= 2.5A limit, 379ns >= 80ns

  const auto hot = S::solve_inductor(12.0, 2.0, 1.2);
  assert(near(hot.peak_a, 2.6));
  assert(!hot.peak_ok); // 2.6A exceeds the 2.5A worst-case HS limit
}

void test_cout() {
  // Eq.9 golden: 0.8A * 5mOhm = 4mV.
  assert(near(S::cout_ripple_v(0.8, 0.005), 0.004));
  // Eq.10 golden: 4.7uH*(2.4)^2 / (5.25^2 - 25) = 10.5646829uF.
  assert(near(S::cout_min_f(4.7e-6, 2.0, 0.8, 5.0, 0.25), 1.0564683e-5, 1e-12));

  const auto r = S::solve_cout(4.7e-6, 2.0, 0.8, 0.005, 0.25);
  assert(near(r.ripple_v, 0.004));
  assert(near(r.cout_min_f, 1.0564683e-5, 1e-12));
}

void test_uvlo() {
  // Eq.1/2 goldens: VON=8V, VOFF=7V -> R5 = 111.2195k, R6 = 18.7879k.
  assert(near(S::uvlo_r5_ohm(8.0, 7.0), 111219.5122, 1e-6));
  assert(near(S::uvlo_r6_ohm(7.0, 111219.51219512206), 18787.9242, 1e-6));

  const auto r = S::solve_uvlo(8.0, 7.0);
  assert(near(r.r5_ohm, 111219.5122, 1e-6));
  assert(near(r.r6_ohm, 18787.9242, 1e-6));
}

void test_delay_thermal() {
  // Eq.3 goldens: 10ms -> 12.7nF and back.
  assert(near(S::en_delay_cap_nf(10.0), 12.7));
  assert(near(S::en_delay_time_ms(12.7), 10.0));
  // Eq.4/5 goldens: 0.5W at TA=85 -> TJ = 129.5C (over the +125C target).
  assert(near(S::temp_rise_c(0.5), 44.5));
  assert(near(S::junction_temp_c(0.5, 85.0), 129.5));
  const auto hot = S::solve_thermal(0.5, 85.0);
  assert(!hot.ok);
  // Conduction estimate: 12V/2A -> 0.367W -> TJ = 117.663C at TA=85.
  assert(near(S::conduction_loss_w(12.0, 5.0, 2.0), 0.367));
  const auto ok = S::solve_thermal(S::conduction_loss_w(12.0, 5.0, 2.0), 85.0);
  assert(near(ok.tj_c, 117.663));
  assert(ok.ok);
}

std::pair<int, std::string> run_chip(std::string line) {
  std::array<std::string_view, kMaxArgs> buf{};
  const std::size_t n = tokenize_into(line, buf);
  const std::size_t m = n < buf.size() ? n : buf.size();
  std::span<const std::string_view> args{buf.data(), m};
  ArgCursor cur{args};
  std::ostringstream out;
  const int rc = ic::Ap63205Spec::run(cur, out);
  return {rc, out.str()};
}

void test_cli_inductor() {
  {
    auto [rc, s] = run_chip("inductor --vin 12V --iout 2A");
    assert(rc == 0);
    assert(s.find("L = 3.31439") != std::string::npos);
    assert(s.find("IL(peak) = 2.4") != std::string::npos);
    assert(s.find("4.7 uH") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("inductor --vin 12V --delta-il 0.6A");
    assert(rc == 0 && s.find("L = 4.41919") != std::string::npos);
  }
  {
    // Over the HS peak limit: check failure, exit 1.
    auto [rc, s] = run_chip("inductor --vin 12V --delta-il 1.2A");
    assert(rc == 1 && s.find("FAIL") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("inductor --vin 12V --ripple 0.2");
    assert(rc == 0 && s.find("WARN") != std::string::npos); // outside 0.3..0.5
  }
  {
    auto [rc, s] = run_chip("inductor --vin 40V");
    assert(rc == 2); // outside 3.8..32V
  }
  {
    auto [rc, s] = run_chip("inductor --vin 12V --ripple 0.4 --delta-il 0.8A");
    assert(rc == 2); // mutually exclusive
  }
  {
    auto [rc, s] = run_chip("inductor --vin 12V extra");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("inductor");
    assert(rc == 2); // --vin required
  }
}

void test_cli_cout_uvlo_delay() {
  {
    auto [rc, s] = run_chip("cout --vin 12V --esr 5mOhm");
    assert(rc == 0);
    assert(s.find("4 mV") != std::string::npos);
    assert(s.find("COUT(min) = 7.45011") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("cout --vin 12V");
    assert(rc == 2); // --esr required
  }
  {
    auto [rc, s] = run_chip("uvlo --von 8V --voff 7V");
    assert(rc == 0);
    assert(s.find("R5 = 111.22") != std::string::npos);
    assert(s.find("R6 = 18.7879") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("uvlo --von 7V --voff 8V");
    assert(rc == 2); // need VOFF < VON
  }
  {
    auto [rc, s] = run_chip("uvlo --von 8V");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("delay --ts 10ms");
    assert(rc == 0 && s.find("Cd = 12.7 nF") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("delay --cd 12.7nF");
    assert(rc == 0 && s.find("ts = 10 ms") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("delay --ts 10ms --cd 12.7nF");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("delay");
    assert(rc == 2);
  }
}

void test_cli_thermal_spec() {
  {
    auto [rc, s] = run_chip("thermal --pd 0.5W --ta 85");
    assert(rc == 1 && s.find("TJ = 129.5") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("thermal --vin 12V --iout 2A --ta 85");
    assert(rc == 0 && s.find("conduction estimate") != std::string::npos);
    assert(s.find("TJ = 117.663") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("thermal --pd 0.5W --vin 12V");
    assert(rc == 2); // --pd and --vin are exclusive
  }
  {
    auto [rc, s] = run_chip("thermal");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("spec");
    assert(rc == 0 && s.find("AP63205WU-7") != std::string::npos);
  }
  {
    auto [rc, s] = run_chip("bogus");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("");
    assert(rc == 2);
  }
  {
    auto [rc, s] = run_chip("--help");
    assert(rc == 0 && s.find("Usage") != std::string::npos);
  }
}

void test_cli_complete_shell() {
  static_assert(IcRegistry::find("ap63205") != nullptr);
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"ap63205"};
    ic::complete<IcRegistry>(prior, "", out);
    const std::vector<std::string> want{"inductor", "cout",      "uvlo",
                                        "delay",    "thermal",   "spec"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 1> prior{"ap63205"};
    ic::complete<IcRegistry>(prior, "t", out);
    const std::vector<std::string> want{"thermal"};
    assert(out == want);
  }
  {
    std::vector<std::string> out;
    const std::array<std::string_view, 2> prior{"ap63205", "inductor"};
    ic::complete<IcRegistry>(prior, "", out); // op takes no further words
    assert(out.empty());
  }
  {
    Shell sh;
    sh.set_history_file("");
    std::ostringstream out;
    assert(sh.execute("ic ap63205 spec", out) == 0);
    assert(out.str().find("AP63205WU-7") != std::string::npos);
    assert(sh.execute("ic ap63205 inductor --vin 12V", out) == 0);
    assert(sh.execute("ic ap63205 bogus", out) == 2);
  }
}

} // namespace

int main() {
  test_constants();
  test_inductor();
  test_cout();
  test_uvlo();
  test_delay_thermal();
  test_cli_inductor();
  test_cli_cout_uvlo_delay();
  test_cli_thermal_spec();
  test_cli_complete_shell();
  std::cout << "test_ap63205: OK\n";
  return 0;
}
