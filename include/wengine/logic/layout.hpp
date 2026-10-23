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
// (`ensure_topology`; draw/export verbs do). On a topology error the
// result is an empty schematic (levels = 0); the verbs report the error
// themselves before calling here.

#include "wengine/logic/eval.hpp"
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

[[nodiscard]] inline Schematic layout_schematic(Netlist &nl) {
  Schematic sc;
  if (nl.live_count == 0) {
    sc.width = 2 * kLayoutMargin;
    sc.height = 2 * kLayoutMargin;
    return sc;
  }
  if (!ensure_topology(nl).ok()) {
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
  int max_count = 1;
  for (int c : per_level) {
    if (c > max_count) {
      max_count = c;
    }
  }
  sc.width = kLayoutMargin * 2 + sc.levels * kLayoutColW;
  sc.height = kLayoutMargin * 2 + max_count * (kLayoutBoxH + kLayoutGapY);

  // Slot -> Placed lookup for anchor math (live slots only).
  const Placed *by_slot[kMaxGates] = {};
  for (const Placed &p : sc.gates) {
    by_slot[p.slot] = &p;
  }
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
      const int n = g.input_count;
      const int y0 = src->y + kLayoutBoxH / 2;
      const int y1 = dst->y + (static_cast<int>(i) + 1) * kLayoutBoxH / (n + 1);
      const int x0 = src->x + kLayoutBoxW;
      const int x1 = dst->x;
      const int xm = (x0 + x1) / 2;
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
