#pragma once

// Dear ImGui signal-source panel: batch signal configuration (kind,
// stamped params, run length) with an explicit Apply step, plus live
// per-source control (list/add/set/start/stop/drop) over the controller's
// SIG-10 verb bus — driving the shared SimController through the
// engine-only SignalModel (batch) and the same verb lines the CLI
// `sig add/set/start/stop/drop` family and Zephyr clients speak.
//
// Batch section: kind/quantity combos, stamped text fields, expression box
// (kind=expression), table CSV picker (kind=table: the panel loads the
// file — GUI owns file I/O — the model parses the text), Lua script
// picker (kind=lua: needs the scripting runtime the app wires into the
// controller; without it Apply rejects inline, never silently). Apply
// rebuilds the run from step 0 and forwards the stored runtime so batch
// tweaks never silently drop it.
//
// Per-source section (signal solver only): live source census read from
// sim.state() in slot order (serial, kind, quantity, params, running flag)
// with Start/Stop/Drop buttons, an ADD form (parametric + expression +
// sums with operand serials; table/lua need bulk payloads and ride batch
// rebuilds, like the CLI family), and a SET form (serial + param +
// value). Values parse at this edge with the same dimensional firewall as
// the CLI (unit suffixes accepted, SI doubles on the wire via %.17g); the
// wire carries SI only. Verbs enqueue on the local bus channel and drain
// immediately for instant feedback, and again at every step boundary, so
// clicks between steps apply exactly once at a deterministic step.
//
// Transport (Play/Pause/Step) stays in SimPanel; both panels drive the
// same controller without fighting: SimPanel rebuilds only on its own
// fields' diff, and this panel applies batch only on button press (no
// per-keystroke rebuilds).
//
// ID scope: this panel shares the "WEngine" window with SimPanel, so every
// widget label carries a ##signal suffix (duplicate labels across panels
// would otherwise collide and misbehave); per-row buttons additionally
// carry the stable source serial.

#include "imgui.h"
#include "controllers/sim_controller.hpp"
#include "models/signal_model.hpp"
#include "wengine/cli/args.hpp"
#include "wengine/signal/types.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace wengine::gui {

namespace detail {

[[nodiscard]] inline const char *signal_kind_getter(void *data, int idx) {
  (void)data;
  if (idx < 0 ||
      static_cast<std::size_t>(idx) >= signal::kKindNames.size()) {
    return "";
  }
  // Kind names are static string literals: NUL-terminated and stable.
  return signal::kKindNames[static_cast<std::size_t>(idx)].data();
}

[[nodiscard]] inline const char *signal_param_getter(void *data, int idx) {
  (void)data;
  static constexpr const char *kParams[7] = {"FREQ", "AMP", "OFFSET",
                                             "PHASE", "DUTY", "SEED",
                                             "QUANTITY"};
  if (idx < 0 || idx >= 7) {
    return "";
  }
  return kParams[idx];
}

// %.17g round-trips IEEE doubles exactly (same guarantee as the CSV log,
// the LIST lines, and the CLI wire formatter); the engine parses back
// with from_chars.
[[nodiscard]] inline std::string signal_wire_f64(double v) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(buf)) {
    return "0";
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

// Read a whole file into text (GUI owns file I/O; the model parses).
[[nodiscard]] inline bool signal_load_text(const char *path, std::string &out,
                                           std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = std::string("cannot open file '") + path + "'";
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (!in && !in.eof()) {
    error = std::string("cannot read file '") + path + "'";
    return false;
  }
  out = buf.str();
  return true;
}

} // namespace detail

