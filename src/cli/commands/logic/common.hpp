#pragma once

// Shared helpers for the logic module verbs.
//
// Src-local (not public API): only TUs in src/cli/commands/logic/ include
// this. The registry surface stays in
// include/wengine/cli/logic_registry.hpp; the process-wide workspace is
// declared there and defined in ../logic.cpp.

#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic::support {

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out);
[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept;

// Display names (per-kind counters, never reused): "AND1", "AND1.Y", "AND1.2".
[[nodiscard]] std::string gate_name(const wengine::logic::Netlist &nl, std::uint16_t slot);
[[nodiscard]] std::string pin_text(const wengine::logic::Netlist &nl, std::uint16_t slot, bool is_output, std::uint8_t input);

void collect_gate_names(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);
// Slot order; per gate the output pin first, then inputs 1..k.
void collect_pins(const wengine::logic::Netlist &nl, bool want_out, bool want_in, std::string_view partial,
                  std::vector<std::string> &out);
void collect_block_inputs(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);

// Ordered interface (slot order, then input index) for solve/truth/export.
void interface_pins(const wengine::logic::Netlist &nl, std::vector<wengine::logic::BlockPin> &in_pins,
                    std::vector<wengine::logic::BlockPin> &out_pins);

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

// Strict v1 loader (see logic/yaml_store.hpp): exact `version: 1` gate,
// all of gates/wires/meta present, unknown keys are errors. Builds into
// `out` (slot order == file order); `out` is untouched on failure.
bool load_text(const std::string &text, wengine::logic::Netlist &out, std::string &err);

} // namespace yaml

// Layered schematic (pure layout over the compiled topology).
struct Placed {
  std::uint16_t slot = 0;
  int level = 0;
  int x = 0;
  int y = 0;
};

struct Schematic {
  std::vector<Placed> gates;
  int levels = 0;
  int width = 0;
  int height = 0;
};

Schematic layout_schematic(wengine::logic::Netlist &nl);
void draw_ascii(std::ostream &os, const wengine::logic::Netlist &nl, const Schematic &sc);
bool write_bmp(const std::string &path, const wengine::logic::Netlist &nl, const Schematic &sc, int &w_out, int &h_out, std::string &err);
[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept;

} // namespace wengine::cli::logic::support
