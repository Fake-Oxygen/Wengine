// Standalone schematic raster TU (see wengine/cli/logic_draw.hpp).
//
// SDL2 software 2D rendering only — deliberately NOT the ImGui/Vulkan GUI
// backend: one window for `logic draw`, or an off-screen surface for
// `draw --out` / `export --bmp` via SDL_SaveBMP. Without WENGINE_HAVE_SDL2
// every method is a stub so the CLI still builds (verbs fail loudly at
// runtime with a rebuild hint).
//
// Symbol edges paint a gray halo under the black core (cheap,
// deterministic anti-aliasing with zero deps). Wires, text, dots and the
// grid stay crisp single-pass ink by design.

#include "wengine/cli/logic_draw.hpp"

#include "sdl_gfx.hpp"

#ifdef WENGINE_HAVE_SDL2
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <SDL.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wengine::cli::logic_draw {

namespace {

#ifdef WENGINE_HAVE_SDL2

using Pt = gfx::Pt;

// Primitives (font, thick lines, circles, polygon fill, beziers) live in
// the shared src-local header (src/cli/sdl_gfx.hpp); only the halo
// strokes, symbol bodies and scene assembly stay here.

// Render scale: the scene is authored in layout units and rasterized at
// 2x (window + BMP alike), so gates, wires and text read at a modern
// size. Layout goldens stay in 1x units; BMP pixel dims are 2x canvas.
inline constexpr int kRenderScale = 2;

// Halo strokes: every symbol edge paints a smooth gray tube first, then
// the 2px near-black core. Tube stamped disc-by-disc for continuous
// joints on curves.
void stroke_seg(SDL_Renderer *ren, int x0, int y0, int x1, int y1) noexcept {
  // Smooth halo tube: stamped discs keep polyline joints continuous
  // (per-segment quads leave dotted notches on curves).
  int dx = x1 - x0;
  if (dx < 0) {
    dx = -dx;
  }
  int dy = y1 - y0;
  if (dy < 0) {
    dy = -dy;
  }
  const int steps = (dx >= dy) ? dx : dy;
  (void)SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
  for (int k = 0; k <= steps; ++k) {
    const int cx = (steps == 0) ? x0 : x0 + (x1 - x0) * k / steps;
    const int cy = (steps == 0) ? y0 : y0 + (y1 - y0) * k / steps;
    gfx::fill_circle(ren, cx, cy, 2);
  }
  (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
  (void)SDL_RenderDrawLine(ren, x0, y0, x1, y1);
  // Second core pass on the dominant-perpendicular side: 2px ink.
  if (dx >= dy) {
    (void)SDL_RenderDrawLine(ren, x0, y0 + 1, x1, y1 + 1);
  } else {
    (void)SDL_RenderDrawLine(ren, x0 + 1, y0, x1 + 1, y1);
  }
}

void stroke_poly(SDL_Renderer *ren, const std::vector<Pt> &poly) noexcept {
  if (poly.size() < 2) {
    return;
  }
  for (std::size_t k = 0; k < poly.size(); ++k) {
    const Pt a = poly[k];
    const Pt b = poly[(k + 1) % poly.size()];
    stroke_seg(ren, a.x, a.y, b.x, b.y);
  }
}

void halo_circle(SDL_Renderer *ren, int cx, int cy, int r) noexcept {
  (void)SDL_SetRenderDrawColor(ren, 200, 200, 200, 255);
  gfx::fill_circle(ren, cx, cy, r + 1);
  (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
  gfx::fill_circle(ren, cx, cy, r);
  (void)SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  gfx::fill_circle(ren, cx, cy, r - 2);
}

[[nodiscard]] std::string gate_label(const wengine::logic::Netlist &nl, std::uint16_t slot) {
  std::string s(wengine::logic::gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

// One gate body in ANSI symbols. (x, y, w, h) is the layout box. Input
// leads bridge the box-edge wire anchors to the symbol border: straight
// to the flat back for AND/BUF/NOT, curve-solved onto the concave back
// for OR/XOR families (no floating stubs). Bubbles mark inversion
// (NOT/NAND/NOR/XNOR).
void draw_gate_body(SDL_Renderer *ren, const wengine::logic::Gate &g, int x, int y, int w, int h) noexcept {
  using wengine::logic::GateKind;
  constexpr int kLead = 10;
  constexpr int kPad = 4;
  const wengine::logic::GateKind kind = g.kind;
  const int n = g.input_count;
  const int mid = y + h / 2;
  const int lx = x + kLead;
  // Inversion bubble shared by NOT/NAND/NOR/XNOR: gray halo, 2px ring,
  // white core, with a short stub to the box edge.
  const auto bubble_at = [&](int cx) {
    constexpr int kR = 4;
    halo_circle(ren, cx, mid, kR);
    (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    gfx::hline(ren, cx + kR, x + w, mid, 3);
  };
  const bool inverted = (kind == GateKind::kNot || kind == GateKind::kNand || kind == GateKind::kNor ||
                         kind == GateKind::kXnor);
  const int tip_x = inverted ? (x + w - kLead - 4) : (x + w - 2);
  const bool is_or_family =
      (kind == GateKind::kOr || kind == GateKind::kNor || kind == GateKind::kXor || kind == GateKind::kXnor);
  const bool xor_extra = (kind == GateKind::kXor || kind == GateKind::kXnor);
  const int shift = xor_extra ? 7 : 0;
  const int bx = x + kLead + 2 + shift; // OR back-curve endpoints
  // Input stubs, landing on the actual border (curve-solved for OR/XOR).
  (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const int yi = y + (static_cast<int>(i) + 1) * h / (n + 1);
    int x_end = lx;
    if (is_or_family) {
      const int ex = xor_extra ? bx - 7 : bx;
      x_end = gfx::qbez_x_at_y(Pt{ex, y + kPad}, Pt{ex + 12, mid}, Pt{ex, y + h - kPad}, yi);
      if (x_end < x) {
        x_end = x;
      }
    }
    gfx::hline(ren, x, x_end, yi, 3);
  }
  switch (kind) {
  case GateKind::kBuf:
  case GateKind::kNot: {
    const Pt a{lx, y + kPad};
    const Pt b{lx, y + h - kPad};
    const Pt t{tip_x, mid};
    const std::vector<Pt> tri{a, b, t};
    (void)SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    gfx::fill_poly(ren, tri);
    stroke_poly(ren, tri);
    if (kind == GateKind::kNot) {
      bubble_at(tip_x + 5);
    } else {
      (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
      gfx::hline(ren, tip_x, x + w, mid, 3);
    }
    break;
  }
  case GateKind::kAnd:
  case GateKind::kNand: {
    const int rh = h / 2 - kPad;
    const int body_right = inverted ? (x + w - kLead - 8) : (x + w - 2);
    const int ax = body_right - rh;
    std::vector<Pt> poly;
    poly.push_back(Pt{lx, y + kPad});
    poly.push_back(Pt{ax, y + kPad});
    for (int d = -90; d <= 90; d += 3) {
      const double rad = static_cast<double>(d) * 3.141592653589793 / 180.0;
      poly.push_back(Pt{static_cast<int>(std::lround(static_cast<double>(ax) +
                                                     static_cast<double>(rh) * std::cos(rad))),
                        static_cast<int>(std::lround(static_cast<double>(mid) +
                                                     static_cast<double>(rh) * std::sin(rad)))});
    }
    poly.push_back(Pt{ax, y + h - kPad});
    poly.push_back(Pt{lx, y + h - kPad});
    (void)SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    gfx::fill_poly(ren, poly);
    stroke_poly(ren, poly);
    (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
    gfx::vline(ren, lx, y + kPad, y + h - kPad, 3); // straight back
    if (inverted) {
      bubble_at(body_right + 4);
    } else {
      gfx::hline(ren, body_right, x + w, mid, 3);
    }
    break;
  }
  case GateKind::kOr:
  case GateKind::kNor:
  case GateKind::kXor:
  case GateKind::kXnor: {
    std::vector<Pt> poly;
    // Back (input) concave curve, top -> bottom.
    gfx::sample_qbez(poly, Pt{bx, y + kPad}, Pt{bx + 12, mid}, Pt{bx, y + h - kPad}, 24);
    // Bottom curve, back -> tip.
    {
      std::vector<Pt> seg;
      gfx::sample_qbez(seg, Pt{bx, y + h - kPad}, Pt{(bx + tip_x) / 2 + 8, y + h - kPad}, Pt{tip_x, mid}, 24);
      for (std::size_t k = 1; k < seg.size(); ++k) {
        poly.push_back(seg[k]);
      }
    }
    // Top curve, tip -> back.
    {
      std::vector<Pt> seg;
      gfx::sample_qbez(seg, Pt{tip_x, mid}, Pt{(bx + tip_x) / 2 + 8, y + kPad}, Pt{bx, y + kPad}, 24);
      for (std::size_t k = 1; k < seg.size(); ++k) {
        poly.push_back(seg[k]);
      }
    }
    (void)SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    gfx::fill_poly(ren, poly);
    stroke_poly(ren, poly);
    if (xor_extra) {
      // Extra input curve: same concave shape, offset left, outline only.
      std::vector<Pt> extra;
      gfx::sample_qbez(extra, Pt{bx - 7, y + kPad}, Pt{bx - 7 + 12, mid}, Pt{bx - 7, y + h - kPad}, 24);
      for (std::size_t k = 0; k + 1 < extra.size(); ++k) {
        stroke_seg(ren, extra[k].x, extra[k].y, extra[k + 1].x, extra[k + 1].y);
      }
    }
    if (inverted) {
      bubble_at(tip_x + 4);
    } else {
      (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
      gfx::hline(ren, tip_x, x + w, mid, 3);
    }
    break;
  }
  }
}

void draw_scene(SDL_Renderer *ren, const wengine::logic::Netlist &nl,
                const wengine::logic::Schematic &sc) noexcept {
  using wengine::logic::kLayoutBoxH;
  using wengine::logic::kLayoutBoxW;
  // Paper background.
  (void)SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  (void)SDL_RenderClear(ren);

  if (sc.gates.empty()) {
    const char *msg = "(EMPTY)";
    const int tw = gfx::text_width(msg, 2);
    gfx::draw_text(ren, msg, (sc.width - tw) / 2, sc.height / 2 - 7, 2, 100, 100, 100);
    return;
  }

  // Faint dotted grid (CAD feel; drawn first so ink stays on top).
  {
    (void)SDL_SetRenderDrawColor(ren, 233, 233, 233, 255);
    constexpr int kGrid = 20;
    for (int gy = kGrid; gy < sc.height; gy += kGrid) {
      for (int gx = kGrid; gx < sc.width; gx += kGrid) {
        (void)SDL_RenderDrawPoint(ren, gx, gy);
      }
    }
  }

  // Column headers (level index per column).
  {
    char buf[16];
    for (int lvl = 0; lvl < sc.levels; ++lvl) {
      (void)std::snprintf(buf, sizeof buf, "L%d", lvl);
      const int cx = wengine::logic::kLayoutMargin + lvl * wengine::logic::kLayoutColW +
                     wengine::logic::kLayoutColW / 2;
      gfx::draw_text(ren, buf, cx - gfx::text_width(buf, 2) / 2, 6, 2, 110, 110, 110);
    }
  }

  // Wires under the bodies: Manhattan doglegs from the layout routes.
  (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
  for (const auto &r : sc.wires) {
    gfx::hline(ren, r.p[0].x, r.p[1].x, r.p[0].y, 3);
    gfx::vline(ren, r.p[1].x, r.p[1].y, r.p[2].y, 3);
    gfx::hline(ren, r.p[2].x, r.p[3].x, r.p[2].y, 3);
  }

  // Bodies (input leads included: they land on the symbol border).
  for (const auto &p : sc.gates) {
    draw_gate_body(ren, nl.gates[p.slot], p.x, p.y, kLayoutBoxW, kLayoutBoxH);
  }

  // Fanout junction dots: one driver fanning to several inputs branches
  // at the source anchor — the dot marks the branch (T-junctions of
  // unrelated routes never merge by construction).
  (void)SDL_SetRenderDrawColor(ren, 20, 20, 20, 255);
  for (const auto &p : sc.gates) {
    if (nl.gates[p.slot].output_fanout > 1) {
      gfx::fill_circle(ren, p.x + kLayoutBoxW, p.y + kLayoutBoxH / 2, 4);
    }
  }

  // Labels below the symbols (never over the ink; wires pass left/right
  // of the below-box band, never through it). Double-size pixel type for
  // readability.
  for (const auto &p : sc.gates) {
    const std::string name = gate_label(nl, p.slot);
    gfx::draw_text(ren, name.c_str(), p.x + kLayoutBoxW / 2 - gfx::text_width(name.c_str(), 2) / 2,
                        p.y + kLayoutBoxH + 4, 2, 20, 20, 20);
  }
}

#endif // WENGINE_HAVE_SDL2

} // namespace

bool supported() noexcept {
#ifdef WENGINE_HAVE_SDL2
  return true;
#else
  return false;
#endif
}

bool window_supported() noexcept {
#ifdef WENGINE_HAVE_IMGUI_DRAW
  return true;
#else
  return false;
#endif
}

#ifndef WENGINE_HAVE_IMGUI_DRAW
// Interactive canvas lives in logic_draw_imgui.cpp when the ImGui build
// is present; without it the verb fails loudly (same contract as the
// plot window without SDL2).
bool show_schematic(const wengine::logic::Netlist &nl, const wengine::logic::Schematic &sc, std::string &err) {
  (void)nl;
  (void)sc;
  err = "logic draw needs an ImGui canvas build (rebuild with SDL2 + the imgui submodule)";
  return false;
}
#endif

bool write_bmp(const std::string &path, const wengine::logic::Netlist &nl,
               const wengine::logic::Schematic &sc, int &w_out, int &h_out, std::string &err) {
#ifdef WENGINE_HAVE_SDL2
  const int w = ((sc.width <= 0) ? 480 : sc.width) * kRenderScale;
  const int h = ((sc.height <= 0) ? 320 : sc.height) * kRenderScale;
  w_out = w;
  h_out = h;
  if (SDL_Init(0) != 0) {
    err = "cannot init SDL (BMP export)";
    return false;
  }
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
  if (surf == nullptr) {
    SDL_Quit();
    err = "cannot create BMP surface";
    return false;
  }
  SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
  if (ren == nullptr) {
    SDL_FreeSurface(surf);
    SDL_Quit();
    err = "cannot create BMP renderer";
    return false;
  }
  (void)SDL_RenderSetScale(ren, static_cast<float>(kRenderScale), static_cast<float>(kRenderScale));
  draw_scene(ren, nl, sc);
  SDL_DestroyRenderer(ren);
  const int rc = SDL_SaveBMP(surf, path.c_str());
  SDL_FreeSurface(surf);
  SDL_Quit();
  if (rc != 0) {
    err = std::string("cannot write file '") + path + "'";
    return false;
  }
  return true;
#else
  (void)path;
  (void)nl;
  (void)sc;
  w_out = 0;
  h_out = 0;
  err = "logic draw needs an SDL2 build (rebuild with SDL2 available)";
  return false;
#endif
}

} // namespace wengine::cli::logic_draw