struct SignalPanel {
  static constexpr std::size_t kFieldSize = 32;
  char freq[kFieldSize] = "1";
  char amp[kFieldSize] = "1";
  char offset[kFieldSize] = "0";
  char phase[kFieldSize] = "0";
  char duty[kFieldSize] = "0.5";
  char seed[kFieldSize] = "1";
  char count[kFieldSize] = "1";
  char dt[kFieldSize] = "0.01";
  char steps[kFieldSize] = "100";
  char emit_every[kFieldSize] = "1";
  int kind_idx = 0; // index into signal::kKindNames (sine default)
  int qty_idx = 0; // 0 = U, 1 = I
  static constexpr std::size_t kExprSize = 256;
  char expr[kExprSize] = "";
  char table_path[256] = "";
  char lua_path[256] = "";
  std::string table_csv; // loaded table document (kind=table only)
  std::string lua_script; // loaded script text (kind=lua only)
  std::string error; // last Apply rejection (frontend-owned, may allocate)
  std::string load_error; // last file-load failure
  // Per-source ADD form (empty option fields are omitted from the line,
  // exactly like missing CLI options — the engine fills its defaults).
  int add_kind_idx = 0;
  int add_qty_idx = 0;
  char add_freq[kFieldSize] = "";
  char add_amp[kFieldSize] = "";
  char add_offset[kFieldSize] = "";
  char add_phase[kFieldSize] = "";
  char add_duty[kFieldSize] = "";
  char add_seed[kFieldSize] = "";
  char add_ops[64] = "";
  char add_expr[kExprSize] = "";
  // Per-source SET form.
  char set_serial[kFieldSize] = "0";
  int set_param_idx = 0; // index into FREQ..QUANTITY
  char set_value[kFieldSize] = "";
  std::string verb_status; // last verb verdict line (OK ... / ERR ...)

  void sync_from(const core::RunConfig &cfg) {
    if (cfg.solver != solvers::SignalSolver::kName) {
      return; // leave fields alone when another solver drives the run
    }
    const signal::SignalParams &p = cfg.signal.params;
    for (std::size_t k = 0; k < signal::kKindNames.size(); ++k) {
      if (static_cast<signal::SignalKind>(k) == p.kind) {
        kind_idx = static_cast<int>(k);
      }
    }
    qty_idx = (p.quantity == signal::QuantityId::kCurrent) ? 1 : 0;
    std::snprintf(freq, kFieldSize, "%g", p.freq_hz);
    std::snprintf(amp, kFieldSize, "%g", p.amp);
    std::snprintf(offset, kFieldSize, "%g", p.offset);
    std::snprintf(phase, kFieldSize, "%g", p.phase_rad);
    std::snprintf(duty, kFieldSize, "%g", p.duty);
    std::snprintf(seed, kFieldSize, "%llu", static_cast<unsigned long long>(p.seed));
    std::snprintf(count, kFieldSize, "%llu",
                  static_cast<unsigned long long>(cfg.signal.count));
    std::snprintf(dt, kFieldSize, "%g", cfg.loop.dt);
    std::snprintf(steps, kFieldSize, "%llu", static_cast<unsigned long long>(cfg.steps));
    std::snprintf(emit_every, kFieldSize, "%llu",
                  static_cast<unsigned long long>(cfg.loop.emit_every));
    // Expression text, table documents, and script text are panel-owned
    // payloads with no unparser/file round-trip: left untouched so a
    // controller-driven rebuild never wipes what the user loaded.
  }

  // Enqueue one verb line and drain immediately (plus the automatic drain
  // at every step boundary, which then finds an empty queue — clicks apply
  // exactly once). The verdict line lands in verb_status for inline
  // display; rejected lines change nothing.
  void run_verb(SimController &sim, const std::string &line) {
    if (!sim.send_command(line)) {
      verb_status = "ERR internal bus unavailable";
      return;
    }
    (void)sim.drain_commands();
    const std::vector<std::string> resps = sim.take_responses();
    verb_status = resps.empty() ? "ERR internal no verdict" : resps.front();
  }

