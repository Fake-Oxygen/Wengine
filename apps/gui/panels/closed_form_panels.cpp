// Closed-form solver panel rendering (Dear ImGui). Each panel owns static
// text-field buffers, re-parses them every frame through its model, and
// shows either the CLI-identical result block or an inline CLI-parity
// error. Typing a valid unit string updates the result the same frame;
// nothing here can crash on bad input (parse failures are values, and
// ImGui::InputText owns its buffer bounds).

#include "panels/closed_form_panels.hpp"

#include "models/closed_form_models.hpp"
#include "imgui.h"

namespace wengine::gui {

namespace {

void show_result(const CfResult &r) {
  if (r.ok) {
    ImGui::TextUnformatted(r.text.c_str());
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
  ImGui::TextWrapped("Error: %s", r.error.c_str());
  ImGui::PopStyleColor();
}

} // namespace

void draw_ohm_panel() {
  static char u_buf[64] = "12.4V";
  static char i_buf[64] = "0.54A";
  static char r_buf[64] = "";
  ImGui::TextDisabled("Fill any two of U/I/R (unit suffixes accepted).");
  ImGui::InputText("U", u_buf, sizeof(u_buf));
  ImGui::InputText("I", i_buf, sizeof(i_buf));
  ImGui::InputText("R", r_buf, sizeof(r_buf));
  OhmModel model;
  model.u = u_buf;
  model.i = i_buf;
  model.r = r_buf;
  ImGui::Separator();
  show_result(model.evaluate());
}

void draw_s0_panel() {
  static const char *const kModes[] = {"rate", "power", "energy",
                                       "pulses", "convert", "full"};
  static S0Model model;
  static char power_buf[64] = "2kW";
  static char freq_buf[64] = "";
  static char pulses_buf[64] = "";
  static char const_buf[64] = "1000imp/kWh";
  static char time_buf[64] = "1h";
  static char energy_buf[64] = "";
  static char u_buf[64] = "";
  static char i_buf[64] = "";
  static char pf_buf[64] = "";
  ImGui::Combo("mode", &model.mode, kModes,
               static_cast<int>(sizeof(kModes) / sizeof(kModes[0])));
  ImGui::InputText("power", power_buf, sizeof(power_buf));
  ImGui::InputText("freq", freq_buf, sizeof(freq_buf));
  ImGui::InputText("pulses", pulses_buf, sizeof(pulses_buf));
  ImGui::InputText("const", const_buf, sizeof(const_buf));
  ImGui::InputText("time", time_buf, sizeof(time_buf));
  ImGui::InputText("energy", energy_buf, sizeof(energy_buf));
  ImGui::InputText("U", u_buf, sizeof(u_buf));
  ImGui::InputText("I", i_buf, sizeof(i_buf));
  ImGui::InputText("pf", pf_buf, sizeof(pf_buf));
  model.power = power_buf;
  model.freq = freq_buf;
  model.pulses = pulses_buf;
  model.const_c = const_buf;
  model.time = time_buf;
  model.energy = energy_buf;
  model.u = u_buf;
  model.i = i_buf;
  model.pf = pf_buf;
  ImGui::Separator();
  show_result(model.evaluate());
}

void draw_i2c_panel() {
  static const char *const kModes[] = {"pullup", "series", "full"};
  static const char *const kSpeeds[] = {"std", "fast", "fmp"};
  static I2cModel model;
  static char vdd_buf[64] = "3.3V";
  static char cb_buf[64] = "200pF";
  static char vol_buf[64] = "";
  static char iol_buf[64] = "";
  static char tr_buf[64] = "";
  static char rp_buf[64] = "1.2kOhm";
  static char rs_buf[64] = "100Ohm";
  ImGui::Combo("mode", &model.mode, kModes,
               static_cast<int>(sizeof(kModes) / sizeof(kModes[0])));
  ImGui::Combo("speed", &model.speed, kSpeeds,
               static_cast<int>(sizeof(kSpeeds) / sizeof(kSpeeds[0])));
  ImGui::InputText("VDD", vdd_buf, sizeof(vdd_buf));
  ImGui::InputText("Cb", cb_buf, sizeof(cb_buf));
  ImGui::InputText("VOL (auto)", vol_buf, sizeof(vol_buf));
  ImGui::InputText("IOL (auto)", iol_buf, sizeof(iol_buf));
  ImGui::InputText("tr (auto)", tr_buf, sizeof(tr_buf));
  ImGui::InputText("Rp (auto)", rp_buf, sizeof(rp_buf));
  ImGui::InputText("Rs (auto)", rs_buf, sizeof(rs_buf));
  model.vdd = vdd_buf;
  model.cb = cb_buf;
  model.vol = vol_buf;
  model.iol = iol_buf;
  model.tr = tr_buf;
  model.rp = rp_buf;
  model.rs = rs_buf;
  ImGui::Separator();
  show_result(model.evaluate());
}

const std::array<ClosedFormPanel, 3> kClosedFormPanels = {{
    {"Ohm", "Resistor U/I/R/P from any two", &draw_ohm_panel},
    {"S0", "Energy-meter pulse output (DIN 43864)", &draw_s0_panel},
    {"I2C", "Bus pull-up and series resistors (NXP UM10204)", &draw_i2c_panel},
}};

} // namespace wengine::gui
