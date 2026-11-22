#pragma once

// Shared helpers for the logic module verbs.
//
// Src-local (not public API): only TUs in src/cli/commands/logic/ include
// this. The registry surface stays in
// include/wengine/cli/logic_registry.hpp; the process-wide workspace is
// declared there and defined in ../logic.cpp.

#include "wengine/cli/args.hpp"
#include "wengine/core/status.hpp"
#include "wengine/logic/netlist.hpp"
#include "wengine/logic/seq_state.hpp"
#include "wengine/logic/timing.hpp"
#include "wengine/logic/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wengine::cli::logic::support {

// Verb prelude (one line per site, no cloned wording): every `run`
// starts with handle_help, collects positionals with take_positionals,
// parses pins with parse_input_pin, and every `complete` routes the
// dash-prefix through complete_opts. Z renders through bit_of.
[[nodiscard]] bool handle_help(ArgCursor &args, std::ostream &out, std::string_view name, std::string_view description, std::string_view usage);
[[nodiscard]] bool complete_opts(std::string_view partial, std::span<const std::string_view> opts, std::vector<std::string> &out);
// Collect up to max positionals, then enforce the [min, max] count with
// loud usage/extra/stray errors (false = already reported, caller returns
// kExitUsage). include_options also collects dash-leading tokens as
// positionals (default skips them so options are never eaten): set-delay
// passes true so a negative VALUE reaches parse_time_ps (which refuses it
// as non-negative) instead of vanishing into the flag filter. The
// negative-value grammar lives here, not per verb.
[[nodiscard]] bool take_positionals(ArgCursor &args, std::ostream &out, std::string_view usage, std::vector<std::string_view> &out_pos, std::size_t min,
                                    std::size_t max, std::string_view missing_msg, bool include_options = false);
[[nodiscard]] bool parse_input_pin(const wengine::logic::Netlist &nl, std::string_view text, wengine::logic::PinRef &pin_out, std::ostream &out,
                                    std::string_view usage, std::string_view output_msg);
[[nodiscard]] char bit_of(wengine::logic::LogicValue v) noexcept;

