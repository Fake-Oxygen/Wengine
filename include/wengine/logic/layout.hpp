#pragma once

/**
 * @file
 * Layered schematic layout (LOGIC-7): the drawing stage of the logic pipeline
 * (netlist parse -> state -> eval -> layout/topo -> optimize). Pure
 * geometry over the compiled topology. Header-only, SDL-free;
 * allocation happens only here at draw time (never on a solver hot
 * path).
 *
 * Determinism: slot-order scans everywhere; column = topo_level
 * (inputs-only gates sit at level 0 on the left, sinks at the max level
 * on the right); within a level gates stack in slot order. Wire routes
 * are Manhattan doglegs through the mid column between the boxes:
 * (x0,y0) -> (xm,y0) -> (xm,y1) -> (x1,y1).
 *
 * Precondition: the caller compiles the topology first (topo::rebuild;
 * draw/export verbs do). On a topology error the result is an empty
 * schematic (levels = 0); the verbs report the error themselves before
 * calling here.
 *
 * Dependency rules (docs/logic.md section 8): depends on pins.hpp,
 * topo.hpp, and types.hpp; both raster backends (SDL BMP export, ImGui
 * canvas) consume the layout without recomputing it. Logic is a
 * standalone domain: this header must never depend on CLI types.
 * Consumers: src/cli/logic_draw_sdl.cpp, the ImGui canvas, and the
 * test_logic_layout suite.
 */

#include "wengine/logic/pins.hpp"
#include "wengine/logic/topo.hpp"
#include "wengine/logic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wengine::logic {

/**
 * @brief Box + grid metrics shared with the SDL raster TU (single
 *        source of truth so layout goldens and pixels cannot drift
 *        apart). Boxes are datasheet-proportioned (near-square
 *        distinctive shapes, not wide bars).
 */
/** @brief Column width in layout units. */
inline constexpr int kLayoutColW = 120;
/** @brief Gate body width in layout units. */
inline constexpr int kLayoutBoxW = 76;
/** @brief Gate body height in layout units. */
inline constexpr int kLayoutBoxH = 56;
/** @brief Vertical gap between stacked gates. */
inline constexpr int kLayoutGapY = 30;
/** @brief Canvas margin around the schematic. */
inline constexpr int kLayoutMargin = 30;

/**
 * @brief One placed gate box: slot, level column, and pixel rect.
 */
struct Placed {
  /** @brief Netlist slot of the placed gate. */
  std::uint16_t slot = 0;
  /** @brief Level column (== topo_level). */
  int level = 0;
  /** @brief Left edge in layout units. */
  int x = 0;
  /** @brief Top edge in layout units. */
  int y = 0;
};

/**
 * @brief One Manhattan route point in layout units.
 */
struct WirePt {
  /** @brief Horizontal position in layout units. */
  int x = 0;
  /** @brief Vertical position in layout units. */
  int y = 0;
};

/**
 * @brief One routed wire: driver, sink, and its 4 Manhattan points.
 */
struct WireRoute {
  /** @brief Driver slot (kNoDriver when unwired — never routed). */
  std::uint16_t from_slot = kNoDriver;
  /** @brief Sink slot. */
  std::uint16_t to_slot = kNoDriver;
  /** @brief 0-based sink input index. */
  std::uint8_t to_input = 0;
  /** @brief Route points: (x0,y0) -> (xm,y0) -> (xm,y1) -> (x1,y1). */
  WirePt p[4] = {};
};

/**
 * @brief Whole placed schematic: gates plus routed wires plus extents.
 */
struct Schematic {
  /** @brief Placed gates in slot order. */
  std::vector<Placed> gates;     // slot order
  /** @brief Routed wires: destination slot order, then input index. */
  std::vector<WireRoute> wires;  // destination slot order, then input index
  /** @brief Level-column count (0 for the empty/error schematic). */
  int levels = 0;
  /** @brief Canvas width in layout units. */
  int width = 0;
  /** @brief Canvas height in layout units. */
  int height = 0;
};

