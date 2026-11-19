#pragma once

/**
 * @file
 * Shared sequential symbol model (008-seq-schematic-symbols, contract
 * C5): the draw stage of the logic pipeline (netlist parse -> state ->
 * eval -> layout/topo -> optimize).
 *
 * One deep Module behind a small Interface: both raster Adapters (SDL +
 * ImGui) paint from SeqSym and never recompute symbol geometry. Pure
 * layout-unit ints, header-only, no SDL/ImGui types — unit-testable
 * linking wengine_core alone. Raster mapping (halo strokes, text
 * rasterization, pixel offsets for wedge/notch/bubble shapes) stays
 * in the TUs per contract C7; every integer decision lives here.
 *
 * Dependency rules: depends only on layout.hpp geometry + types.hpp
 * kinds. Logic is a standalone domain — never depends on CLI types.
 * Consumers: src/cli/logic_draw_sdl.cpp, src/cli/logic_draw_imgui.cpp,
 * and test_logic_seq_symbol.
 */

#include "wengine/logic/layout.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::logic {

/**
 * @brief Body insets shared with combinational bodies so stubs land
 *        identically: lead-in from the box's left edge (10 units).
 */
inline constexpr int kSeqLead = 10;
/**
 * @brief Body insets shared with combinational bodies so stubs land
 *        identically: vertical padding inside the box (4 units).
 */
inline constexpr int kSeqPad = 4;

/**
 * @brief Clock/mark decoration drawn at a sequential gate's special
 *        pin: kNone (0) for none, kWedge (1) for the clock wedge on
 *        edge-flop CLK pins (shared row 2 of 6ths), kNotch (2) for the
 *        level-latch notch on the DLATCH enable pin.
 */
enum class SeqMark : std::uint8_t { kNone = 0, kWedge = 1, kNotch = 2 };

/**
 * @brief Geometry of one sequential gate's schematic symbol, in layout
 *        units: rectangle body, per-input stub rows, face letter, the
 *        clock wedge/latch-notch mark, and Q/NQ output anchors.
 *
 * Both raster backends paint from this struct and never recompute
 * geometry (contract C5).
 */
struct SeqSym {
  /** @brief Body left edge (box.x + kSeqLead), layout units. */
  int lx = 0;
  /** @brief Body top edge (box.y + kSeqPad), layout units. */
  int top = 0;
  /** @brief Body right edge, layout units. */
  int rx = 0;
  /** @brief Body bottom edge, layout units. */
  int bot = 0;
  /** @brief Body vertical middle, layout units. */
  int mid = 0;
  /** @brief Centered face letter ("D", "JK", "T", "SR", "DL"). */
  std::string_view face{};
  /** @brief Clock wedge / latch notch decoration kind. */
  SeqMark mark = SeqMark::kNone;
  /** @brief Y of the marked pin's stub row, layout units. */
  int mark_y = 0;
  /** @brief True when the NQ output is shown (used or un-ignored). */
  bool nq = false;
  /** @brief Number of input stubs (gate fan-in). */
  std::uint8_t stub_n = 0;
  /** @brief Per-input stub row Y, layout units (role-based anchors). */
  int stub_y[kMaxInputs]{};
  /** @brief Q output anchor, layout units. */
  WirePt q{};
  /** @brief NQ output anchor, layout units. */
  WirePt nq_pt{};
  /** @brief Fanout junction dot on Q (fanout greater than one). */
  bool q_dot = false;
  /** @brief Fanout junction dot on NQ (fanout greater than one). */
  bool nq_dot = false;
};

/**
 * @brief Derive the symbol geometry for one placed sequential gate.
 *
 * Pin anchors are role-based (layout.hpp kind table): every clock
 * wedge sits at the same relative height, so shared clocks trace
 * horizontally; full-control DFF flops stack D/CLK/RST/SET/EN top to
 * bottom.
 *
 * @param[in] box Placed body rectangle (layout units) for the gate.
 * @param[in] g The gate slot (kind, fan-in, NQ usage/ignore flags).
 * @return Fully populated symbol geometry (pure function of inputs).
 * @note NQ visibility rule: shown when the flop has NQ fanout or NQ is
 *       not ignored; junction dots appear only above fanout one.
 */

[[nodiscard]] constexpr SeqSym seq_symbol(const Placed &box, const Gate &g) noexcept {
  SeqSym s;
  s.lx = box.x + kSeqLead;
  s.top = box.y + kSeqPad;
  s.rx = box.x + kLayoutBoxW - 2;
  s.bot = box.y + kLayoutBoxH - kSeqPad;
  s.mid = box.y + kLayoutBoxH / 2;
  s.face = seq_face(g.kind);
  s.stub_n = g.input_count;
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    s.stub_y[i] = seq_input_anchor(box, g.kind, i).y;
    if (s.mark == SeqMark::kNone) {
      if (is_clock_pin(g.kind, i)) {
        s.mark = SeqMark::kWedge;
        s.mark_y = s.stub_y[i];
      } else if (is_latch_enable_pin(g.kind, i)) {
        s.mark = SeqMark::kNotch;
        s.mark_y = s.stub_y[i];
      }
    }
  }
  s.nq = (g.nq_fanout > 0) || (!g.ignore_nq_output);
  s.q = seq_q_anchor(box);
  s.nq_pt = seq_nq_anchor(box);
  s.q_dot = g.output_fanout > 1;
  s.nq_dot = g.nq_fanout > 1;
  return s;
}

} // namespace wengine::logic
