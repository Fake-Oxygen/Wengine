#pragma once

// Run builders for `sim run` / `sig run` batch runs (TUI-4).
//
// The TUI must never reimplement parsing or validation (D-TUI-1): every
// builder only *composes an argv string* which is submitted through
// `tui::TuiExecutor` (i.e. `cli::Shell::execute`). The CLI owns all
// validation, so invalid input surfaces the CLI usage-error text verbatim.
//
// This header is ncurses-free and unit-testable (see
// `tests/test_tui_batches.cpp`). The ncurses panes in `src/tui/` own
// only field editing + result display + the --forever async routing.
//
// Model:
// - `BatchDef::command` is "sim"/"sig"; the mode is pinned to "run" (batch
//   runs only — the sig per-source family stays on the typed line).
// - `BatchField::key` is the CLI flag (e.g. "--steps"); `flag == true`
//   marks a bare flag (e.g. "--paced"): any enabled value emits the bare
//   key, disabled/empty omits it. Non-flag fields emit `--key <value>`.
// - Solver/kind pickers derive from the registries: `solver_names()` from
//   `SolverRegistry`, `signal_kind_names()` from `signal::kKindNames` —
//   new solvers/kinds appear with no builder-side change.
// - `--forever` never runs synchronously: `wants_async()` reports it so
//   the ncurses loop can route the composed line to the async worker
//   (TUI-5) instead of blocking the builder in `execute_line`.

#include "wengine/cli/commands.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/solver_registry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wengine::tui::batches {

enum class BatchKind : int { kSim = 0, kSig };

inline constexpr int kBatchCount = 2;

// One editable builder field. `key` is the CLI flag; `flag` marks a bare
// flag (no value). `label` is the short row name, `hint` the unit/example.
struct BatchField {
  std::string_view key;
  std::string_view label;
  std::string_view hint;
  bool flag = false;
};

struct BatchDef {
  BatchKind kind{};
  std::string_view command; // "sim" / "sig"
  std::string_view mode;    // always "run"
  std::string_view description;
  std::string_view usage;
  std::vector<BatchField> fields;
};

// Registry-derived solver names for the `sim` builder (`SolverRegistry`
// order, never hardcoded).
[[nodiscard]] inline std::vector<std::string> solver_names() {
  std::vector<std::string> out;
  for (const auto &d : solvers::SolverRegistry::all()) {
    out.emplace_back(d.name);
  }
  return out;
}

// Registry-derived signal kinds for the `sig` builder (`kKindNames`
// order, never hardcoded).
[[nodiscard]] inline std::vector<std::string> signal_kind_names() {
  std::vector<std::string> out;
  for (std::string_view k : signal::kKindNames) {
    out.emplace_back(k);
  }
  return out;
}

// Quantity spellings for `--sig-quantity` (single-table order U, I).
[[nodiscard]] inline std::vector<std::string> quantity_names() {
  return {"U", "I"};
}

// UART wire modes for `--emit-uart-mode`.
[[nodiscard]] inline std::vector<std::string> uart_modes() {
  return {"ascii", "cobs"};
}

