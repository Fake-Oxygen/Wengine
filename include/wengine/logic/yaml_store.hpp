#pragma once

// Versioned logic-workspace store (LOGIC-6; 006-versioned-workspace-v1).
//
// This is the ONLY logic engine header that talks about YAML, and it knows
// nothing about yaml-cpp: no yaml headers are included here (or in any other
// engine header), so every existing target keeps linking `wengine_core`
// alone. The single TU allowed to know yaml-cpp exists is
// `src/logic/yaml_store.cpp` (same seam as `src/signal/lua_solver.cpp`).
//
// Schema v1, new generation (006): the retired v1/v2 line is gone — no
// fallback loader, no silent migration. A `version: 1` file is accepted
// only with all six sections present; anything else is a loud refusal
// naming the found version:
//
//   version: 1
//   gates:
//     - {id: AND1, kind: and, inputs: 2, ignore: [2],
//        ignore_output: false, delay_ps: 0}
//   wires:
//     - {from: AND1.Y, to: NOT1.1}
//   ties:
//     - {a: NAND1.1, b: NAND1.2}
//   clocks:
//     - {name: BUF1.Y}
//   meta:
//     tool: wengine-logic
//
// Per-sequential-row `init: 0|1` (default 0, combinational rows must not
// carry it) persists each flop/latch power-on value; it is read into the
// load bundle and applied at fresh attach. `clocks` pins one entry per
// derived clock domain (`derive_clock_domains` vocabulary: source-pin
// spelling, or bare `clk` for dangling clocks), validated by set-equality
// against the stored topology — membership itself is never stored.
// `ties` lists one entry per non-leader tied member (a = class leader,
// b = member, both dangling inputs), ordered by b in slot order, so the
// bytes are canonical and deterministic. Unknown keys/sections are loud
// errors (never silent tolerance). `save` emits one canonical v1 byte
// layout; `load` accepts the YAML superset (comments, quoting, block vs
// flow style, multiline) and re-saving yields the canonical bytes, so
// save/load/save round-trips byte-identically (a hand-written file missing
// optional keys stabilizes from the second save).

#include "wengine/logic/netlist.hpp"

#include <array>
#include <string>

namespace wengine::logic::yaml {

// Pinned workspace version (v1 new generation; retired v1/v2 files are
// refused loudly, never loaded).
inline constexpr int kWorkspaceVersion = 1;
inline constexpr const char *kToolName = "wengine-logic";

// Load bundle: gate topology in slot order plus per-slot power-on values
// (`LogicValue::kLow` default; meaningful only where the slot holds a live
// sequential gate). Netlist itself gains no fields: inits ride beside it,
// the same pattern as `SeqState` beside `Netlist` at runtime.
struct Workspace {
  Netlist netlist;
  std::array<LogicValue, kMaxGates> init{};
};

// Canonical save: appends the exact v1 bytes (pinned by the round-trip
// test), with per-element inits taken from `inits` (live pool Q at the
// session edge; section values at the engine edge). Deterministic: slot
// order, then input index; lowercase kinds; `true`/`false` spelled out.
void save_text(const Netlist &nl, const std::array<LogicValue, kMaxGates> &inits, std::string &out);

// Strict load: builds `out` (slot order == file order) or returns false
// with a `line N: ...` diagnostic in `err`. `out` is untouched on failure.
// The loader is atomic at the workspace level: version gate (new v1 only),
// unknown keys/sections, bad inits, clocks/topology mismatch, wire errors,
// multi-driver inputs, unknown gates, bad pins, and cycle-closing wires
// all fail loudly.
[[nodiscard]] bool load_text(const std::string &text, Workspace &out, std::string &err);

} // namespace wengine::logic::yaml
