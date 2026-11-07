#pragma once

// Shared sequential symbol model (008-seq-schematic-symbols, contract C5).
//
// One deep Module behind a small Interface: both raster Adapters (SDL +
// ImGui) paint from SeqSym and never recompute symbol geometry. Pure
// layout-unit ints, header-only, no SDL/ImGui types — unit-testable
// linking wengine_core alone. Raster mapping (halo strokes, text
// rasterization, pixel offsets for wedge/notch/bubble shapes) stays in
// the TUs per contract C7; every integer decision lives here.

#include "wengine/logic/layout.hpp"

#include <cstdint>
#include <string_view>

namespace wengine::logic {

// Body insets shared with combinational bodies so stubs land identically.
inline constexpr int kSeqLead = 10;
inline constexpr int kSeqPad = 4;

enum class SeqMark : std::uint8_t { kNone = 0, kWedge = 1, kNotch = 2 };

struct SeqSym {
  int lx = 0;
  int top = 0;
  int rx = 0;
  int bot = 0;
  int mid = 0;
  std::string_view face{};
  SeqMark mark = SeqMark::kNone;
  int mark_y = 0;
  bool nq = false;
  std::uint8_t stub_n = 0;
  int stub_y[kMaxInputs]{};
  WirePt q{};
  WirePt nq_pt{};
  bool q_dot = false;
  bool nq_dot = false;
};

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