[[nodiscard]] inline std::vector<BatchDef> batch_defs() {
  using cli::commands::SigSpec;
  using cli::commands::SimSpec;
  std::vector<BatchDef> defs;
  defs.push_back({
      BatchKind::kSim,
      SimSpec::kName,
      "run",
      SimSpec::kDescription,
      SimSpec::kUsage,
      {
          {"--solver", "solver", "registry: projectile|signal"},
          {"--steps", "steps", "e.g. 100"},
          {"--dt", "dt", "e.g. 0.01s / 10ms"},
          {"--gravity", "gravity", "m/s^2, projectile only"},
          {"--particles", "particles", "e.g. 1"},
          {"--x0", "x0", "e.g. 0"},
          {"--y0", "y0", "e.g. 0"},
          {"--vx0", "vx0", "e.g. 10"},
          {"--vy0", "vy0", "e.g. 10"},
          {"--emit-every", "emit-every", "global gate, e.g. 1"},
          {"--emit-csv", "csv file", "path, e.g. out.csv"},
          {"--emit-csv-stride", "csv stride", "0=muted, N>=1"},
          {"--emit-csv-select", "csv select", "serials e.g. 0 (signal only)"},
          {"--emit-shm", "shm seg", "e.g. /wengine-sim"},
          {"--emit-shm-stride", "shm stride", "0=muted, N>=1"},
          {"--emit-shm-select", "shm select", "serials e.g. 0 (signal only)"},
          {"--emit-tcp", "tcp out", "host:port"},
          {"--emit-tcp-stride", "tcp stride", "0=muted, N>=1"},
          {"--emit-tcp-select", "tcp select", "serials e.g. 0 (signal only)"},
          {"--emit-udp", "udp out", "host:port"},
          {"--emit-udp-stride", "udp stride", "0=muted, N>=1"},
          {"--emit-udp-select", "udp select", "serials e.g. 0 (signal only)"},
          {"--emit-udp-budget", "udp budget", "bytes 1..65507"},
          {"--emit-uart", "uart dev", "device[,baud] (signal only)"},
          {"--emit-uart-mode", "uart mode", "ascii|cobs"},
          {"--emit-uart-stride", "uart stride", "0=muted, N>=1"},
          {"--emit-uart-select", "uart select", "serials e.g. 0 (signal only)"},
          {"--paced", "paced", "flag: 1 to enable wall-clock pacing", true},
          {"--rate", "rate", "wall steps/s, e.g. 1kHz (needs --paced)"},
          {"--emit-health", "health", "flag: 1 for live-output report", true},
      },
  });
  defs.push_back({
      BatchKind::kSig,
      SigSpec::kName,
      "run",
      SigSpec::kDescription,
      SigSpec::kUsage,
      {
          {"--sig-kind", "kind", "sine|square|triangle|saw|dc|pulse|noise|sum|expression|table|lua"},
          {"--sig-freq", "freq", "e.g. 1kHz (periodic kinds)"},
          {"--sig-amp", "amp", "e.g. 3.3V / 20mA (per quantity)"},
          {"--sig-offset", "offset", "e.g. 0V"},
          {"--sig-phase", "phase", "radians, e.g. 0"},
          {"--sig-duty", "duty", "0..1 (square/pulse)"},
          {"--sig-seed", "seed", "e.g. 1 (noise)"},
          {"--sig-quantity", "quantity", "U|I"},
          {"--sig-count", "count", "identical sources, e.g. 1"},
          {"--sig-expr", "expr", "math over t, e.g. sin(2*pi*440*t)*0.5"},
          {"--sig-table", "table file", "CSV waveform (time,value)"},
          {"--sig-lua", "lua file", "script path (pure fn of t/seed/step/dt)"},
          {"--steps", "steps", "e.g. 1000 (ignored with --forever)"},
          {"--dt", "dt", "e.g. 0.1ms"},
          {"--emit-every", "emit-every", "global gate, e.g. 1"},
          {"--emit-csv", "csv file", "path, e.g. sig.csv"},
          {"--emit-csv-stride", "csv stride", "0=muted, N>=1"},
          {"--emit-csv-select", "csv select", "serials e.g. 0,2"},
          {"--emit-shm", "shm seg", "e.g. /wengine-sig"},
          {"--emit-shm-stride", "shm stride", "0=muted, N>=1"},
          {"--emit-shm-select", "shm select", "serials e.g. 0,2"},
          {"--emit-tcp", "tcp out", "host:port"},
          {"--emit-tcp-stride", "tcp stride", "0=muted, N>=1"},
          {"--emit-tcp-select", "tcp select", "serials e.g. 0,2"},
          {"--emit-udp", "udp out", "host:port"},
          {"--emit-udp-stride", "udp stride", "0=muted, N>=1"},
          {"--emit-udp-select", "udp select", "serials e.g. 0,2"},
          {"--emit-udp-budget", "udp budget", "bytes 1..65507"},
          {"--emit-uart", "uart dev", "device[,baud]"},
          {"--emit-uart-mode", "uart mode", "ascii|cobs"},
          {"--emit-uart-stride", "uart stride", "0=muted, N>=1"},
          {"--emit-uart-select", "uart select", "serials e.g. 0,2"},
          {"--control-tcp", "control", "host:port (step-sync server)"},
          {"--control-uart", "ctl-uart", "flag: 1 (needs --emit-uart)", true},
          {"--paced", "paced", "flag: 1 to enable wall-clock pacing", true},
          {"--rate", "rate", "wall steps/s, e.g. 1kHz (needs --paced)"},
          {"--forever", "forever", "flag: 1 runs until Ctrl-C (async, TUI-5)", true},
          {"--emit-health", "health", "flag: 1 for live-output report", true},
      },
  });
  return defs;
}

