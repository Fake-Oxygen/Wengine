// One-time TUI session setup (no terminal handling).

#include "state.hpp"

#include <filesystem>
#include <system_error>

namespace wengine::tui {

void init_state(State &s) {
  s.pal_entries = palette_entries();

  // Same history file as the REPL (shared format, shared cap). Create the
  // parent dir like `LineReader::ensure_history_loaded` does, so the first
  // TUI run on a fresh $XDG_CACHE_HOME still persists (appends below
  // would otherwise fail silently on a missing directory).
  s.hist_path = cli::LineReader::default_history_path();
  if (!s.hist_path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{s.hist_path}.parent_path(), ec);
    (void)s.history.load(s.hist_path);
  }

  push_text(s.scrollback,
            "WEngine TUI -- same commands as the CLI. Type `help`, press F1 for "
            "context help, F2 for the command palette, F3 for guided forms, "
            "F4 for sim/sig run builders, `exit` to quit.",
            kMaxScrollback);

  s.status_msg = "ready -- F2 for commands, F3 for forms";

  // Guided forms (TUI-3): one argv-composing editor per closed-form family.
  // Values persist per form while the TUI runs; submission goes through the
  // executor so validation/output stay CLI-identical.
  s.form_defs = forms::form_defs();
  s.form_vals.reserve(s.form_defs.size());
  for (const auto &d : s.form_defs) {
    s.form_vals.emplace_back(d.fields.size());
  }
  auto form_set = [&](std::string_view cmd, std::string_view key_or_label, std::string_view value) {
    for (std::size_t fi = 0; fi < s.form_defs.size(); ++fi) {
      if (s.form_defs[fi].command != cmd) {
        continue;
      }
      for (std::size_t i = 0; i < s.form_defs[fi].fields.size(); ++i) {
        const auto &f = s.form_defs[fi].fields[i];
        if (f.key == key_or_label || f.label == key_or_label) {
          s.form_vals[fi][i] = std::string(value);
        }
      }
    }
  };
  form_set("ohm", "--u", "12.4V");
  form_set("ohm", "--i", "0.54A");
  form_set("s0", "mode", "rate");
  form_set("s0", "--power", "2kW");
  form_set("s0", "--const", "1000imp/kWh");
  form_set("i2c", "mode", "pullup");
  form_set("i2c", "--vdd", "3.3V");
  form_set("i2c", "--cb", "200pF");
  form_set("ldo", "--vin", "5V");
  form_set("ldo", "--vout", "3.3V");
  form_set("ldo", "--iout", "500mA");
  form_set("led", "--vs", "5V");
  form_set("led", "--vf", "2V");
  form_set("led", "--if", "20mA");
  form_set("ic", "chip", "ap63205");
  form_set("ic", "op", "inductor");
  form_set("ic", "--vin", "12V");
  s.form_res.resize(s.form_defs.size());
  s.form_has.resize(s.form_defs.size(), false);
  s.form_argv.resize(s.form_defs.size());
  s.form_res_first.resize(s.form_defs.size(), 0);

  // Run builders (TUI-4): one argv-composing editor per batch command
  // (`sim run` / `sig run`). Same contract as the forms above — the CLI
  // owns every verdict — plus `--forever` async routing: a forever line
  // would block until Ctrl-C, so it never reaches the sync executor and
  // instead waits for the TUI-5 async worker.
  s.batch_defs = batches::batch_defs();
  s.batch_vals.reserve(s.batch_defs.size());
  for (const auto &d : s.batch_defs) {
    s.batch_vals.emplace_back(d.fields.size());
  }
  auto batch_set = [&](std::string_view cmd, std::string_view key, std::string_view value) {
    for (std::size_t bi = 0; bi < s.batch_defs.size(); ++bi) {
      if (s.batch_defs[bi].command != cmd) {
        continue;
      }
      for (std::size_t i = 0; i < s.batch_defs[bi].fields.size(); ++i) {
        if (s.batch_defs[bi].fields[i].key == key) {
          s.batch_vals[bi][i] = std::string(value);
        }
      }
    }
  };
  batch_set("sim", "--solver", "projectile");
  batch_set("sim", "--steps", "100");
  batch_set("sim", "--particles", "1");
  batch_set("sig", "--sig-kind", "sine");
  batch_set("sig", "--sig-freq", "1kHz");
  batch_set("sig", "--sig-amp", "3.3V");
  batch_set("sig", "--steps", "1000");
  batch_set("sig", "--dt", "0.1ms");
  s.batch_res.resize(s.batch_defs.size());
  s.batch_has.resize(s.batch_defs.size(), false);
  s.batch_argv.resize(s.batch_defs.size());
  s.batch_res_first.resize(s.batch_defs.size(), 0);
  s.batch_top.resize(s.batch_defs.size(), 0);
}

} // namespace wengine::tui
