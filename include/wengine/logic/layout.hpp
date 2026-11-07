#pragma once

// Layered schematic layout (LOGIC-7): pure geometry over the compiled
// topology. Header-only, SDL-free, allocation happens only here at
// draw time (never on a solver hot path).
//
// Determinism: slot-order scans everywhere; column = topo_level
// (inputs-only gates sit at level 0 on the left, sinks at the max level
// on the right); within a level gates stack in slot order. Wire routes
// are Manhattan doglegs through the mid column between the boxes:
// (x0,y0) -> (xm,y0) -> (xm,y1) -> (x1,y1).
//
// Precondition: the caller compiles the topology first
// (`topo::rebuild`; draw/export verbs do). On a topology error the
// result is an empty schematic (levels = 0); the verbs report the error
// themselves before calling here.

#include "wengine/logic/pins.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::logic {

// Box + grid metrics shared with the SDL raster TU (single source of
// truth so layout goldens and pixels cannot drift apart). Boxes are
// datasheet-proportioned (near-square distinctive shapes, not wide bars).
inline constexpr int kLayoutColW = 120;
inline constexpr int kLayoutBoxW = 76;
inline constexpr int kLayoutBoxH = 56;
inline constexpr int kLayoutGapY = 30;
inline constexpr int kLayoutMargin = 30;

struct Placed {
  std::uint16_t slot = 0;
  int level = 0;
  int x = 0;
  int y = 0;
};

struct WirePt {
  int x = 0;
  int y = 0;
};

struct WireRoute {
  std::uint16_t from_slot = kNoDriver;
  std::uint16_t to_slot = kNoDriver;
  std::uint8_t to_input = 0;
  WirePt p[4] = {};
};

struct Schematic {
  std::vector<Placed> gates;     // slot order
  std::vector<WireRoute> wires;  // destination slot order, then input index
  int levels = 0;
  int width = 0;
  int height = 0;
};

// Sequential pin anchors (008-seq-schematic-symbols, contract C2/C3).
// Single owner: the pins.hpp KindPin table (anchor_row/is_clock/is_notch
// per pin, face per kind). The queries below are thin constexpr Adapters
// over that table so layout, parse, and raster never drift apart.
// Roles (row numerators, denominator kSeqAnchorDenom):
// DFF: D0->1 CLK1->2 RST2->3 SET3->4 EN4->5; DLATCH: D0->1 EN1->2;
// JK: J0->1 K1->4 CLK2->2; T: T0->1 CLK1->2; SRLATCH: S0->1 R1->4.
// Returns the row numerator (denominator kSeqAnchorDenom), or -1 for
// non-sequential kinds.
inline constexpr int kSeqAnchorDenom = 6;

[[nodiscard]] constexpr int seq_input_row(GateKind kind, std::uint8_t input) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind != kind) {
      continue;
    }
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      if (e.pins[p].input == input) {
        return e.pins[p].anchor_row;
      }
    }
    return -1;
  }
  return -1;
}

// Clock-wedge pins (edge-triggered flops) and the latch-enable notch
// pin (level-sensitive DLATCH). SRLATCH has neither mark.
[[nodiscard]] constexpr bool is_clock_pin(GateKind kind, std::uint8_t input) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind != kind) {
      continue;
    }
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      if (e.pins[p].input == input) {
        return e.pins[p].is_clock;
      }
    }
    return false;
  }
  return false;
}

[[nodiscard]] constexpr bool is_latch_enable_pin(GateKind kind, std::uint8_t input) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind != kind) {
      continue;
    }
    for (std::uint8_t p = 0; p < e.pin_count; ++p) {
      if (e.pins[p].input == input) {
        return e.pins[p].is_notch;
      }
    }
    return false;
  }
  return false;
}

// Face letters painted on sequential bodies (contract C5).
[[nodiscard]] constexpr std::string_view seq_face(GateKind kind) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind == kind) {
      return e.face.empty() ? std::string_view{"?"} : e.face;
    }
  }
  return "?";
}

// Anchor points for one placed box. Input anchors sit on the left edge
// at the role row; Q keeps the mid convention; NQ sits at row 5 with
// the inversion bubble (contract C2/C3).
[[nodiscard]] constexpr WirePt seq_input_anchor(const Placed &box, GateKind kind, std::uint8_t input) noexcept {
  const int row = seq_input_row(kind, input);
  const int y = (row < 0) ? (box.y + kLayoutBoxH / 2) : (box.y + row * kLayoutBoxH / kSeqAnchorDenom);
  return WirePt{box.x, y};
}

[[nodiscard]] constexpr WirePt seq_q_anchor(const Placed &box) noexcept {
  return WirePt{box.x + kLayoutBoxW, box.y + kLayoutBoxH / 2};
}

[[nodiscard]] constexpr WirePt seq_nq_anchor(const Placed &box) noexcept {
  return WirePt{box.x + kLayoutBoxW, box.y + 5 * kLayoutBoxH / kSeqAnchorDenom};
}

// True when the sink input reads the negated (NQ) source value.
// Index mirrors tie_flat(slot, input); only wire_pins() from an NQ
// source ever sets the flag (see types.hpp Gate::driver_inv).
[[nodiscard]] inline bool sink_reads_nq(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  const std::size_t flat = static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input);
  return nl.driver_inv[flat] != 0;
}

// Wire-lane stagger (follow-up: coincident vertical trunks). Wires
// spanning the same column gap share the midpoint (x0+x1)/2, so two
// unrelated nets collapse into one unreadable trunk. Each wire takes a
// deterministic lane by its ordinal among same-gap wires (slot-order
// enumeration, so repeat layouts are identical): lane 0 stays on the
// midpoint, later lanes alternate sides in 8-unit steps. Same-driver
// branches still meet exactly at the source anchor (where the fanout
// dot marks the connection); unrelated nets never share a vertical.
inline constexpr int kWireLaneStep = 8;