  void draw_batch(SimController &sim) {
    ImGui::Separator();
    ImGui::Text("Signals (batch source)");
    ImGui::Combo("kind##signal", &kind_idx, &detail::signal_kind_getter, nullptr,
                 static_cast<int>(signal::kKindNames.size()));
    const char *qty_items[2] = {"U", "I"};
    ImGui::Combo("quantity##signal", &qty_idx, qty_items, 2);
    ImGui::InputText("freq (Hz)##signal", freq, kFieldSize);
    ImGui::InputText("amp##signal", amp, kFieldSize);
    ImGui::InputText("offset##signal", offset, kFieldSize);
    ImGui::InputText("phase (rad)##signal", phase, kFieldSize);
    ImGui::InputText("duty##signal", duty, kFieldSize);
    ImGui::InputText("seed##signal", seed, kFieldSize);
    ImGui::InputText("count##signal", count, kFieldSize);
    ImGui::InputText("dt (s)##signal", dt, kFieldSize);
    ImGui::InputText("steps##signal", steps, kFieldSize);
    ImGui::InputText("emit every##signal", emit_every, kFieldSize);
    ImGui::InputText("expr##signal", expr, kExprSize);
    ImGui::TextDisabled("kind=expression evaluates the box above.");
    ImGui::InputText("table csv##signal", table_path, sizeof(table_path));
    ImGui::SameLine();
    if (ImGui::Button("Load table##signal")) {
      std::string text;
      if (detail::signal_load_text(table_path, text, load_error)) {
        table_csv = std::move(text);
        load_error.clear();
      } else {
        table_csv.clear();
      }
    }
    ImGui::TextDisabled("kind=table replays the loaded file (%llu points).",
                        static_cast<unsigned long long>(table_csv.empty() ? 0 : 1));
    ImGui::InputText("lua file##signal", lua_path, sizeof(lua_path));
    ImGui::SameLine();
    if (ImGui::Button("Load lua##signal")) {
      std::string text;
      if (detail::signal_load_text(lua_path, text, load_error)) {
        lua_script = std::move(text);
        load_error.clear();
      } else {
        lua_script.clear();
      }
    }
    if (!table_csv.empty()) {
      ImGui::TextDisabled("table: %llu bytes loaded.", static_cast<unsigned long long>(table_csv.size()));
    }
    if (!lua_script.empty()) {
      ImGui::TextDisabled("lua: %llu bytes loaded.", static_cast<unsigned long long>(lua_script.size()));
    }
    if (!load_error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      ImGui::TextWrapped("Load error: %s", load_error.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("Apply rebuilds the run from step 0.");
    if (ImGui::Button("Apply##signal")) {
      SignalModel model;
      model.kind = signal::kKindNames[static_cast<std::size_t>(kind_idx)];
      model.quantity = qty_idx == 1 ? "I" : "U";
      model.freq = freq;
      model.amp = amp;
      model.offset = offset;
      model.phase = phase;
      model.duty = duty;
      model.seed = seed;
      model.count = count;
      model.dt = dt;
      model.steps = steps;
      model.emit_every = emit_every;
      model.expr = expr;
      model.table_csv = table_csv;
      model.lua_script = lua_script;
      const SignalBuild built = model.build();
      if (built.ok) {
        error.clear();
        // Forward the stored runtime so batch tweaks never silently drop
        // the scripting solver (panels must preserve what they don't own).
        const core::Status st =
            sim.set_config(built.cfg, sim.extras(), sim.has_lua_runtime());
        if (!st.ok()) {
          error = sim.error();
        }
      } else {
        error = built.error;
      }
    }
    if (!error.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      ImGui::TextWrapped("Error: %s", error.c_str());
      ImGui::PopStyleColor();
    }
    // Script failures fail the step loudly through the solver; the text
    // (lent by the app-owned runtime) shows here, beside the status line.
    const std::string script_err = sim.script_error();
    if (!script_err.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      ImGui::TextWrapped("Lua: %s", script_err.c_str());
      ImGui::PopStyleColor();
    }
  }

  void draw_sources(SimController &sim) {
    ImGui::Separator();
    ImGui::Text("Sources (live verbs)");
    if (sim.config().solver != solvers::SignalSolver::kName) {
      ImGui::TextDisabled("Select the signal solver to tune sources.");
      return;
    }
    const world::Signals &pool = sim.state().signals;
    bool any = false;
    for (std::uint32_t i = 0; i < pool.capacity(); ++i) {
      if (!pool.has(i)) {
        continue;
      }
      any = true;
      const std::uint64_t serial = pool.serial[i];
      const std::size_t kind_idx_local = pool.kind[i];
      const std::string_view kind_name =
          (kind_idx_local < signal::kKindNames.size())
              ? signal::kKindNames[kind_idx_local]
              : "?";
      const auto qty = static_cast<signal::QuantityId>(pool.quantity[i]);
      const bool running = pool.running[i] != 0;
      ImGui::Text("s%llu %.*s %.*s f=%g a=%g %s",
                  static_cast<unsigned long long>(serial),
                  static_cast<int>(kind_name.size()), kind_name.data(),
                  static_cast<int>(signal::quantity_suffix(qty).size()),
                  signal::quantity_suffix(qty).data(), pool.freq[i],
                  pool.amp[i], running ? "(running)" : "(stopped)");
      ImGui::SameLine();
      const std::string start_id =
          "Start##signal" + std::to_string(serial);
      if (ImGui::Button(start_id.c_str())) {
        run_verb(sim, "START " + std::to_string(serial));
      }
      ImGui::SameLine();
      const std::string stop_id = "Stop##signal" + std::to_string(serial);
      if (ImGui::Button(stop_id.c_str())) {
        run_verb(sim, "STOP " + std::to_string(serial));
      }
      ImGui::SameLine();
      const std::string drop_id = "Drop##signal" + std::to_string(serial);
      if (ImGui::Button(drop_id.c_str())) {
        run_verb(sim, "DROP " + std::to_string(serial));
      }
    }
    if (!any) {
      ImGui::TextDisabled("no live sources (Apply a batch or ADD one).");
    }
    // ADD form: same keys as `sig add` (only non-empty fields join the
    // line); table/lua need bulk payloads and ride batch rebuilds.
    ImGui::Combo("add kind##signal", &add_kind_idx,
                 &detail::signal_kind_getter, nullptr,
                 static_cast<int>(signal::kKindNames.size()));
    const char *add_qty_items[2] = {"U", "I"};
    ImGui::Combo("add quantity##signal", &add_qty_idx, add_qty_items, 2);
    ImGui::InputText("add freq##signal", add_freq, kFieldSize);
    ImGui::InputText("add amp##signal", add_amp, kFieldSize);
    ImGui::InputText("add offset##signal", add_offset, kFieldSize);
    ImGui::InputText("add phase##signal", add_phase, kFieldSize);
    ImGui::InputText("add duty##signal", add_duty, kFieldSize);
    ImGui::InputText("add seed##signal", add_seed, kFieldSize);
    ImGui::InputText("add ops (s0,s1)##signal", add_ops, sizeof(add_ops));
    ImGui::InputText("add expr##signal", add_expr, kExprSize);
    if (ImGui::Button("Add source##signal")) {
      add_source(sim);
    }
    // SET form: one param on one serial (unit suffixes accepted for
    // FREQ/AMP/OFFSET, resolved against the target's own quantity).
    ImGui::InputText("set serial##signal", set_serial, kFieldSize);
    ImGui::Combo("set param##signal", &set_param_idx,
                 &detail::signal_param_getter, nullptr, 7);
    ImGui::InputText("set value##signal", set_value, kFieldSize);
    if (ImGui::Button("Set param##signal")) {
      set_param(sim);
    }
    if (!verb_status.empty()) {
      const bool ok = verb_status.size() >= 2 && verb_status[0] == 'O' &&
                      verb_status[1] == 'K';
      if (!ok) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.4F, 0.4F, 1.0F));
      }
      ImGui::TextWrapped("%s", verb_status.c_str());
      if (!ok) {
        ImGui::PopStyleColor();
      }
    }
  }

  void draw(SimController &sim) {
    draw_batch(sim);
    draw_sources(sim);
  }

 private:
  // Dimensional firewall at the panel edge (mirrors the CLI family):
  // quantity-aware amp/offset, frequency for FREQ, plain doubles else.
  // Returns false with verb_status set when the text is invalid (nothing
  // is sent); SI values land in `out`.
  bool parse_add_fields(signal::QuantityId qty, double &freq_out, double &amp_out,
                        double &offset_out, double &phase_out, double &duty_out,
                        std::uint64_t &seed_out, bool &have_freq, bool &have_amp,
                        bool &have_offset, bool &have_phase, bool &have_duty,
                        bool &have_seed) {
    auto fail = [&](const std::string &msg) {
      verb_status = msg;
      return false;
    };
    have_freq = have_amp = have_offset = have_phase = have_duty = have_seed =
        false;
    const std::string_view freq_t = signal_detail::trim(add_freq);
    if (!freq_t.empty()) {
      const auto r = cli::parse_frequency(freq_t);
      if (!r.ok()) {
        return fail("invalid add freq '" + std::string(freq_t) + "'");
      }
      freq_out = r.value;
      have_freq = true;
    }
    const std::string_view amp_t = signal_detail::trim(add_amp);
    if (!amp_t.empty()) {
      const auto r = cli::parse_quantity_value(qty, amp_t);
      if (!r.ok()) {
        return fail("invalid add amp '" + std::string(amp_t) + "'");
      }
      amp_out = r.value;
      have_amp = true;
    }
    const std::string_view off_t = signal_detail::trim(add_offset);
    if (!off_t.empty()) {
      const auto r = cli::parse_quantity_value(qty, off_t);
      if (!r.ok()) {
        return fail("invalid add offset '" + std::string(off_t) + "'");
      }
      offset_out = r.value;
      have_offset = true;
    }
    const std::string_view phase_t = signal_detail::trim(add_phase);
    if (!phase_t.empty()) {
      const auto r = cli::parse_double(phase_t);
      if (!r.ok()) {
        return fail("invalid add phase '" + std::string(phase_t) + "'");
      }
      phase_out = r.value;
      have_phase = true;
    }
    const std::string_view duty_t = signal_detail::trim(add_duty);
    if (!duty_t.empty()) {
      const auto r = cli::parse_double(duty_t);
      if (!r.ok()) {
        return fail("invalid add duty '" + std::string(duty_t) + "'");
      }
      duty_out = r.value;
      have_duty = true;
    }
    const std::string_view seed_t = signal_detail::trim(add_seed);
    if (!seed_t.empty()) {
      const auto r = cli::parse_i64(seed_t);
      if (!r.ok() || r.value < 0) {
        return fail("invalid add seed '" + std::string(seed_t) + "'");
      }
      seed_out = static_cast<std::uint64_t>(r.value);
      have_seed = true;
    }
    return true;
  }

  void add_source(SimController &sim) {
    const auto kind =
        static_cast<signal::SignalKind>(static_cast<std::size_t>(add_kind_idx));
    if (kind == signal::SignalKind::kTable || kind == signal::SignalKind::kLua) {
      verb_status = "ERR not_implemented table/lua need bulk payloads (use batch Apply)";
      return;
    }
    const signal::QuantityId qty =
        (add_qty_idx == 1) ? signal::QuantityId::kCurrent
                           : signal::QuantityId::kVoltage;
    double freq_v = 1.0;
    double amp_v = 1.0;
    double offset_v = 0.0;
    double phase_v = 0.0;
    double duty_v = 0.5;
    std::uint64_t seed_v = 1;
    bool have_freq = false;
    bool have_amp = false;
    bool have_offset = false;
    bool have_phase = false;
    bool have_duty = false;
    bool have_seed = false;
    if (!parse_add_fields(qty, freq_v, amp_v, offset_v, phase_v, duty_v, seed_v,
                          have_freq, have_amp, have_offset, have_phase,
                          have_duty, have_seed)) {
      return;
    }
    std::string line =
        "ADD " + std::string(signal::kKindNames[static_cast<std::size_t>(add_kind_idx)]) +
        " " + std::string(signal::quantity_suffix(qty));
    if (have_freq) {
      line += " FREQ " + detail::signal_wire_f64(freq_v);
    }
    if (have_amp) {
      line += " AMP " + detail::signal_wire_f64(amp_v);
    }
    if (have_offset) {
      line += " OFFSET " + detail::signal_wire_f64(offset_v);
    }
    if (have_phase) {
      line += " PHASE " + detail::signal_wire_f64(phase_v);
    }
    if (have_duty) {
      line += " DUTY " + detail::signal_wire_f64(duty_v);
    }
    if (have_seed) {
      line += " SEED " + std::to_string(seed_v);
    }
    if (kind == signal::SignalKind::kSum) {
      const std::string_view ops_t = signal_detail::trim(add_ops);
      if (ops_t.empty()) {
        verb_status = "ERR invalid_argument sum needs OPS serials (e.g. 0,1)";
        return;
      }
      line += " OPS " + std::string(ops_t);
    } else if (!signal_detail::trim(add_ops).empty()) {
      verb_status = "ERR invalid_argument OPS needs kind=sum";
      return;
    }
    if (kind == signal::SignalKind::kExpression) {
      const std::string_view expr_t = signal_detail::trim(add_expr);
      if (expr_t.empty()) {
        verb_status = "ERR invalid_argument expression needs EXPR text";
        return;
      }
      line += " EXPR " + std::string(expr_t);
    } else if (!signal_detail::trim(add_expr).empty()) {
      verb_status = "ERR invalid_argument EXPR needs kind=expression";
      return;
    }
    run_verb(sim, line);
  }

  void set_param(SimController &sim) {
    const auto serial_r = cli::parse_i64(signal_detail::trim(set_serial));
    if (!serial_r.ok() || serial_r.value < 0) {
      verb_status =
          "ERR invalid_argument bad serial '" + std::string(signal_detail::trim(set_serial)) + "'";
      return;
    }
    const std::uint64_t serial = static_cast<std::uint64_t>(serial_r.value);
    static constexpr const char *kNames[7] = {"FREQ", "AMP",   "OFFSET", "PHASE",
                                              "DUTY", "SEED", "QUANTITY"};
    const char *param = kNames[set_param_idx < 0 || set_param_idx >= 7
                                   ? 0
                                   : set_param_idx];
    const std::string_view val_t = signal_detail::trim(set_value);
    if (val_t.empty()) {
      verb_status = "ERR invalid_argument set needs a value";
      return;
    }
    const std::string id = std::to_string(serial);
    const std::string pname(param);
    if (pname == "QUANTITY") {
      if (!signal::parse_quantity(val_t).ok()) {
        verb_status =
            "ERR invalid_argument unknown quantity (want U|I)";
        return;
      }
      run_verb(sim, "SET " + id + " QUANTITY " + std::string(val_t));
      return;
    }
    if (pname == "SEED") {
      const auto r = cli::parse_i64(val_t);
      if (!r.ok() || r.value < 0) {
        verb_status = "ERR invalid_argument bad SEED value";
        return;
      }
      run_verb(sim, "SET " + id + " SEED " + std::to_string(r.value));
      return;
    }
    if (pname == "FREQ") {
      const auto r = cli::parse_frequency(val_t);
      if (!r.ok()) {
        verb_status =
            "ERR invalid_argument bad value '" + std::string(val_t) + "'";
        return;
      }
      run_verb(sim, "SET " + id + " FREQ " + detail::signal_wire_f64(r.value));
      return;
    }
    if (pname == "AMP" || pname == "OFFSET") {
      // Resolve the target's own quantity for the firewall (volts text on
      // a current source fails here, exactly like the CLI).
      const auto slot =
          world::find_serial(sim.state().signals, serial);
      const signal::QuantityId qty =
          slot.has_value()
              ? static_cast<signal::QuantityId>(
                    sim.state().signals.quantity[*slot])
              : signal::QuantityId::kVoltage;
      const auto r = cli::parse_quantity_value(qty, val_t);
      if (!r.ok()) {
        verb_status =
            "ERR invalid_argument bad value '" + std::string(val_t) + "'";
        return;
      }
      run_verb(sim,
               "SET " + id + " " + pname + " " + detail::signal_wire_f64(r.value));
      return;
    }
    const auto r = cli::parse_double(val_t);
    if (!r.ok()) {
      verb_status = "ERR invalid_argument bad value '" + std::string(val_t) + "'";
      return;
    }
    run_verb(sim, "SET " + id + " " + pname + " " + detail::signal_wire_f64(r.value));
  }
};

} // namespace wengine::gui
