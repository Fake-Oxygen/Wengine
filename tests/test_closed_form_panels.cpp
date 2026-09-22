// GUI closed-form solver panels: model parity with the CLI commands.
// Same field text must render the same golden substrings and the same
// validation messages as `ohm` / `s0` / `i2c`; the panel registry size is
// pinned so new solvers cannot miss registration.

#include "models/closed_form_models.hpp"
#include "panels/closed_form_panels.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <tuple>

namespace {

void test_registry_size() {
  // New solver panels extend kClosedFormPanels; update this count with them.
  static_assert(std::tuple_size_v<decltype(wengine::gui::kClosedFormPanels)> == 3);
}

void test_ohm() {
  using wengine::gui::OhmModel;
  // README golden: 12.4V + 0.54A -> R = 22.963 Ohm, P = 6.696 W.
  {
    OhmModel m;
    m.u = "12.4V";
    m.i = "0.54A";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok);
    assert(r.text == "U = 12.4 V\nI = 0.54 A\nR = 22.963 Ohm\nP = 6.696 W\n");
  }
  // Unit prefixes + bare numbers parse like the CLI.
  {
    OhmModel m;
    m.u = "1kV";
    m.r = "4Ohm";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("I = 250 A") != std::string::npos);
  }
  // Exactly-two rule with CLI wording.
  {
    OhmModel m;
    m.u = "12V";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "provide exactly 2 of U/I/R (got 1)");
  }
  // Invalid suffix reports the field and the reason, CLI-style.
  {
    OhmModel m;
    m.u = "12x";
    m.i = "3A";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "invalid voltage '12x' (unknown unit suffix)");
  }
  // Zero resistance on the U/R path is rejected like the CLI.
  {
    OhmModel m;
    m.u = "12V";
    m.r = "0Ohm";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "resistance must be non-zero");
  }
  // Surrounding whitespace is tolerated (CLI tokens never carry any).
  {
    OhmModel m;
    m.u = "  12V ";
    m.i = "3A";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("R = 4 Ohm") != std::string::npos);
  }
}

void test_s0() {
  using wengine::gui::S0Model;
  // Trusted CLI behaviors (see tests/test_s0.cpp).
  {
    S0Model m;
    m.mode = S0Model::Rate;
    m.power = "2kW";
    m.const_c = "1000imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("Hz") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Power;
    m.freq = "0.5555555556Hz";
    m.const_c = "1000imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("2000") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Energy;
    m.pulses = "500";
    m.const_c = "1000imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("0.5 kWh") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Pulses;
    m.power = "2kW";
    m.time = "1h";
    m.const_c = "1000imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("2000 imp") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Convert;
    m.const_c = "1imp/Wh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("1000 imp/kWh") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Full;
    m.u = "230V";
    m.i = "10A";
    m.const_c = "1000imp/kWh";
    m.time = "1h";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("2300") != std::string::npos);
  }
  // Missing inputs and bad values report CLI-worded errors, never crash.
  {
    S0Model m;
    m.mode = S0Model::Rate;
    m.power = "2kW";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "rate needs power and a meter constant");
  }
  {
    S0Model m;
    m.mode = S0Model::Rate;
    m.power = "2xW";
    m.const_c = "1000imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error.find("invalid power") != std::string::npos);
  }
  {
    S0Model m;
    m.mode = S0Model::Full;
    m.u = "230V";
    m.i = "10A";
    m.const_c = "0imp/kWh";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "meter constant must be > 0");
  }
}

void test_i2c() {
  using wengine::gui::I2cModel;
  // Trusted CLI behaviors (see tests/test_i2c.cpp).
  {
    I2cModel m;
    m.mode = I2cModel::Pullup;
    m.speed = I2cModel::Std;
    m.vdd = "5.5V";
    m.cb = "100pF";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("Rp(min) = 1700") != std::string::npos);
  }
  {
    I2cModel m;
    m.mode = I2cModel::Pullup;
    m.vdd = "3.3V";
    m.cb = "200pF";
    m.rp = "4.7kOhm";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("FAIL") != std::string::npos);
  }
  {
    I2cModel m;
    m.mode = I2cModel::Full;
    m.vdd = "3.3V";
    m.cb = "200pF";
    m.rp = "1.2kOhm";
    m.rs = "100Ohm";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("summary: PASS") != std::string::npos);
  }
  {
    I2cModel m;
    m.mode = I2cModel::Series;
    m.vdd = "3.3V";
    m.cb = "200pF";
    m.rp = "1.2kOhm";
    m.rs = "100Ohm";
    const wengine::gui::CfResult r = m.evaluate();
    assert(r.ok && r.text.find("VOL(bus)") != std::string::npos);
  }
  // Missing inputs report CLI-worded errors.
  {
    I2cModel m;
    m.mode = I2cModel::Pullup;
    m.cb = "200pF";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error == "need VDD and bus capacitance");
  }
  {
    I2cModel m;
    m.mode = I2cModel::Pullup;
    m.vdd = "3.3V";
    m.cb = "20xF";
    const wengine::gui::CfResult r = m.evaluate();
    assert(!r.ok && r.error.find("invalid bus capacitance") != std::string::npos);
  }
}

} // namespace

int main() {
  test_registry_size();
  test_ohm();
  test_s0();
  test_i2c();
  std::printf("test_closed_form_panels: OK\n");
  return 0;
}
