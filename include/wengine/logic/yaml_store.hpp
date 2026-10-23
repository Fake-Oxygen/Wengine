#pragma once

// Versioned logic-workspace store (LOGIC-6).
//
// This is the ONLY logic engine header that talks about YAML, and it knows
// nothing about yaml-cpp: no yaml headers are included here (or in any other
// engine header), so every existing target keeps linking `wengine_core`
// alone. The single TU allowed to know yaml-cpp exists is
// `src/logic/yaml_store.cpp` (same seam as `src/signal/lua_solver.cpp`).
//
// Schema v1 (strict, see docs/logic.md sec. 9):
//   version: 1
//   gates:
//     - {id: AND1, kind: and, inputs: 2, ignore: [2],
//        ignore_output: false, delay_ps: 0}
//   wires:
//     - {from: AND1.Y, to: NOT1.1}
//   meta:
//     tool: wengine-logic
// Unknown keys/sections are loud errors (never silent tolerance); the
// version gate exact-matches with a loud found-version refusal. `save`
// emits one canonical byte layout; `load` accepts the YAML superset
// (comments, quoting, block vs flow style, multiline) and re-saving yields
// the canonical bytes, so save/load round-trips byte-identically.

#include "wengine/logic/netlist.hpp"

#include <string>

namespace wengine::logic::yaml {

// Pinned workspace version (exact-match gate; V2 arrives with a fallback
// loader per docs/logic.md sec. 12, never silent tolerance).
inline constexpr int kWorkspaceVersion = 1;
inline constexpr const char *kToolName = "wengine-logic";

// Canonical save: appends the exact `save`/`export --schema` bytes
// (pinned by the round-trip test). Deterministic: slot order, then input
// index; lowercase kinds; `true`/`false` spelled out.
void save_text(const Netlist &nl, std::string &out);

// Strict load: builds `out` (slot order == file order) or returns false
// with a `line N: ...` diagnostic in `err`. `out` is untouched on failure.
// The loader is atomic at the workspace level: wire errors, multi-driver
// inputs, unknown gates, bad pins, and cycle-closing wires all fail loudly.
[[nodiscard]] bool load_text(const std::string &text, Netlist &out, std::string &err);

} // namespace wengine::logic::yaml
