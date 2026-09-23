#include "wengine/core/runner.hpp"
#include "wengine/emit/signal_sink.hpp"
#include "wengine/link/frame.hpp"
#include "wengine/link/protocol.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/run.hpp"
#include "wengine/solvers/signal.hpp"
#include "wengine/solvers/solver_registry.hpp"
#include "wengine/world/serialize.hpp"
#include "wengine/cli/args.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

using namespace wengine;

// Scaling-path proof support (file scope: explicit specializations must be
// declared at namespace scope enclosing wengine::units).
namespace scaling_proof {
using TestTemp = units::Dimension<0, 0, 0, 0, 1, 0, 0>;
constexpr units::Value<TestTemp> operator""_tC(long double v) {
  return units::Value<TestTemp>(static_cast<double>(v));
}
} // namespace scaling_proof

template <>
struct units::scales_for<scaling_proof::TestTemp> {
  static constexpr std::array<units::SuffixScale, 2> value{{{"tC", 1.0}, {"mtC", 1e-3}}};
};

namespace {

constexpr bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

signal::SignalParams sine2v() {
  signal::SignalParams p;
  p.kind = signal::SignalKind::kSine;
  p.freq_hz = 1.0;
  p.amp = 2.0;
  return p;
}

void test_eval_goldens() {
  // Independent references: hand-derived closed forms (noise u01 verified
  // against a separate Python splitmix64 implementation 2026-09-23).
  const signal::SignalParams s = sine2v();
  assert(near(signal::eval_signal(s, 0.0, 0.0), 0.0, 1e-12)); // sin(0)
  assert(near(signal::eval_signal(s, 0.25, 0.0), 2.0, 1e-12)); // quarter period
  assert(near(signal::eval_signal(s, 0.5, 0.0), 0.0, 1e-12)); // half period
  assert(near(signal::eval_signal(s, 1.0, 0.0), 0.0, 1e-9)); // full period (libm)

  signal::SignalParams sq;
  sq.kind = signal::SignalKind::kSquare;
  sq.amp = 3.0;
  sq.offset = 1.0;
  assert(signal::eval_signal(sq, 0.0, 0.0) == 4.0); // exact: compare only
  assert(signal::eval_signal(sq, 0.75, 0.0) == -2.0);

  signal::SignalParams pu;
  pu.kind = signal::SignalKind::kPulse;
  pu.amp = 5.0;
  pu.duty = 0.25;
  assert(signal::eval_signal(pu, 0.0, 0.0) == 5.0);
  assert(signal::eval_signal(pu, 0.5, 0.0) == 0.0);

  signal::SignalParams tr;
  tr.kind = signal::SignalKind::kTriangle;
  tr.amp = 2.0;
  tr.offset = 1.0;
  assert(signal::eval_signal(tr, 0.0, 0.0) == -1.0);
  assert(signal::eval_signal(tr, 0.5, 0.0) == 3.0);
  assert(signal::eval_signal(tr, 0.25, 0.0) == 1.0);

  signal::SignalParams saw;
  saw.kind = signal::SignalKind::kSaw;
  saw.amp = 2.0;
  assert(signal::eval_signal(saw, 0.0, 0.0) == -2.0);
  assert(near(signal::eval_signal(saw, 0.5, 0.0), 0.0, 1e-12));

  signal::SignalParams dc;
  dc.kind = signal::SignalKind::kDc;
  dc.offset = 3.3;
  assert(signal::eval_signal(dc, 123.456, 0.0) == 3.3);

  // Noise golden: seed 1 -> u01 = 0.5665615751722809 (independent Python
  // reference); single rounding op past that, hence the tight epsilon.
  signal::SignalParams nz;
  nz.kind = signal::SignalKind::kNoise;
  nz.amp = 3.3;
  nz.seed = 1;
  const double u = signal::u01_from_bits(signal::rng_next(1));
  assert(near(u, 0.5665615751722809, 1e-15));
  assert(near(signal::eval_signal(nz, 0.0, u), 0.4393063961370539, 1e-12));
}

void test_parse_and_validate() {
  assert(signal::parse_kind("sine").value == signal::SignalKind::kSine);
  assert(signal::parse_kind("noise").value == signal::SignalKind::kNoise);
  assert(!signal::parse_kind("sawtooth").ok());
  assert(!signal::parse_kind("").ok());
  assert(signal::parse_quantity("U").value == signal::QuantityId::kVoltage);
  assert(signal::parse_quantity("I").value == signal::QuantityId::kCurrent);
  assert(!signal::parse_quantity("W").ok());
  assert(signal::quantity_suffix(signal::QuantityId::kVoltage) == "U");
  assert(signal::quantity_suffix(signal::QuantityId::kCurrent) == "I");

  signal::SignalParams p;
  assert(signal::validate_params(p).ok());
  p.freq_hz = 0.0;
  assert(!signal::validate_params(p).ok()); // sine needs f > 0
  p.kind = signal::SignalKind::kDc;
  assert(signal::validate_params(p).ok()); // dc ignores freq
  p.kind = signal::SignalKind::kNoise;
  assert(signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kSquare;
  p.freq_hz = 1.0;
  p.duty = 1.5;
  assert(!signal::validate_params(p).ok());
  p.duty = 1.0;
  assert(signal::validate_params(p).ok());
  p.kind = signal::SignalKind::kSine;
  assert(signal::validate_params(p).ok()); // sine ignores duty
}

world::Entity spawn(core::Runner &runner, const signal::SignalParams &p) {
  world::Entity e = runner.state().spawn_entity();
  assert(world::configure_source(e, runner.state().signals, p, runner.state().sim_time).ok());
  return e;
}

void test_solver_golden_and_snapshot() {
  // 1 Hz sine, dt = 1 ms, 1000 steps -> t = 1 s (one full period).
  core::Runner runner{{.dt = 0.001}};
  solvers::SignalSolver solver;
  assert(solver.name() == "signal");
  runner.add_solver(solver);
  spawn(runner, sine2v());
  assert(runner.run(1000).ok());

  const world::State &st = runner.state();
  assert(st.step_index == 1000 && near(st.sim_time, 1.0, 1e-9));
  // End-of-step convention: snapshot at T carries value(T) = 2*sin(2pi).
  assert(near(st.signals.value[0], 0.0, 1e-9));

  const world::Snapshot snap = world::take_snapshot(st);
  assert(snap.signal_count() == 1);
  assert(snap.sig_serial[0] == 0);
  assert(snap.sig_quantity[0] == static_cast<std::uint8_t>(signal::QuantityId::kVoltage));
  assert(snap.sig_value[0] == st.signals.value[0]);
  assert(snap.particle_count() == 0);
}

void test_noise_sequence_and_determinism() {
  auto drive = [] {
    core::Runner r{{.dt = 0.01}};
    solvers::SignalSolver s;
    r.add_solver(s);
    signal::SignalParams p;
    p.kind = signal::SignalKind::kNoise;
    p.amp = 1.0;
    p.seed = 7;
    spawn(r, p);
    assert(r.run(5).ok());
    return r;
  };
  const core::Runner r1 = drive();
  const core::Runner r2 = drive();
  assert(r1.state() == r2.state()); // exact repeat
  // Sequence advances one draw per step in slot order: rng holds draw 5.
  std::uint64_t rng = 7;
  double last = 0.0;
  for (int k = 0; k < 5; ++k) {
    rng = signal::rng_next(rng);
    last = 2.0 * signal::u01_from_bits(rng) - 1.0;
  }
  assert(r1.state().signals.rng[0] == rng);
  assert(r1.state().signals.value[0] == last);
}

void test_holes_and_bad_dt() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  signal::SignalParams dc;
  dc.kind = signal::SignalKind::kDc;
  dc.offset = 9.0;
  spawn(runner, dc);
  runner.state().signals.detach(1); // hole: solver must skip, value frozen
  assert(runner.run(10).ok());
  assert(runner.state().signals.value[1] == 9.0);

  world::State s;
  assert(!solver.step(s, 0.0).ok());
  assert(!solver.step(s, -0.01).ok());
}

void test_run_headless_signal() {
  core::RunConfig cfg;
  cfg.loop.dt = 0.01;
  cfg.steps = 10;
  cfg.solver = "signal";
  cfg.particles = 0;
  world::State out;
  assert(solvers::run_headless(cfg, std::span<emit::ISink *>{}, out).ok());
  assert(out.signals.live_count == 1);
  // Default sine 1 Hz at t = 0.1 s: sin(2*pi*0.1).
  assert(near(out.signals.value[0], 0.5877852522924731, 1e-9));

  // Batch seeds derive (seed + index): two noise sources never alias.
  core::RunConfig cfg2 = cfg;
  cfg2.signal.params.kind = signal::SignalKind::kNoise;
  cfg2.signal.params.amp = 1.0;
  cfg2.signal.count = 2;
  world::State out2;
  assert(solvers::run_headless(cfg2, std::span<emit::ISink *>{}, out2).ok());
  assert(out2.signals.live_count == 2);
  assert(out2.signals.value[0] != out2.signals.value[1]);
  assert(out2.signals.seed[0] == 1 && out2.signals.seed[1] == 2);

  // Rejections: set signal section with projectile, gravity with signal,
  // invalid params (sine at 0 Hz, square duty out of range).
  core::RunConfig bad = cfg;
  bad.solver = "projectile";
  bad.signal.set = true;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.has_gravity = true;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.signal.params.freq_hz = 0.0;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
  bad = cfg;
  bad.signal.params.kind = signal::SignalKind::kSquare;
  bad.signal.params.duty = 2.0;
  assert(!solvers::run_headless(bad, std::span<emit::ISink *>{}, out).ok());
}

void test_frame_adapter() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams p = sine2v();
  p.quantity = signal::QuantityId::kCurrent;
  spawn(runner, p);
  assert(runner.run(3).ok());
  const link::Frame f =
      link::frame_from_signal_snapshot(world::take_snapshot(runner.state()));
  assert(f.step == 3 && near(f.time, 0.03, 1e-12));
  assert(f.channels.size() == 2);
  assert(f.channels[0].name == "sig.count" && f.channels[0].value == 1.0);
  assert(f.channels[1].name == "sig.0.I");
  assert(f.channels[1].value == runner.state().signals.value[0]);
  assert(f.items.empty());

