#pragma once

// Signal panel model for apps/gui: text-field editing of one batch signal
// source plus run parameters, mirroring the `sig run` CLI validation,
// computation, and config exactly (same parse functions, same
// validate_params, same RunConfig section).
//
// Parsing reuses wengine/cli/args.hpp — header-only machinery over the
// units/scales.hpp suffix tables (the single source of truth) — plus the
// signal::parse_kind/parse_quantity tables. No GUI-side unit tables, no
// wengine_cli link. Empty (or whitespace-only) fields mean "default",
// exactly like a missing CLI option.
//
// Engine-only includes (no ImGui/SDL/Vulkan): unit tests link wengine_core
// alone. File reading stays in the panel (GUI owns file I/O); the model
// takes loaded text: table_csv (a table-format CSV document, parsed with
// signal::parse_table_csv) and lua_script (script text, carried as a
// bounded blob like the CLI). The Lua syntax gate lives in the scripting
// runtime (wengine_signal_lua, never linked here): the model only checks
// presence + size, and the shared builder rejects kLua without a runtime.
// Per-source tuning (add/drop, start/stop) speaks the SIG-10 verb bus on
// the controller, not this batch model. Batch sums stay rejected (sums
// need distinct per-source operands — the SIG-6 family configures those
// directly), mirroring the kSum batch rule.

