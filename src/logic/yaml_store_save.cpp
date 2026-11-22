// Canonical save for the versioned workspace store.
//
// Emits the exact v1 byte layout in canonical section order (version,
// gates, wires, shared, pulls, ties, clocks, meta). The optional
// per-input sections (`shared:`, `pulls:`) share one generic double
// loop — scan for any marked/pulled input, then emit one row per input
// in slot order — via emit_input_rows.

#include "yaml_store_detail.hpp"

#include "wengine/logic/clock_domains.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wengine::logic::yaml {

namespace {

[[nodiscard]] std::string lower_kind(GateKind k) {
  std::string s(gate_kind_name(k));
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - ('A' - 'a'));
    }
  }
  return s;
}

[[nodiscard]] std::string gate_name(const Netlist &nl, Slot slot) {
  std::string s(gate_kind_name(nl.slots[slot].kind));
  s += std::to_string(nl.slots[slot].counter);
  return s;
}

[[nodiscard]] std::string pin_text(const Netlist &nl, Slot slot, bool is_output, std::uint8_t input, bool negated = false) {
  std::string s = gate_name(nl, slot);
  s += '.';
  const Gate &g = nl.slots[slot];
  if (is_output && is_sequential_kind(g.kind)) {
    s += negated ? "NQ" : "Q";
  } else if (is_output) {
    s += 'Y';
  } else if (is_sequential_kind(g.kind)) {
    // Canonical named inputs: DFF D/CLK (+RST/SET/EN on full-control
    // flops), DLATCH D/EN, JK J/K/CLK, T T/CLK, SR S/R.
    if (g.kind == GateKind::kJk) {
      s += (input == 0) ? "J" : (input == 1) ? "K" : "CLK";
    } else if (g.kind == GateKind::kT) {
      s += (input == 0) ? "T" : "CLK";
    } else if (g.kind == GateKind::kSrlatch) {
      s += (input == 0) ? "S" : "R";
    } else if (input == 0) {
      s += 'D';
    } else if (g.kind == GateKind::kDff && input >= 2) {
      s += (input == 2) ? "RST" : (input == 3) ? "SET" : "EN";
    } else {
      s += (g.kind == GateKind::kDff) ? "CLK" : "EN";
    }
  } else {
    s += std::to_string(static_cast<unsigned>(input) + 1u);
  }
  return s;
}

// One generic double loop for the optional per-input sections: scan in
// slot order for any matching input, emit the header, then emit one row
// per matching input (input index breaks ties deterministically).
// Absent section on read means empty, so no rows means no header.
template <typename HasFn, typename EmitRowFn>
void emit_input_rows(std::string &out, const Netlist &nl, const char *header, HasFn has, EmitRowFn emit_row) {
  bool any = false;
  for (Slot s = 0; s < logic::slot_count(nl) && !any; ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (has(nl, s, i)) {
        any = true;
        break;
      }
    }
  }
  if (!any) {
    return;
  }
  out += header;
  for (Slot s = 0; s < logic::slot_count(nl); ++s) {
    if (!nl.slots[s].live) {
      continue;
    }
    for (std::uint8_t i = 0; i < nl.slots[s].input_count; ++i) {
      if (!has(nl, s, i)) {
        continue;
      }
      emit_row(out, nl, s, i);
    }
  }
}

} // namespace

