#pragma once

// Shared helpers for the logic module verbs.
//
// Src-local (not public API): only TUs in src/cli/commands/logic/ include
// this. The registry surface stays in
// include/wengine/cli/logic_registry.hpp; the process-wide workspace is
// declared there and defined in ../logic.cpp.

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic::support {

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out);
[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept;

// Display names (per-kind counters, never reused): "AND1", "AND1.Y", "AND1.2".
// Sequential gates show named pins: "DFF1.D", "DFF1.CLK", "DFF1.Q", "DFF1.NQ".
[[nodiscard]] std::string gate_name(const wengine::logic::Netlist &nl, std::uint16_t slot);
[[nodiscard]] std::string pin_text(const wengine::logic::Netlist &nl, std::uint16_t slot, bool is_output,
                                   std::uint8_t input, bool negated = false);

void collect_gate_names(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);
// Slot order; per gate the output pins first (Q then NQ on sequential gates),
// then inputs (named on sequential gates, numeric elsewhere).

void collect_gate_names(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);
// Slot order; per gate the output pin first, then inputs 1..k.
void collect_pins(const wengine::logic::Netlist &nl, bool want_out, bool want_in, std::string_view partial,
                  std::vector<std::string> &out);
void collect_block_inputs(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);

// Ordered interface (slot order, then input index) for solve/truth/export.
void interface_pins(const wengine::logic::Netlist &nl, std::vector<wengine::logic::BlockPin> &in_pins,
                    std::vector<wengine::logic::BlockPin> &out_pins);

// Sequential session state (US1, data-model §2/§3/§5): process-wide beside
// workspace(), cleared by `reset`. Sticky poke values keyed by tie-leader
// flat index, per-domain cycle counters, and the clocked flop pool.
wengine::logic::SeqState &seq_state();
std::map<std::size_t, wengine::logic::LogicValue> &poke_map();
std::map<std::string, std::uint64_t> &cycle_counts();
void reset_seq_session_state();

// Clock domains (data-model §1): live flops grouped by CLK/EN driver net
// (named by the source pin, e.g. "BUF1.Y" or "DFF2.NQ"); flops with
// dangling CLK/EN pins form the implicit "clk" domain. First-seen (slot)
// order; members in slot order.
struct ClockDomain {
  std::string name;
  std::vector<std::uint16_t> members;
};
std::vector<ClockDomain> clock_domains(const wengine::logic::Netlist &nl);

// Block-input vector in interface order, composed from the poke map with
// unset pins defaulting to Low (same order solve() consumes), except an
// untied unpoked DFF clock-enable (input 4), which defaults to High
// (FR-008: unconnected enable reads asserted). Tied EN follows its tie
// net; poked EN keeps the poked value; ignored dangling EN reads
// asserted via the engine rule in resolve_control. Stale
// entries (removed/rewired pins) are skipped by construction.
wengine::core::Status compose_block_inputs(const wengine::logic::Netlist &nl, std::vector<wengine::logic::LogicValue> &out);

// Drop poke entries that no longer resolve to a live pokable pin
// (removed gate, rewired input); returns the dropped count so callers can
// report it loudly.
std::size_t reap_stale_pokes(const wengine::logic::Netlist &nl);

// Apply pending async controls (RST/SET) after a control-net mutation:
// composes block inputs, forces Qs via apply_async_controls, and reports
// each forced flop in step-report form (`DFF1.Q: a->b (NQ ...)`); quiet
// when nothing moved. Never touches cycle counters. Returns failure on
// engine refusal (engine contract: state untouched).
wengine::core::Status apply_async_and_report(wengine::logic::Netlist &nl, std::ostream &out);

// Truth streaming over the bit-parallel word evaluator (bit-exact vs solve).
core::Status write_truth_csv(wengine::logic::Netlist &nl, std::uint64_t rows,
                             const std::vector<wengine::logic::BlockPin> &in_pins,
                             const std::vector<wengine::logic::BlockPin> &out_pins, std::ostream &os);

// Pretty terminal table over the same bit-parallel evaluator (same rows,
// same row convention as write_truth_csv; bit-exact vs solve). Column
// widths derive from the pin names; bits are centered. When rows < total
// (a --limit truncation) a loud footer names the shown/total counts.
// --csv files stay raw CSV via write_truth_csv; the terminal never is.
core::Status write_truth_pretty(wengine::logic::Netlist &nl, std::uint64_t rows, std::uint64_t total_rows,
                                const std::vector<wengine::logic::BlockPin> &in_pins,
                                const std::vector<wengine::logic::BlockPin> &out_pins, std::ostream &os);

// Canonical YAML workspace (byte-identical save/load round-trip).
void save_yaml(std::ostream &os, const wengine::logic::Netlist &nl);

[[nodiscard]] std::size_t wire_count(const wengine::logic::Netlist &nl) noexcept;

// Complete a `--from` value: bare pin prefix -> pin names; exact pin (or
// pin + bit prefix) -> valued PIN=0|1 forms.
void complete_from_value(const wengine::logic::Netlist &nl, std::string_view val, std::vector<std::string> &out);

namespace yaml {

// Strict v2 loader (see logic/yaml_store.hpp): `version: 1..2` gate (v1
// loads untied via fallback), all of gates/wires/ties(v2)/meta present,
// unknown keys are errors. Builds into
// `out` (slot order == file order); `out` is untouched on failure.
bool load_text(const std::string &text, wengine::logic::Netlist &out, std::string &err);

} // namespace yaml

// Layered schematic geometry lives in the header-only engine header
// `wengine/logic/layout.hpp` (pure, SDL-free, unit-tested without SDL);
// rasterization (SDL2 window + SDL_SaveBMP) lives in the edge TU
// `src/cli/logic_draw_sdl.cpp` behind `wengine/cli/logic_draw.hpp`.
[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept;

} // namespace wengine::cli::logic::support
