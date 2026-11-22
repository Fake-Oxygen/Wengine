#pragma once

/**
 * @file
 * Versioned logic-workspace store (LOGIC-6; 006-versioned-workspace-v1):
 * the persistence edge of the logic pipeline (netlist parse -> state ->
 * eval -> layout/topo -> optimize), pinning the exact YAML bytes a
 * workspace saves and loads.
 *
 * This is the ONLY logic engine header that talks about YAML, and it
 * knows nothing about yaml-cpp: no yaml headers are included here (or
 * in any other engine header), so every existing target keeps linking
 * `wengine_core` alone. The store TUs under `src/logic/`
 * (`wengine_logic_yaml`: orchestration, checkers, replay, save) are
 * the only ones allowed to know yaml-cpp exists (same seam as
 * `src/signal/lua_solver.cpp`).
 *
 * File-format expectations — schema v1, new generation (006): the
 * retired first/second generation (v1/v2 line) is gone — no fallback
 * loader, no silent migration. A `version: 1` file is accepted only
 * with all six sections present; anything else is a loud refusal
 * naming the found version:
 *
 *   version: 1
 *   gates:
 *     - {id: AND1, kind: and, inputs: 2, ignore: [2],
 *        ignore_output: false, delay_ps: 0}
 *   wires:
 *     - {from: AND1.Y, to: NOT1.1}
 *   shared:                       # optional, 011 additive: one row per
 *     - {net: BUS.1}              # marked input, slot order; emitted
 *                                 # iff >= 1 mark exists
 *   pulls:                        # optional, 012 additive: one row per
 *     - {net: BUS.1, pull: up}    # pulled input, slot order; emitted
 *                                 # iff >= 1 pull exists
 *   ties:
 *     - {a: NAND1.1, b: NAND1.2}
 *   clocks:
 *     - {name: BUF1.Y}
 *   meta:
 *     tool: wengine-logic
 *
 * Per-sequential-row `init: 0|1` (default 0, combinational rows must
 * not carry it) persists each flop/latch power-on value; it is read
 * into the load bundle and applied at fresh attach. `clocks` pins one
 * entry per derived clock domain (`derive_clock_domains` vocabulary:
 * source-pin spelling, or bare `clk` for dangling clocks), validated
 * by set-equality against the stored topology — membership itself is
 * never stored. `ties` lists one entry per non-leader tied member
 * (a = class leader, b = member, both dangling inputs), ordered by b
 * in slot order, so the bytes are canonical and deterministic.
 * Unknown keys/sections are loud errors (never silent tolerance).
 * `save` emits one canonical v1 byte layout; `load` accepts the YAML
 * superset (comments, quoting, block vs. flow style, multiline) and
 * re-saving yields the canonical bytes, so save/load/save round-trips
 * byte-identically (a hand-written file missing optional keys
 * stabilizes from the second save).
 *
 * Additive 011 policy (no generation retired, marker stays
 * `version: 1`): the optional `shared:` section lists marked inputs
 * (one `{net: PIN}` row per marked input, slot order; the attachment
 * count derives from the repeated `wires:` rows and is never stored).
 * Absent `shared:` means no marks — pre-011 files load byte-identically.
 * Repeated `to:` wire rows are legal iff the destination is marked;
 * an unmarked duplicate keeps the exact pre-011 refusal. Marks and
 * tied pins are disjoint: tying a marked input fails loudly.
 *
 * Additive 012 policy (no generation retired, marker stays
 * `version: 1`): the optional `pulls:` section lists pulled inputs
 * (one `{net: PIN, pull: up|down}` row per pulled input, slot order;
 * input index breaks ties deterministically). Absent `pulls:` means no
 * pulls — pre-012 files load byte-identically. `none` is never written
 * (absence expresses it); `tbuf` gate rows ride the existing lowercase
 * kind spelling. Duplicate `net:` rows, unknown levels (including
 * `none`), and tied/ignored destinations fail loudly. Canonical save
 * order: version, gates, wires, shared, pulls, ties, clocks, meta.
 *
 * Dependency rules: header-only engine seam; logic is a standalone
 * domain — never depends on CLI types. Consumers: the CLI `save`/
 * `load` logic verbs, `sim run --seq-file`, examples/logic/
 * workspaces, and test_logic_yaml (links wengine_logic_yaml, never
 * wengine_cli).
 */

#include "wengine/logic/netlist.hpp"

#include <array>
#include <string>

namespace wengine::logic::yaml {

/**
 * @brief Pinned workspace version (v1 new generation; retired v1/v2
 *        files are refused loudly, never loaded).
 */
inline constexpr int kWorkspaceVersion = 1;
/** @brief Tool identity written into the `meta:` section on save. */
inline constexpr const char *kToolName = "wengine-logic";

/**
 * @brief Load bundle: gate topology in slot order plus per-slot
 *        power-on values (`LogicValue::kLow` default; meaningful only
 *        where the slot holds a live sequential gate).
 *
 * Netlist itself gains no fields: inits ride beside it, the same
 * pattern as `SeqState` beside `Netlist` at runtime.
 */
struct Workspace {
  /** @brief Gate topology in slot order (file order on load). */
  Netlist netlist;
  /** @brief Per-slot power-on Q for sequential gates; kLow elsewhere (pool-sized). */
  std::vector<LogicValue> init{};
};

/**
 * @brief Canonical save: append the exact v1 bytes to `out`.
 *
 * Per-element inits are taken from `inits` (live pool Q at the
 * session edge; section values at the engine edge). Deterministic:
 * slot order, then input index; lowercase kinds; `true`/`false`
 * spelled out.
 *
 * @param[in] nl Netlist to serialize (read-only).
 * @param[in] inits Per-slot power-on values (indexed like nl slots).
 * @param[out] out Destination string; the v1 document is appended.
 * @note The byte layout is pinned by the save/load/save round-trip
 *       test — any change is a schema event, never incidental.
 */
void save_text(const Netlist &nl, const std::vector<LogicValue> &inits, std::string &out);

/**
 * @brief Strict load: build the workspace from `text` or refuse
 *        loudly with a diagnostic.
 *
 * Builds `out` (slot order == file order) or returns false with a
 * `line N: ...` diagnostic in `err`; `out` is untouched on failure.
 * The loader is atomic at the workspace level: version gate (new v1
 * only), unknown keys/sections, bad inits, clocks/topology mismatch,
 * wire errors, multi-driver inputs, unknown gates, bad pins, and
 * cycle-closing wires all fail loudly.
 *
 * @param[in] text Whole YAML document to parse.
 * @param[out] out Loaded workspace on success; untouched on failure.
 * @param[out] err Human-readable `line N: ...` diagnostic on failure.
 * @return True on success; false with `err` set on any schema,
 *         validation, or topology failure (this edge reports
 *         bool+diagnostics rather than core::Status).
 */
[[nodiscard]] bool load_text(const std::string &text, Workspace &out, std::string &err);

} // namespace wengine::logic::yaml