[[nodiscard]] constexpr int wire_lane_offset(std::size_t lane) noexcept {
  const int half = static_cast<int>((lane + 1u) / 2u);
  const int mag = half * kWireLaneStep;
  return ((lane & 1u) != 0u) ? mag : -mag; // 0, +8, -8, +16, -16, ...
}

namespace detail {
// Internal Seams for layout_schematic (never public): placement, canvas,
// anchors, and lane ownership. The public Interface stays
// `Netlist& -> Schematic`.

inline void place_gates(Netlist &nl, Schematic &sc, std::vector<int> &per_level) {
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (!nl.gates[s].live) {
      continue;
    }
    const int lvl = nl.topo_level[s];
    const int idx = per_level[static_cast<std::size_t>(lvl)]++;
    Placed p;
    p.slot = s;
    p.level = lvl;
    p.x = kLayoutMargin + lvl * kLayoutColW + (kLayoutColW - kLayoutBoxW) / 2;
    p.y = kLayoutMargin + idx * (kLayoutBoxH + kLayoutGapY);
    sc.gates.push_back(p);
  }
}

inline void canvas_extents(const std::vector<int> &per_level, int levels, int &w, int &h) {
  int max_count = 1;
  for (int c : per_level) {
    if (c > max_count) {
      max_count = c;
    }
  }
  w = kLayoutMargin * 2 + levels * kLayoutColW;
  h = kLayoutMargin * 2 + max_count * (kLayoutBoxH + kLayoutGapY);
}

// Sequential endpoints use role anchors (contract C4); combinational
// endpoints keep the legacy spread so existing goldens hold.
// Sources: NQ-reading sinks start at the NQ anchor, everything else
// at the Q anchor (the mid convention, unchanged for combinational
// sources since driver_inv is only ever set from NQ sources).
inline int source_y(const Netlist &nl, const Placed &src, std::uint16_t dst_slot, std::uint8_t input) {
  if (is_sequential_kind(nl.gates[src.slot].kind) && sink_reads_nq(nl, dst_slot, input)) {
    return seq_nq_anchor(src).y;
  }
  return seq_q_anchor(src).y;
}

inline int sink_y(const Placed &dst, const Gate &g, std::uint8_t input) {
  if (seq_input_row(g.kind, input) >= 0) {
    return seq_input_anchor(dst, g.kind, input).y;
  }
  const int n = g.input_count;
  return dst.y + (static_cast<int>(input) + 1) * kLayoutBoxH / (n + 1);
}

// Per-gap lane ownership: (src_level, dst_level) -> wires seen so far.
// Linear scan over distinct gaps (a handful in practice); the wire loop
// enumerates in destination slot order, so lanes are deterministic.
struct Lanes {
  std::vector<std::pair<std::pair<int, int>, std::size_t>> use;
  std::size_t next(const std::pair<int, int> &gap) {
    for (std::pair<std::pair<int, int>, std::size_t> &e : use) {
      if (e.first == gap) {
        return e.second++;
      }
    }
    use.emplace_back(gap, 1);
    return 0;
  }
};
} // namespace detail

[[nodiscard]] inline Schematic layout_schematic(Netlist &nl) {
  Schematic sc;
  if (nl.live_count == 0) {
    sc.width = 2 * kLayoutMargin;
    sc.height = 2 * kLayoutMargin;
    return sc;
  }
  if (!topo::rebuild(nl, EdgeFilter::kAll).ok()) {
    return sc; // levels = 0, empty: the verb already reported the error
  }
  int max_level = 0;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    if (nl.gates[s].live && nl.topo_level[s] > max_level) {
      max_level = nl.topo_level[s];
    }
  }
  sc.levels = max_level + 1;
  std::vector<int> per_level(static_cast<std::size_t>(sc.levels), 0);
  detail::place_gates(nl, sc, per_level);
  detail::canvas_extents(per_level, sc.levels, sc.width, sc.height);

  // Slot -> Placed lookup for anchor math (live slots only).
  const Placed *by_slot[kMaxGates] = {};
  for (const Placed &p : sc.gates) {
    by_slot[p.slot] = &p;
  }
  detail::Lanes lanes;
  for (std::uint16_t s = 0; s < kMaxGates; ++s) {
    const Gate &g = nl.gates[s];
    if (!g.live) {
      continue;
    }
    const Placed *dst = by_slot[s];
    if (dst == nullptr) {
      continue;
    }
    for (std::uint8_t i = 0; i < g.input_count; ++i) {
      const std::uint16_t drv = g.drivers[i];
      if (drv == kNoDriver) {
        continue;
      }
      const Placed *src = (drv < kMaxGates) ? by_slot[drv] : nullptr;
      if (src == nullptr) {
        continue;
      }
      const int y0 = detail::source_y(nl, *src, s, i);
      const int y1 = detail::sink_y(*dst, g, i);
      const int x0 = src->x + kLayoutBoxW;
      const int x1 = dst->x;
      const std::size_t lane = lanes.next(std::pair<int, int>{src->level, dst->level});
      const int xm = (x0 + x1) / 2 + wire_lane_offset(lane);
      WireRoute r;
      r.from_slot = drv;
      r.to_slot = s;
      r.to_input = i;
      r.p[0] = WirePt{x0, y0};
      r.p[1] = WirePt{xm, y0};
      r.p[2] = WirePt{xm, y1};
      r.p[3] = WirePt{x1, y1};
      sc.wires.push_back(r);
    }
  }
  return sc;
}

} // namespace wengine::logic