  // Encode/decode round-trip is exact (existing wire path, new payload).
  link::LinkFrame wire{};
  assert(link::encode_frame(f, wire, 42).ok());
  link::Frame back;
  assert(link::decode_frame(wire, back).ok());
  assert(back == f);
}

void test_signals_csv_round_trip() {
  const std::string path = "/tmp/wengine_test_signal.csv";
  std::remove(path.c_str());
  core::Runner runner{{.dt = 0.01, .emit_every = 2}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  emit::SignalsCsvSink sink(path);
  assert(sink.ok());
  runner.add_sink(sink);
  assert(runner.run(6).ok());
  assert(sink.flush().ok());
  assert(sink.rows_written() == 3); // steps 2, 4, 6

  std::ifstream in(path);
  std::string header;
  assert(static_cast<bool>(std::getline(in, header)));
  assert(header == "step,time,sig_id,value,quantity");
  std::vector<world::Snapshot> snaps;
  std::string error;
  in.clear();
  in.seekg(0);
  assert(emit::parse_signals_log(in, snaps, error).ok());
  assert(snaps.size() == 3);
  assert(snaps[0].step_index == 2 && snaps[0].signal_count() == 1);
  assert(snaps[0].sig_serial[0] == 0 && snaps[0].sig_quantity[0] == 0);
  // Exact round-trip: max_digits10 doubles re-parse bit-identically.
  assert(snaps[2].sig_value[0] == runner.state().signals.value[0]);
  std::remove(path.c_str());

  // Malformed logs rejected loudly.
  for (const std::string &bad :
       {"", "step,time,slot,x\n", "step,time,sig_id,value,quantity\n1,0.01,0,abc,U\n",
        "step,time,sig_id,value,quantity\n1,0.01,0,0.5,X\n"}) {
    std::istringstream bin(bad);
    assert(!emit::parse_signals_log(bin, snaps, error).ok());
    assert(!error.empty() && snaps.empty());
  }
}

void test_serialize_signals() {
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  spawn(runner, sine2v());
  assert(runner.run(5).ok());

  std::ostringstream bytes;
  assert(world::save_state(runner.state(), bytes).ok());
  world::State dst;
  std::istringstream in(bytes.str());
  assert(world::load_state(dst, in).ok());
  assert(dst == runner.state());
  assert(world::take_snapshot(dst) == world::take_snapshot(runner.state()));

  // v1 blob (no signal section) still loads: signals come back empty.
  std::string v1;
  v1 += 'W';
  v1 += 'E';
  v1 += 'N';
  v1 += 'G';
  const std::uint32_t ver = 1;
  for (int k = 0; k < 4; ++k) {
    v1 += static_cast<char>((ver >> (8 * k)) & 0xFFu);
  }
  v1.append(8, '\0'); // sim_time
  v1.append(8, '\0'); // step_index
  v1.append(4, '\0'); // entities
  v1.append(4, '\0'); // freelist
  v1.append(4, '\0'); // particles
  world::State old;
  std::istringstream vin(v1);
  assert(world::load_state(old, vin).ok());
  assert(old == world::State{});
  assert(world::take_snapshot(old).signal_count() == 0);
}

void test_registry() {
  static_assert(solvers::SolverRegistry::find("signal") != nullptr);
  static_assert(solvers::SolverRegistry::all().size() == 2);
  const solvers::SolverDef *def = solvers::SolverRegistry::find("signal");
  assert(def != nullptr);
  std::unique_ptr<core::ISolver> solver = def->create();
  assert(solver && solver->name() == "signal");
}

void test_quantity_table() {
  // Single-table round-trips: every spelling parses, every id has a
  // suffix/name/scales row, codes are dense (sync with DimensionFor).
  static_assert(signal::kQuantityCount == 2);
  for (const signal::QuantityDesc &d : signal::kQuantities) {
    assert(!d.suffix.empty() && !d.name.empty() && !d.scales.empty());
    for (std::string_view spelling : d.spellings) {
      assert(signal::parse_quantity(spelling).value == d.id);
    }
    assert(signal::quantity_suffix(d.id) == d.suffix);
    assert(signal::quantity_name(d.id) == d.name);
  }
  assert(signal::quantity_suffix(static_cast<signal::QuantityId>(9)) == "?");
  assert(!signal::parse_quantity("W").ok());
  assert(!signal::parse_quantity("").ok());

  // Quantity-aware value parsing through the units-owned tables (the only
  // parser commands need): voltage suffixes work for U, current suffixes
  // for I, and cross-dimension text is rejected — the dimensional firewall.
  using wengine::cli::parse_quantity_value;
  using signal::QuantityId;
  auto u = parse_quantity_value(QuantityId::kVoltage, "3.3kV");
  assert(u.ok() && u.value == 3300.0);
  assert(parse_quantity_value(QuantityId::kVoltage, "12V").value == 12.0);
  assert(parse_quantity_value(QuantityId::kVoltage, "5mV").value == 0.005);
  auto i = parse_quantity_value(QuantityId::kCurrent, "2mA");
  assert(i.ok() && i.value == 0.002);
  assert(!parse_quantity_value(QuantityId::kVoltage, "2mA").ok());
  assert(!parse_quantity_value(QuantityId::kCurrent, "3.3kV").ok());
  assert(!parse_quantity_value(QuantityId::kVoltage, "abc").ok());
  assert(!parse_quantity_value(static_cast<QuantityId>(9), "1V").ok());
}

void test_stamped_values() {
  // Typed boundary: stamp() carries the dimension tag AND a matching
  // Value<Dim>; misreading volts as amps fails to compile wherever the
  // variant is consumed (checked here at runtime over live snapshot data).
  using signal::QuantityId;
  using units::ElectricCurrent;
  using units::ElectricPotential;
  const signal::StampedValue v = signal::stamp(QuantityId::kVoltage, 3.3);
  assert(std::holds_alternative<units::Value<ElectricPotential>>(v));
  assert(std::get<units::Value<ElectricPotential>>(v).get() == 3.3);
  const signal::StampedValue c = signal::stamp(QuantityId::kCurrent, 0.05);
  assert(std::holds_alternative<units::Value<ElectricCurrent>>(c));

  // Stamp every live sample of a real run and verify dimension + value.
  core::Runner runner{{.dt = 0.01}};
  solvers::SignalSolver solver;
  runner.add_solver(solver);
  signal::SignalParams p;
  p.quantity = QuantityId::kCurrent;
  p.kind = signal::SignalKind::kDc;
  p.offset = 0.5;
  spawn(runner, p);
  assert(runner.run(2).ok());
  const world::Snapshot snap = world::take_snapshot(runner.state());
  assert(snap.signal_count() == 1);
  const signal::StampedValue s =
      signal::stamp(static_cast<QuantityId>(snap.sig_quantity[0]), snap.sig_value[0]);
  assert(std::holds_alternative<units::Value<ElectricCurrent>>(s));
  assert(std::get<units::Value<ElectricCurrent>>(s).get() == 0.5);
  // std::visit dispatches on the stamped dimension (the control-plane
  // pattern: one handler per physical quantity, chosen by the tag).
  double visited = -1.0;
  const char *seen = "";
  std::visit(
      [&](const auto &val) {
        visited = val.get();
        using V = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<V, units::Value<ElectricCurrent>>) {
          seen = "current";
        } else {
          seen = "voltage";
        }
      },
      s);
  assert(visited == 0.5 && std::string_view(seen) == "current");
}