#include "wengine/cli/args.hpp"
#include "wengine/core/run_config.hpp"
#include "wengine/signal/table.hpp"
#include "wengine/signal/types.hpp"
#include "wengine/solvers/signal.hpp"

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace wengine::gui {

struct SignalBuild {
  bool ok = false;
  core::RunConfig cfg; // solver=signal, particles=0, section set when ok
  std::string error; // reason when !ok (CLI-parity message)
};

namespace signal_detail {

[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  s = cli::trim_front(s);
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace signal_detail

struct SignalModel {
  // Raw text fields; empty means "default" (CLI-missing-option semantics).
  std::string kind = "sine";
  std::string freq = "1";
  std::string amp = "1";
  std::string offset; // default 0
  std::string phase; // default 0
  std::string duty = "0.5";
  std::string seed = "1";
  std::string quantity = "U";
  std::string count = "1";
  std::string expr; // expression source text (kind=expression only)
  std::string table_csv; // table CSV document text (kind=table only)
  std::string lua_script; // Lua script text (kind=lua only)
  std::string dt = "0.01";
  std::string steps = "100";
  std::string emit_every = "1";

  [[nodiscard]] SignalBuild build() const {
    SignalBuild out;
    auto fail = [&](const std::string &msg) {
      out.error = msg;
      return out;
    };
    const std::string_view kind_t = signal_detail::trim(kind);
    const std::string_view qty_t = signal_detail::trim(quantity);
    const auto kind_r =
        signal::parse_kind(kind_t.empty() ? "sine" : kind_t);
    if (!kind_r.ok()) {
      return fail("unknown signal kind (want sine|square|triangle|saw|dc|pulse|noise|sum|"
                  "expression|table)");
    }
    const auto qty_r =
        signal::parse_quantity(qty_t.empty() ? "U" : qty_t);
    if (!qty_r.ok()) {
      return fail("unknown signal quantity (want U|I)");
    }
    signal::SignalParams p;
    p.kind = kind_r.value;
    p.quantity = qty_r.value;

    auto need = [&](const std::string &field, const char *what, const char *dflt,
                    core::Result<double> (*parse)(std::string_view),
                    double &slot) -> bool {
      const std::string_view t = signal_detail::trim(field);
      const auto r = parse(t.empty() ? dflt : t);
      if (!r.ok()) {
        out.error = std::string("invalid ") + what + " '" + std::string(t) + "' (" +
                    std::string(r.error) + ")";
        return false;
      }
      slot = r.value;
      return true;
    };
    // Amp/offset parse against the stamped quantity (the dimensional
    // firewall: "2mA" as volts fails here, exactly like the CLI).
    auto need_qty = [&](const std::string &field, const char *what, const char *dflt,
                        double &slot) -> bool {
      const std::string_view t = signal_detail::trim(field);
      const auto r = cli::parse_quantity_value(
          p.quantity, t.empty() ? std::string_view(dflt) : t);
      if (!r.ok()) {
        out.error = std::string("invalid ") + what + " '" + std::string(t) + "' (" +
                    std::string(r.error) + ")";
        return false;
      }
      slot = r.value;
      return true;
    };
    auto need_i64 = [&](const std::string &field, const char *what, const char *dflt,
                        long long &slot) -> bool {
      const std::string_view t = signal_detail::trim(field);
      const auto r = cli::parse_i64(t.empty() ? std::string_view(dflt) : t);
      if (!r.ok()) {
        out.error = std::string("invalid ") + what + " '" + std::string(t) + "' (" +
                    std::string(r.error) + ")";
        return false;
      }
      slot = r.value;
      return true;
    };
    if (!need(freq, "signal frequency", "1", cli::parse_frequency, p.freq_hz) ||
        !need_qty(amp, "signal amplitude", "1", p.amp) ||
        !need_qty(offset, "signal offset", "0", p.offset) ||
        !need(phase, "signal phase", "0", cli::parse_double, p.phase_rad) ||
        !need(duty, "signal duty", "0.5", cli::parse_double, p.duty)) {
      return out;
    }
    long long seed_v = 1;
    long long count_v = 1;
    long long steps_v = 100;
    long long emit_v = 1;
    double dt_v = 0.01;
    if (!need_i64(seed, "signal seed", "1", seed_v) ||
        !need_i64(count, "signal count", "1", count_v) ||
        !need_i64(steps, "steps", "100", steps_v) ||
        !need_i64(emit_every, "emit stride", "1", emit_v)) {
      return out;
    }
    {
      const std::string_view t = signal_detail::trim(dt);
      const auto r = cli::parse_time(t.empty() ? "0.01" : t);
      if (!r.ok()) {
        return fail("invalid time step '" + std::string(t) + "' (" +
                    std::string(r.error) + ")");
      }
      dt_v = r.value;
    }
    if (p.kind == signal::SignalKind::kSum) {
      return fail("sum needs per-source operands (not supported in batch runs)");
    }
    std::vector<signal::TablePoint> table_points;
    std::string lua_text;
    const bool is_expr = p.kind == signal::SignalKind::kExpression;
    const bool is_table = p.kind == signal::SignalKind::kTable;
    const bool is_lua = p.kind == signal::SignalKind::kLua;
    if (is_table) {
      // Bulk payload rides the batch section (parsed data, no file I/O
      // here — the panel loads the file, like the CLI `sig run` edge).
      if (signal_detail::trim(table_csv).empty()) {
        return fail("table kind needs CSV text (load a file in the panel)");
      }
      std::istringstream csv_in(table_csv);
      std::string table_error;
      if (!signal::parse_table_csv(csv_in, table_points, table_error).ok()) {
        return fail("invalid table CSV (" + table_error + ")");
      }
    } else if (!signal_detail::trim(table_csv).empty()) {
      return fail("table CSV needs kind=table");
    }
    if (is_lua) {
      if (signal_detail::trim(lua_script).empty()) {
        return fail("lua kind needs script text (load a file in the panel)");
      }
      if (lua_script.size() > signal::kMaxLuaScriptBytes) {
        return fail("lua script too large (max 65536 bytes)");
      }
      lua_text = lua_script; // syntax/runtime gates live downstream
    } else if (!signal_detail::trim(lua_script).empty()) {
      return fail("lua script needs kind=lua");
    }
    if (is_expr) {
      const std::string_view t = signal_detail::trim(expr);
      if (t.empty()) {
        return fail("expression kind needs expression text");
      }
      const auto r = signal::parse_expression(t);
      if (!r.ok()) {
        return fail("invalid expression '" + std::string(t) + "' (" + std::string(r.error) +
                    ")");
      }
      p.expr = r.value;
    } else if (!signal_detail::trim(expr).empty()) {
      return fail("expression text needs kind=expression");
    }
    p.seed = static_cast<std::uint64_t>(seed_v < 0 ? 0 : seed_v);
    if (seed_v < 0 || count_v < 0 || steps_v < 1 || !(dt_v > 0.0) || emit_v < 0 ||
        !signal::validate_params(p).ok()) {
      return fail("invalid signal params (freq > 0, duty in 0..1, "
                  "seed/count/stride >= 0, steps >= 1, dt > 0)");
    }
    out.cfg.loop.dt = dt_v;
    out.cfg.loop.emit_every = static_cast<std::uint64_t>(emit_v);
    out.cfg.steps = static_cast<std::uint64_t>(steps_v);
    out.cfg.solver = solvers::SignalSolver::kName;
    out.cfg.particles = 0;
    out.cfg.signal.params = p;
    out.cfg.signal.count = static_cast<std::uint64_t>(count_v);
    out.cfg.signal.table = std::move(table_points);
    out.cfg.signal.lua = std::move(lua_text);
    out.cfg.signal.set = true;
    out.ok = true;
    return out;
  }
};

} // namespace wengine::gui