/**
 * @brief Sequential pin anchors: the single owner is the pins.hpp
 *        KindPin table (anchor_row/is_clock/is_notch per pin, face per
 *        kind). The queries below are thin adapters over that table so
 *        layout, parse, and raster never drift apart.
 *
 * Roles (row numerators, denominator kSeqAnchorDenom): DFF: D0->1 CLK1->2
 * RST2->3 SET3->4 EN4->5; DLATCH: D0->1 EN1->2; JK: J0->1 K1->4 CLK2->2;
 * T: T0->1 CLK1->2; SRLATCH: S0->1 R1->4.
 */
/** @brief Denominator of the schematic role rows (numerators 1..5). */
inline constexpr int kSeqAnchorDenom = 6;

/**
 * @brief Role row numerator for one sequential input pin.
 *
 * @param[in] kind Sequential gate kind.
 * @param[in] input 0-based input index.
 * @return Row numerator (denominator kSeqAnchorDenom), or -1 for
 *         non-sequential kinds and unknown pins.
 */
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

/**
 * @brief True for the clock-wedge pins of edge-triggered flops
 *        (SRLATCH has none).
 *
 * @param[in] kind Gate kind.
 * @param[in] input 0-based input index.
 * @return True when the pin carries the clock wedge.
 */
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

/**
 * @brief True for the latch-enable notch pin (level-sensitive DLATCH
 *        only).
 *
 * @param[in] kind Gate kind.
 * @param[in] input 0-based input index.
 * @return True when the pin carries the enable notch.
 */
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

/**
 * @brief Face letters painted on sequential bodies ("?" for unknown
 *        kinds).
 *
 * @param[in] kind Gate kind.
 * @return Kind table face, or "?" for combinational/unknown kinds.
 */
[[nodiscard]] constexpr std::string_view seq_face(GateKind kind) noexcept {
  for (const KindInfo &e : kKinds) {
    if (e.kind == kind) {
      return e.face.empty() ? std::string_view{"?"} : e.face;
    }
  }
  return "?";
}

/**
 * @brief Anchor points for one placed box: input anchors sit on the
 *        left edge at the role row; Q keeps the mid convention; NQ sits
 *        at row 5 with the inversion bubble.
 *
 * @param[in] box Placed box.
 * @param[in] kind Gate kind (role row lookup).
 * @param[in] input 0-based input index.
 * @return Anchor point for the input pin.
 */
[[nodiscard]] constexpr WirePt seq_input_anchor(const Placed &box, GateKind kind, std::uint8_t input) noexcept {
  const int row = seq_input_row(kind, input);
  const int y = (row < 0) ? (box.y + kLayoutBoxH / 2) : (box.y + row * kLayoutBoxH / kSeqAnchorDenom);
  return WirePt{box.x, y};
}

/**
 * @brief Q output anchor: right edge at the mid convention.
 *
 * @param[in] box Placed box.
 * @return Q anchor point.
 */
[[nodiscard]] constexpr WirePt seq_q_anchor(const Placed &box) noexcept {
  return WirePt{box.x + kLayoutBoxW, box.y + kLayoutBoxH / 2};
}

/**
 * @brief NQ output anchor: right edge at row 5 with the inversion
 *        bubble.
 *
 * @param[in] box Placed box.
 * @return NQ anchor point.
 */
[[nodiscard]] constexpr WirePt seq_nq_anchor(const Placed &box) noexcept {
  return WirePt{box.x + kLayoutBoxW, box.y + 5 * kLayoutBoxH / kSeqAnchorDenom};
}

/**
 * @brief True when the sink input reads the negated (NQ) source value.
 *
 * Index mirrors tie_flat(slot, input); only wire_pins() from an NQ
 * source ever sets the flag (see types.hpp Gate::driver_inv).
 *
 * @param[in] nl Netlist holding driver_inv.
 * @param[in] slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return True when the sink reads NQ.
 */
[[nodiscard]] inline bool sink_reads_nq(const Netlist &nl, std::uint16_t slot, std::uint8_t input) noexcept {
  const std::size_t flat = static_cast<std::size_t>(slot) * kMaxInputs + static_cast<std::size_t>(input);
  return nl.driver_inv[flat] != 0;
}