// Scaling-path proof: a test-only dimension (no production change) plugs
// into the generic units machinery — Dimension + literal + one scales row
// is the whole recipe. This is the "one-entry change" SIG-3 requires.
void test_dimension_scaling() {
  using scaling_proof::TestTemp;
  using wengine::cli::detail::parse_scaled_impl;
  // Generic suffix-scale parsing accepts the new dimension untouched.
  const auto a = parse_scaled_impl("12mtC", units::scales_for<TestTemp>::value);
  assert(a.ok() && a.value == 0.012);
  assert(!parse_scaled_impl("12K", units::scales_for<TestTemp>::value).ok());
  // Value arithmetic + exact comparison work like every other dimension.
  constexpr units::Value<TestTemp> t = scaling_proof::operator""_tC(2.0);
  static_assert(t.get() == 2.0);
  assert(t + t == scaling_proof::operator""_tC(4.0));
}

} // namespace

int main() {
  test_eval_goldens();
  test_parse_and_validate();
  test_solver_golden_and_snapshot();
  test_noise_sequence_and_determinism();
  test_holes_and_bad_dt();
  test_run_headless_signal();
  test_frame_adapter();
  test_signals_csv_round_trip();
  test_serialize_signals();
  test_registry();
  test_quantity_table();
  test_stamped_values();
  test_dimension_scaling();
  std::cout << "test_signal: OK\n";
  return 0;
}