void save_text(const Netlist &nl, const std::vector<LogicValue> &inits, std::string &out) {
  out += "version: 1\n";
  out += "gates:\n";
  for (Slot s = 0; s < logic::slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    out += "  - {id: ";
    out += gate_name(nl, s);
    out += ", kind: ";
    out += lower_kind(g.kind);
    out += ", inputs: ";
    out += std::to_string(static_cast<unsigned>(g.input_count));
    out += ", ignore: [";
    bool first = true;
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) == 0) {
        continue;
      }
      if (!first) {
        out += ", ";
      }
      first = false;
      out += std::to_string(static_cast<unsigned>(i) + 1u);
    }
    out += "], ignore_output: ";
    out += g.ignore_output ? "true" : "false";
    if (is_sequential_kind(g.kind)) {
      // Sequential-only 7th key (comb rows keep the 6-field canonical
      // shape); the power-on value trails last (see below).
      out += ", ignore_nq: ";
      out += g.ignore_nq_output ? "true" : "false";
    }
    out += ", delay_ps: ";
    out += std::to_string(g.propagation_delay_ps);
    if (is_sequential_kind(g.kind)) {
      // Trailing power-on value from the caller's per-slot inits.
      out += ", init: ";
      out += (inits[s] == LogicValue::kHigh) ? "1" : "0";
    }
    out += "}\n";
  }
  out += "wires:\n";
  for (Slot s = 0; s < logic::slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const Slot drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      out += "  - {from: ";
      const bool neg = nl.driver_inv[tie_flat(s, i)] != 0;
      out += pin_text(nl, drv, true, 0, neg);
      out += ", to: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
      // 011: every extra attachment is a repeated `to:` row, in compact
      // index order (legal on load iff the input is marked, which the
      // `shared:` section below guarantees).
      const std::size_t flat = tie_flat(s, i);
      for_each_extra(nl, flat, [&](std::uint8_t, logic::Slot edrv, std::uint8_t einv) {
        const bool eneg = einv != 0;
        out += "  - {from: ";
        out += pin_text(nl, edrv, true, 0, eneg);
        out += ", to: ";
        out += pin_text(nl, s, false, i);
        out += "}\n";
      });
    }
  }
  // 011: optional `shared:` section, emitted iff >= 1 mark exists, one
  // `{net: PIN}` row per marked input in slot order. Absent on read
  // means no marks (pre-011 files load byte-identically).
  emit_input_rows(
      out, nl, "shared:\n",
      [](const Netlist &n, Slot s, std::uint8_t i) { return is_shared_flat(n, tie_flat(s, i)); },
      [](std::string &o, const Netlist &n, Slot s, std::uint8_t i) {
        o += "  - {net: ";
        o += pin_text(n, s, false, i);
        o += "}\n";
      });
  // 012: optional `pulls:` section, emitted iff >= 1 pull exists, one
  // `{net: PIN, pull: up|down}` row per pulled input in slot order
  // (input index breaks ties deterministically). Absent on read means
  // no pulls (pre-012 files load byte-identically); `none` is absence,
  // never written.
  emit_input_rows(
      out, nl, "pulls:\n",
      [](const Netlist &n, Slot s, std::uint8_t i) { return is_pulled_flat(n, tie_flat(s, i)); },
      [](std::string &o, const Netlist &n, Slot s, std::uint8_t i) {
        const std::uint8_t level = pull_level_flat(n, tie_flat(s, i));
        if (level == 0) {
          return; // has() above guarantees a level; defensive only.
        }
        o += "  - {net: ";
        o += pin_text(n, s, false, i);
        o += ", pull: ";
        o += (level == 1) ? "up" : "down";
        o += "}\n";
      });
  out += "ties:\n";
  // Canonical: one entry per non-leader tied member (a = leader, b = member),
  // ordered by b in slot order so save/load round-trips byte-identically.
  for (Slot s = 0; s < logic::slot_count(nl); ++s) {
    const Gate &g = nl.slots[s];
    if (!g.live) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      if (g.drivers[i] != kNoDriver) {
        continue;
      }
      if ((g.ignored_mask & static_cast<std::uint8_t>(1u << i)) != 0) {
        continue;
      }
      const std::size_t flat = tie_flat(s, i);
      if (!is_tied(nl, s, i)) {
        continue;
      }
      const std::size_t leader = tie_leader_flat(nl, flat);
      if (leader == flat) {
        continue;
      }
      const auto lslot = static_cast<Slot>(leader / kMaxInputs);
      const auto linput = static_cast<std::uint8_t>(leader % kMaxInputs);
      out += "  - {a: ";
      out += pin_text(nl, lslot, false, linput);
      out += ", b: ";
      out += pin_text(nl, s, false, i);
      out += "}\n";
    }
  }
  out += "clocks:\n";
  // Canonical: one entry per derived clock domain (`derive_clock_domains`
  // vocabulary), first-seen slot order, so save/load round-trips
  // byte-identically. Empty for combinational nets.
  for (const ClockDomain &d : derive_clock_domains(nl)) {
    out += "  - {name: ";
    out += d.name;
    out += "}\n";
  }
  out += "meta:\n";
  out += "  tool: ";
  out += kToolName;
  out += "\n";
}

} // namespace wengine::logic::yaml