void push_matches(std::string_view partial, std::span<const std::string_view> options, std::vector<std::string> &out);
[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept;

// Display names (per-kind counters, never reused): "AND1", "AND1.Y", "AND1.2".
// Sequential gates show named pins: "DFF1.D", "DFF1.CLK", "DFF1.Q", "DFF1.NQ".
[[nodiscard]] std::string gate_name(const wengine::logic::Netlist &nl, wengine::logic::Slot slot);
[[nodiscard]] std::string pin_text(const wengine::logic::Netlist &nl, wengine::logic::Slot slot, bool is_output, std::uint8_t input, bool negated = false);

void collect_gate_names(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);
// Slot order; per gate the output pins first (Q then NQ on sequential gates),
// then inputs (named on sequential gates, numeric elsewhere).

void collect_gate_names(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);
// Slot order; per gate the output pin first, then inputs 1..k.
void collect_pins(const wengine::logic::Netlist &nl, bool want_out, bool want_in, std::string_view partial,
                  std::vector<std::string> &out);
void collect_block_inputs(const wengine::logic::Netlist &nl, std::string_view partial, std::vector<std::string> &out);

// Ordered interface (slot order, then input index) for solve/truth/export.
void interface_pins(const wengine::logic::Netlist &nl, std::vector<wengine::logic::PinRef> &in_pins,
                    std::vector<wengine::logic::PinRef> &out_pins);

// Sequential session state (US1, data-model §2/§3/§5): process-wide beside
// workspace(), cleared by `reset`. Sticky poke values keyed by tie-leader
// flat index, per-domain cycle counters, and the clocked flop pool.
// (011) Per-net contention counters (SolveDiag): observation-only,
// never serialized, cleared by `reset` with the rest.
wengine::logic::SeqState &seq_state();
std::map<std::size_t, wengine::logic::LogicValue> &poke_map();
std::map<std::string, std::uint64_t> &cycle_counts();
wengine::logic::SolveDiag &solve_diag();
void reset_seq_session_state();

// Clock domains (data-model §1): the engine-owned `derive_clock_domains`
// vocabulary — live flops grouped by CLK driver net (named by the source
// pin, e.g. "BUF1.Y" or "DFF2.NQ"); flops with dangling CLK pins form the
// implicit "clk" domain. First-seen (slot) order; members in slot order.
// Level latches (DLATCH/SRLATCH) belong to no clock domain.
struct ClockDomain {
  std::string name;
  std::vector<wengine::logic::Slot> members;
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

// Apply pending async controls (RST/SET) plus the level-latch settle
// after a control-net mutation: composes block inputs, forces Qs via
// apply_async_controls, settles latches via apply_level_latches, and
// reports each moved flop in step-report form
// (`DFF1.Q: a->b (NQ ...)`); quiet when nothing moved. Never touches
// cycle counters. Returns failure on engine refusal (engine contract:
// state untouched — the pool is snapshotted and restored, so a level
// refusal also rolls back async forces; callers revert their own
// map/topology mutation on top).
wengine::core::Status apply_async_and_report(wengine::logic::Netlist &nl, std::ostream &out);

// Truth streaming over the bit-parallel word evaluator (bit-exact vs solve).
core::Status write_truth_csv(wengine::logic::Netlist &nl, std::uint64_t rows,
                              const std::vector<wengine::logic::PinRef> &in_pins,
                              const std::vector<wengine::logic::PinRef> &out_pins, std::ostream &os,
                              const std::uint64_t *settle_ps = nullptr);

// Pretty terminal table over the same bit-parallel evaluator (same rows,
// same row convention as write_truth_csv; bit-exact vs solve). Column
// widths derive from the pin names; bits are centered. When rows < total
// (a --limit truncation) a loud footer names the shown/total counts.
// --csv files stay raw CSV via write_truth_csv; the terminal never is.
// Both accept an optional per-row settle column (014 `--with-delays`):
// `settle` (pretty) / trailing `settle_ps` (CSV) carrying each row's
// drain time in whole picoseconds. Null (default) keeps the
// zero-delay output byte-identical.
core::Status write_truth_pretty(wengine::logic::Netlist &nl, std::uint64_t rows, std::uint64_t total_rows,
                                 const std::vector<wengine::logic::PinRef> &in_pins,
                                 const std::vector<wengine::logic::PinRef> &out_pins, std::ostream &os,
                                 const std::uint64_t *settle_ps = nullptr);

// Canonical YAML workspace (byte-identical save/load round-trip). The
// saver captures live pool Q as the file's per-element inits (006: save
// captures live state).
void save_yaml(std::ostream &os, const wengine::logic::Netlist &nl);

[[nodiscard]] std::size_t wire_count(const wengine::logic::Netlist &nl) noexcept;

// Contention readback (011): when the workspace holds any shared mark,
// emit `contention: <NET>=<N> [...] (total <T>)` in slot order (marked
// inputs only, zeros included so counting is visibly live); silent when
// no input is marked (mark-free output stays byte-identical).
void report_contention(const wengine::logic::Netlist &nl, const wengine::logic::SolveDiag &diag, std::ostream &out);

// Block-input diff seeds for one timing window (solve/truth share it):
// every High-valued block input seeds its leader pin at fire time 0
// (non-flop, High) — the assignment's diffs against the all-zero
// pre-state. Clears seeds first. Step's flop-force seeding stays
// separate: post-edge Q forces at clock-to-Q are genuinely divergent.
void build_block_seeds(const std::vector<wengine::logic::PinRef> &in_pins, std::span<const wengine::logic::LogicValue> values,
                       std::vector<wengine::logic::TimingSeed> &seeds);

// Shared timing-window refusal text (solve/step/truth name their verb):
// "<verb>: timing window refused (event budget <K> exceeded or picosecond
// time overflow)". Adapters keep their validation roles (they decide when
// a window refused); only the wording lives here.
[[nodiscard]] std::string timing_window_refused(std::string_view verb);

// Timing mode (014): the one flag spelling shared by the solve/step/
// truth verbs, and the one trace printer behind all three windows.
// Grammar (contracts/cli-verbs.md, research D8):
//   timing: unit 1ps, <K> annotated
//     t=<T>ps <PIN>: <old>-><new>
//     ...
//   settled in <D>ps
// Pins reuse the module display spellings (AND2.Y, DFF1.Q, DFF1.NQ,
// --from spellings for block-input nets); values spell 0|1|X|Z.
// Events arrive in pinned order (time, then slot order). The settled
// tail is printed by the unchanged verb paths, never here.
inline constexpr std::string_view kWithDelaysFlag = "--with-delays";
[[nodiscard]] char timing_value_char(wengine::logic::LogicValue v) noexcept;
// Print one window (contracts/cli-verbs.md, research D8): `timing:
// unit 1ps, <K> annotated` (or `timing (<edge>): ...` when edge is
// non-empty — the step verb's per-edge blocks), then `t=<T>ps
// <PIN>: <old>-><new>` lines in pinned order, then `settled in
// <D>ps`.
void print_timing_trace(const wengine::logic::Netlist &nl, const wengine::logic::TimingTrace &trace, std::ostream &out, std::string_view edge = {});

// Complete a `--from` value: bare pin prefix -> pin names; exact pin (or
// pin + bit prefix) -> valued PIN=0|1 forms.
void complete_from_value(const wengine::logic::Netlist &nl, std::string_view val, std::vector<std::string> &out);

namespace yaml {

// Strict v1 loader (see logic/yaml_store.hpp): `version: 1` gate (retired
// generations refused loudly, never loaded), all of
// gates/wires/ties/clocks/meta present, per-sequential-row inits,
// clocks/topology agreement, unknown keys are errors. Builds into `out` (slot order ==
// file order) plus per-slot power-on values into `inits`; both untouched
// on failure.
bool load_text(const std::string &text, wengine::logic::Netlist &out, std::vector<wengine::logic::LogicValue> &inits, std::string &err);

} // namespace yaml

// Layered schematic geometry lives in the header-only engine header
// `wengine/logic/layout.hpp` (pure, SDL-free, unit-tested without SDL);
// rasterization (SDL2 window + SDL_SaveBMP) lives in the edge TU
// `src/cli/logic_draw_sdl.cpp` behind `wengine/cli/logic_draw.hpp`.
[[nodiscard]] bool has_bmp_suffix(std::string_view path) noexcept;

} // namespace wengine::cli::logic::support