/**
 * @brief Wire-lane stagger: wires spanning the same column gap take
 *        deterministic lanes so unrelated nets never collapse into one
 *        trunk.
 *
 * Same-gap wires share the midpoint (x0+x1)/2; each wire takes a lane by
 * its ordinal among same-gap wires (slot-order enumeration, so repeat
 * layouts are identical): lane 0 stays on the midpoint, later lanes
 * alternate sides in 8-unit steps. Same-driver branches still meet
 * exactly at the source anchor (where the fanout dot marks the
 * connection); unrelated nets never share a vertical.
 */
/** @brief Lane step in layout units. */
inline constexpr int kWireLaneStep = 8;

/**
 * @brief Lane offset for one lane ordinal: 0, +8, -8, +16, -16, ...
 *
 * @param[in] lane Ordinal among same-gap wires.
 * @return Horizontal offset in layout units.
 */
[[nodiscard]] constexpr int wire_lane_offset(std::size_t lane) noexcept {
  const int half = static_cast<int>((lane + 1u) / 2u);
  const int mag = half * kWireLaneStep;
  return ((lane & 1u) != 0u) ? mag : -mag; // 0, +8, -8, +16, -16, ...
}

namespace detail {
/**
 * @brief Internal seams for layout_schematic (never public): placement,
 *        canvas, anchors, and lane ownership. The public interface stays
 *        Netlist to Schematic.
 *
 * @param[in] nl Netlist placing from (topo levels read).
 * @param[out] sc Schematic receiving placed gates.
 * @param[in] per_level Live-gate count per level (placement cursor).
 */
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

/**
 * @brief Canvas extents from per-level gate counts.
 *
 * @param[in] per_level Live-gate count per level.
 * @param[in] levels Level-column count.
 * @param[out] w Canvas width in layout units.
 * @param[out] h Canvas height in layout units.
 */
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

/**
 * @brief Source anchor height: NQ-reading sinks start at the NQ anchor,
 *        everything else at the Q anchor (the mid convention, unchanged
 *        for combinational sources since driver_inv is only ever set
 *        from NQ sources).
 *
 * Sequential endpoints use role anchors; combinational endpoints keep
 * the legacy spread so existing goldens hold.
 *
 * @param[in] nl Netlist holding NQ polarity.
 * @param[in] src Placed source box.
 * @param[in] dst_slot Sink slot.
 * @param[in] input 0-based sink input index.
 * @return Source Y in layout units.
 */
inline int source_y(const Netlist &nl, const Placed &src, std::uint16_t dst_slot, std::uint8_t input) {
  if (is_sequential_kind(nl.gates[src.slot].kind) && sink_reads_nq(nl, dst_slot, input)) {
    return seq_nq_anchor(src).y;
  }
  return seq_q_anchor(src).y;
}

/**
 * @brief Sink anchor height: role row for sequential sinks, even spread
 *        for combinational sinks.
 *
 * @param[in] dst Placed sink box.
 * @param[in] g Sink gate (input count for the spread).
 * @param[in] input 0-based sink input index.
 * @return Sink Y in layout units.
 */
inline int sink_y(const Placed &dst, const Gate &g, std::uint8_t input) {
  if (seq_input_row(g.kind, input) >= 0) {
    return seq_input_anchor(dst, g.kind, input).y;
  }
  const int n = g.input_count;
  return dst.y + (static_cast<int>(input) + 1) * kLayoutBoxH / (n + 1);
}

/**
 * @brief Per-gap lane ownership: (src_level, dst_level) to wires seen
 *        so far. Linear scan over distinct gaps (a handful in practice);
 *        the wire loop enumerates in destination slot order, so lanes
 *        are deterministic.
 */
struct Lanes {
  /** @brief Owned lane counters per column gap. */
  std::vector<std::pair<std::pair<int, int>, std::size_t>> use;
  /**
   * @brief Take the next lane for one column gap.
   *
   * @param[in] gap (src_level, dst_level) column pair.
   * @return Lane ordinal for the new wire.
   */
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

/**
 * @brief Place the whole netlist plus route its wires: levels left to
 *        right, slot order within a level, Manhattan doglegs through
 *        staggered mid-column lanes.
 *
 * @param[out] nl Netlist to lay out (topology recompiled when dirty).
 * @return Placed schematic; empty (levels = 0) when the netlist is
 *         empty or the topology fails (the verb reports the error).
 */
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