namespace detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  std::size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
    --e;
  }
  return s.substr(b, e - b);
}

// Quote one argv token when it carries whitespace/quotes so the CLI
// tokenizer keeps it a single argument (mirrors forms::compose_line:
// file paths with spaces need this; unit values never do).
[[nodiscard]] inline std::string quote_token(std::string_view t) {
  const bool need = t.find_first_of(" \t'\"") != std::string_view::npos;
  if (!need) {
    return std::string(t);
  }
  if (t.find('\'') == std::string_view::npos) {
    return "'" + std::string(t) + "'";
  }
  if (t.find('"') == std::string_view::npos) {
    return "\"" + std::string(t) + "\"";
  }
  return std::string(t);
}

// Bare-flag truth table: empty/0/false/no/off/n (case-insensitive first
// letter suffices for the documented spellings) disables; anything else
// enables. Never validated beyond this — the CLI owns every verdict.
[[nodiscard]] inline bool flag_enabled(std::string_view v) noexcept {
  if (v.empty()) {
    return false;
  }
  std::string low;
  low.reserve(v.size());
  for (char c : v) {
    low.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
  }
  return !(low == "0" || low == "false" || low == "no" || low == "off" || low == "n" || low == "f");
}

} // namespace detail

// True when the builder values request a bare flag field.
[[nodiscard]] inline bool field_flag_set(const BatchDef &def, const std::vector<std::string> &values, std::string_view key) {
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    if (def.fields[i].key == key && def.fields[i].flag) {
      return detail::flag_enabled(detail::trim(values[i]));
    }
  }
  return false;
}

// `--forever` must never run synchronously in the TUI: it runs until
// Ctrl-C, which would block the builder loop exactly like a long batch
// blocks the REPL today. The ncurses layer routes such lines to the
// async worker (TUI-5) and never calls `execute_line` with them.
[[nodiscard]] inline bool wants_async(const BatchDef &def, const std::vector<std::string> &values) {
  return field_flag_set(def, values, "--forever");
}

// Compose the CLI line for `def` with `values[i]` in each field.
// Empty-after-trim fields are omitted (missing option); bare flags emit
// the key only when enabled; nothing is validated here — the executor
// (CLI) reports all errors.
[[nodiscard]] inline std::string compose_line(const BatchDef &def, const std::vector<std::string> &values) {
  std::string line(def.command);
  line.push_back(' ');
  line += def.mode;
  for (std::size_t i = 0; i < def.fields.size() && i < values.size(); ++i) {
    const std::string_view v = detail::trim(values[i]);
    if (v.empty()) {
      continue;
    }
    if (def.fields[i].flag) {
      if (detail::flag_enabled(v)) {
        line.push_back(' ');
        line += def.fields[i].key;
      }
      continue;
    }
    line.push_back(' ');
    line += def.fields[i].key;
    line.push_back(' ');
    line += detail::quote_token(v);
  }
  return line;
}

} // namespace wengine::tui::batches
