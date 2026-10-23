// Interactive schematic canvas (see wengine/cli/logic_draw.hpp).
//
// Dear ImGui over the SDL2 2D renderer: GPU-anti-aliased vectors, TTF gate
// labels, wheel-zoom + drag-pan. Independent of the WEngineGui (ImGui/
// Vulkan) backend — the sdlrenderer2 backend needs SDL2 only, so no
// Vulkan dependency leaks into the CLI. Compiled only when SDL2 + the
// vendored imgui submodule are both present (CMake); otherwise the verb
// fails loudly with a rebuild hint (stub in logic_draw_sdl.cpp).
//
// File export keeps the SDL software path in logic_draw_sdl.cpp
// (headless-safe, byte-deterministic); this TU owns the window only.

#include "wengine/cli/logic_draw.hpp"

#include "fonts/schematic_font.hpp"
#include "sdl_gfx.hpp"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

namespace wengine::cli::logic_draw {

namespace {

#ifdef WENGINE_HAVE_IMGUI_DRAW

constexpr float kMinZoom = 0.2f;
constexpr float kMaxZoom = 8.0f;
constexpr float kLabelPx = 15.0f;
constexpr float kHeaderPx = 12.0f;
constexpr float kHintPx = 13.0f;
constexpr float kWireWPx = 3.0f;    // world px, scaled by zoom
constexpr float kOutlineWPx = 2.0f; // world px, scaled by zoom
constexpr float kDotRPx = 4.0f;     // world px, scaled by zoom
constexpr float kBubbleR = 4.0f;    // world px, scaled by zoom
constexpr float kGridStep = 20.0f;  // world px

struct View {
  float zoom = 1.0f;
  float ox = 0.0f;
  float oy = 0.0f;
};

[[nodiscard]] ImVec2 to_screen(const View &v, float x, float y) noexcept {
  return ImVec2(x * v.zoom + v.ox, y * v.zoom + v.oy);
}

void fit_view(View &v, float w, float h, float vw, float vh) noexcept {
  float z = 1.0f;
  if (w > 0.0f && h > 0.0f && vw > 0.0f && vh > 0.0f) {
    z = (vw - 80.0f) / w;
    const float zy = (vh - 80.0f) / h;
    if (zy < z) {
      z = zy;
    }
    if (z < kMinZoom) {
      z = kMinZoom;
    } else if (z > 4.0f) {
      z = 4.0f;
    }
  }
  v.zoom = z;
  v.ox = (vw - w * z) * 0.5f;
  v.oy = (vh - h * z) * 0.5f;
}

[[nodiscard]] std::string gate_label(const wengine::logic::Netlist &nl, std::uint16_t slot) {
  std::string s(wengine::logic::gate_kind_name(nl.gates[slot].kind));
  s += std::to_string(nl.gates[slot].counter);
  return s;
}

struct Scene {
  ImDrawList *dl = nullptr;
  ImFont *font = nullptr;
  View view;
  float wire_w = kWireWPx;
  float out_w = kOutlineWPx;
};

// Stroked polyline with round joints: ImGui strokes thick segments
// independently, leaving notches at every direction change (visible as
// dotted/pinched artifacts that shift with zoom). A disc on every vertex
// closes the joints; segments stay crisp straight ink.
void stroke_smooth(ImDrawList *dl, const ImVec2 *pts, int n, bool closed, ImU32 col, float thickness) {
  if (n < 2) {
    return;
  }
  const int segs = closed ? n : n - 1;
  for (int i = 0; i < segs; ++i) {
    dl->AddLine(pts[i], pts[(i + 1) % n], col, thickness);
  }
  const float r = thickness * 0.5f + 0.5f;
  for (int i = 0; i < n; ++i) {
    dl->AddCircleFilled(pts[i], r, col);
  }
}

[[nodiscard]] ImVec2 qbez_pt(ImVec2 a, ImVec2 c, ImVec2 b, float t) noexcept {
  const float u = 1.0f - t;
  return ImVec2(u * u * a.x + 2.0f * u * t * c.x + t * t * b.x,
                u * u * a.y + 2.0f * u * t * c.y + t * t * b.y);
}

void draw_gate_body(Scene &s, const wengine::logic::Gate &g, float x, float y, float w, float h) {
  using wengine::logic::GateKind;
  const ImU32 ink = IM_COL32(20, 20, 20, 255);
  const ImU32 paper = IM_COL32(255, 255, 255, 255);
  constexpr float kLead = 10.0f;
  constexpr float kPad = 4.0f;
  const float mid = y + h * 0.5f;
  const float lx = x + kLead;
  const bool inverted =
      (g.kind == GateKind::kNot || g.kind == GateKind::kNand || g.kind == GateKind::kNor || g.kind == GateKind::kXnor);
  const float tip_x = inverted ? (x + w - kLead - 4.0f) : (x + w - 2.0f);
  const bool xor_extra = (g.kind == GateKind::kXor || g.kind == GateKind::kXnor);
  const float shift = xor_extra ? 7.0f : 0.0f;
  const float bx = x + kLead + 2.0f + shift;
  const auto T = [&](float px, float py) { return to_screen(s.view, px, py); };
  const auto TW = [&](ImVec2 p) { return to_screen(s.view, p.x, p.y); };
  switch (g.kind) {
  case GateKind::kBuf:
  case GateKind::kNot: {
    const ImVec2 tri[3] = {T(lx, y + kPad), T(lx, y + h - kPad), T(tip_x, mid)};
    s.dl->AddTriangleFilled(tri[0], tri[1], tri[2], paper);
    stroke_smooth(s.dl, tri, 3, true, ink, s.out_w);
    if (inverted) {
      s.dl->AddCircleFilled(T(tip_x + 5.0f, mid), kBubbleR * s.view.zoom, paper);
      s.dl->AddCircle(T(tip_x + 5.0f, mid), kBubbleR * s.view.zoom, ink, 0, s.out_w);
      s.dl->AddLine(T(tip_x + 5.0f + kBubbleR, mid), T(x + w, mid), ink, s.wire_w);
    } else {
      s.dl->AddLine(T(tip_x, mid), T(x + w, mid), ink, s.wire_w);
    }
    break;
  }
  case GateKind::kAnd:
  case GateKind::kNand: {
    const float rh = h * 0.5f - kPad;
    const float body_right = inverted ? (x + w - kLead - 8.0f) : (x + w - 2.0f);
    const float ax = body_right - rh;
    std::vector<ImVec2> poly;
    poly.reserve(70);
    poly.push_back(T(lx, y + kPad));
    poly.push_back(T(ax, y + kPad));
    for (int d = -90; d <= 90; d += 3) {
      const float rad = static_cast<float>(d) * 3.14159265f / 180.0f;
      poly.push_back(to_screen(s.view, ax + rh * std::cos(rad), mid + rh * std::sin(rad)));
    }
    poly.push_back(T(ax, y + h - kPad));
    poly.push_back(T(lx, y + h - kPad));
    for (const ImVec2 &p : poly) {
      s.dl->PathLineTo(p);
    }
    s.dl->PathFillConvex(paper);
    stroke_smooth(s.dl, poly.data(), static_cast<int>(poly.size()), true, ink, s.out_w);
    if (inverted) {
      s.dl->AddCircleFilled(T(body_right + 4.0f, mid), kBubbleR * s.view.zoom, paper);
      s.dl->AddCircle(T(body_right + 4.0f, mid), kBubbleR * s.view.zoom, ink, 0, s.out_w);
      s.dl->AddLine(T(body_right + 4.0f + kBubbleR, mid), T(x + w, mid), ink, s.wire_w);
    } else {
      s.dl->AddLine(T(body_right, mid), T(x + w, mid), ink, s.wire_w);
    }
    break;
  }
  case GateKind::kOr:
  case GateKind::kNor:
  case GateKind::kXor:
  case GateKind::kXnor: {
    constexpr int kSeg = 32;
    std::vector<ImVec2> poly;
    poly.reserve(static_cast<std::size_t>(3 * kSeg + 1));
    for (int k = 0; k <= kSeg; ++k) {
      poly.push_back(TW(qbez_pt(ImVec2{bx, y + kPad}, ImVec2{bx + 12.0f, mid}, ImVec2{bx, y + h - kPad},
                               static_cast<float>(k) / static_cast<float>(kSeg))));
    }
    for (int k = 1; k <= kSeg; ++k) {
      poly.push_back(TW(qbez_pt(ImVec2{bx, y + h - kPad}, ImVec2{(bx + tip_x) * 0.5f + 8.0f, y + h - kPad},
                               ImVec2{tip_x, mid}, static_cast<float>(k) / static_cast<float>(kSeg))));
    }
    for (int k = 1; k <= kSeg; ++k) {
      poly.push_back(TW(qbez_pt(ImVec2{tip_x, mid}, ImVec2{(bx + tip_x) * 0.5f + 8.0f, y + kPad}, ImVec2{bx, y + kPad},
                               static_cast<float>(k) / static_cast<float>(kSeg))));
    }
    for (const ImVec2 &p : poly) {
      s.dl->PathLineTo(p);
    }
    s.dl->PathFillConvex(paper);
    stroke_smooth(s.dl, poly.data(), static_cast<int>(poly.size()), true, ink, s.out_w);
    if (xor_extra) {
      std::vector<ImVec2> extra;
      extra.reserve(static_cast<std::size_t>(kSeg + 1));
      for (int k = 0; k <= kSeg; ++k) {
        extra.push_back(TW(qbez_pt(ImVec2{bx - 7.0f, y + kPad}, ImVec2{bx - 7.0f + 12.0f, mid},
                                  ImVec2{bx - 7.0f, y + h - kPad}, static_cast<float>(k) / static_cast<float>(kSeg))));
      }
      stroke_smooth(s.dl, extra.data(), static_cast<int>(extra.size()), false, ink, s.out_w);
    }
    if (inverted) {
      s.dl->AddCircleFilled(T(tip_x + 4.0f, mid), kBubbleR * s.view.zoom, paper);
      s.dl->AddCircle(T(tip_x + 4.0f, mid), kBubbleR * s.view.zoom, ink, 0, s.out_w);
      s.dl->AddLine(T(tip_x + 4.0f + kBubbleR, mid), T(x + w, mid), ink, s.wire_w);
    } else {
      s.dl->AddLine(T(tip_x, mid), T(x + w, mid), ink, s.wire_w);
    }
    break;
  }
  } // switch
  // Input stubs land on the symbol border, drawn AFTER the body so the
  // concave OR/XOR back (whose convex fill slightly overfills) can never
  // white-out the joint. AND/BUF/NOT take the straight back at lx; OR/XOR
  // stubs ride the inverted bezier and overshoot 1px into the border.
  for (std::uint8_t i = 0; i < g.input_count; ++i) {
    const float yi = y + (static_cast<float>(i) + 1.0f) * h / (static_cast<float>(g.input_count) + 1.0f);
    float x_end = lx + 1.0f;
    if (g.kind == GateKind::kOr || g.kind == GateKind::kNor || g.kind == GateKind::kXor ||
        g.kind == GateKind::kXnor) {
      const float ex = xor_extra ? bx - 7.0f : bx;
      const int xi = gfx::qbez_x_at_y(gfx::Pt{static_cast<int>(ex), static_cast<int>(y + kPad)},
                                      gfx::Pt{static_cast<int>(ex + 12.0f), static_cast<int>(mid)},
                                      gfx::Pt{static_cast<int>(ex), static_cast<int>(y + h - kPad)},
                                      static_cast<int>(yi));
      x_end = static_cast<float>(xi) + 1.0f;
      if (x_end < x) {
        x_end = x;
      }
    }
    s.dl->AddLine(T(x, yi), T(x_end, yi), ink, s.wire_w);
  }
}

void draw_scene(Scene &s, const wengine::logic::Netlist &nl, const wengine::logic::Schematic &sc) {
  using wengine::logic::kLayoutBoxH;
  using wengine::logic::kLayoutBoxW;
  const ImU32 ink = IM_COL32(20, 20, 20, 255);
  const ImU32 grid = IM_COL32(233, 233, 233, 255);
  const ImU32 header = IM_COL32(110, 110, 110, 255);
  const auto T = [&](float x, float y) { return to_screen(s.view, x, y); };
  if (sc.gates.empty()) {
    const char *msg = "(EMPTY)";
    const ImVec2 ts = s.font->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, msg);
    const ImVec2 c = T(static_cast<float>(sc.width) * 0.5f, static_cast<float>(sc.height) * 0.5f);
    s.dl->AddText(s.font, 16.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(100, 100, 100, 255), msg);
    return;
  }
  // Faint dotted grid.
  {
    const float r = (s.view.zoom < 1.0f) ? 1.0f : s.view.zoom;
    for (float gy = kGridStep; gy < static_cast<float>(sc.height); gy += kGridStep) {
      for (float gx = kGridStep; gx < static_cast<float>(sc.width); gx += kGridStep) {
        s.dl->AddCircleFilled(T(gx, gy), r, grid);
      }
    }
  }
  // Column headers.
  {
    char buf[16];
    for (int lvl = 0; lvl < sc.levels; ++lvl) {
      (void)std::snprintf(buf, sizeof buf, "L%d", lvl);
      const float size = kHeaderPx * s.view.zoom;
      const ImVec2 ts = s.font->CalcTextSizeA(size, FLT_MAX, 0.0f, buf);
      const float cx = static_cast<float>(wengine::logic::kLayoutMargin) +
                       static_cast<float>(lvl) * static_cast<float>(wengine::logic::kLayoutColW) +
                       static_cast<float>(wengine::logic::kLayoutColW) * 0.5f;
      s.dl->AddText(s.font, size, ImVec2(T(cx, 0.0f).x - ts.x * 0.5f, T(0.0f, 6.0f).y), header, buf);
    }
  }
  // Wires under the bodies: Manhattan doglegs from the layout routes.
  // Round joints: a disc on each corner kills the miter notch that
  // independent thick segments leave behind.
  for (const auto &r : sc.wires) {
    const ImVec2 p0 = T(static_cast<float>(r.p[0].x), static_cast<float>(r.p[0].y));
    const ImVec2 p1 = T(static_cast<float>(r.p[1].x), static_cast<float>(r.p[1].y));
    const ImVec2 p2 = T(static_cast<float>(r.p[2].x), static_cast<float>(r.p[2].y));
    const ImVec2 p3 = T(static_cast<float>(r.p[3].x), static_cast<float>(r.p[3].y));
    s.dl->AddLine(p0, p1, ink, s.wire_w);
    s.dl->AddLine(p1, p2, ink, s.wire_w);
    s.dl->AddLine(p2, p3, ink, s.wire_w);
    const float jr = s.wire_w * 0.5f;
    s.dl->AddCircleFilled(p1, jr, ink);
    s.dl->AddCircleFilled(p2, jr, ink);
  }
  // Bodies.
  for (const auto &p : sc.gates) {
    draw_gate_body(s, nl.gates[p.slot], static_cast<float>(p.x), static_cast<float>(p.y),
                   static_cast<float>(kLayoutBoxW), static_cast<float>(kLayoutBoxH));
  }
  // Fanout junction dots.
  for (const auto &p : sc.gates) {
    if (nl.gates[p.slot].output_fanout > 1) {
      const ImVec2 c =
          T(static_cast<float>(p.x + kLayoutBoxW), static_cast<float>(p.y) + kLayoutBoxH * 0.5f);
      s.dl->AddCircleFilled(c, kDotRPx * s.view.zoom, ink);
    }
  }
  // Labels below the symbols.
  for (const auto &p : sc.gates) {
    const std::string name = gate_label(nl, p.slot);
    const float size = kLabelPx * s.view.zoom;
    const ImVec2 ts = s.font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str());
    s.dl->AddText(s.font, size,
                  ImVec2(T(static_cast<float>(p.x), 0.0f).x +
                             (static_cast<float>(kLayoutBoxW) * s.view.zoom - ts.x) * 0.5f,
                         T(0.0f, static_cast<float>(p.y + kLayoutBoxH) + 4.0f).y),
                  ink, name.c_str());
  }
}

// Process-lifetime resources for one window run; RAII teardown in reverse
// (backends, context, renderer, window) so every early return is clean.
struct Canvas {
  SDL_Window *win = nullptr;
  SDL_Renderer *ren = nullptr;
  bool imgui = false;
  ~Canvas() {
    if (imgui) {
      ImGui_ImplSDLRenderer2_Shutdown();
      ImGui_ImplSDL2_Shutdown();
      ImGui::DestroyContext();
    }
    if (ren != nullptr) {
      SDL_DestroyRenderer(ren);
    }
    if (win != nullptr) {
      SDL_DestroyWindow(win);
    }
  }
};

#endif // WENGINE_HAVE_IMGUI_DRAW

} // namespace

bool show_schematic(const wengine::logic::Netlist &nl, const wengine::logic::Schematic &sc, std::string &err) {
#ifdef WENGINE_HAVE_IMGUI_DRAW
  try {
    Canvas cv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      err = "cannot open schematic window (no display?)";
      return false;
    }
    char title[64];
    (void)std::snprintf(title, sizeof title, "logic draw (%u gates, %d levels)",
                        static_cast<unsigned>(nl.live_count), sc.levels);
    cv.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (cv.win == nullptr) {
      err = "cannot open schematic window (no display?)";
      return false;
    }
    // Software renderer on purpose (not accelerated-with-fallback like
    // the plot window): anti-aliasing comes from ImGui's own fringe
    // geometry + the font atlas, so GL buys nothing here — while GL
    // (llvmpipe on headless Mesa) spawns shader-compiler threads that
    // deadlock process teardown. Static scene => negligible CPU cost.
    cv.ren = SDL_CreateRenderer(cv.win, -1, SDL_RENDERER_SOFTWARE);
    if (cv.ren == nullptr) {
      err = "cannot open schematic renderer";
      return false;
    }
    IMGUI_CHECKVERSION();
    (void)ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini litter: a CLI viewer keeps no layout state
    ImGui::StyleColorsLight();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    if (!ImGui_ImplSDL2_InitForSDLRenderer(cv.win, cv.ren)) {
      err = "cannot init schematic UI";
      return false;
    }
    if (!ImGui_ImplSDLRenderer2_Init(cv.ren)) {
      err = "cannot init schematic UI";
      return false;
    }
    cv.imgui = true;
    // Embedded DejaVu Sans subset (no runtime font file to locate).
    static std::vector<unsigned char> font_bytes;
    if (font_bytes.empty()) {
      font_bytes.assign(detail::kSchematicFontData, detail::kSchematicFontData + detail::kSchematicFontSize);
    }
    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    ImFont *font = io.Fonts->AddFontFromMemoryTTF(font_bytes.data(), static_cast<int>(font_bytes.size()), 18.0f,
                                                  &font_cfg);
    if (font == nullptr) {
      err = "cannot load schematic font";
      return false;
    }
    Scene scene;
    scene.font = font;
    fit_view(scene.view, static_cast<float>(sc.width), static_cast<float>(sc.height), 1280.0f, 800.0f);
    bool quit = false;
    bool first = true;
    while (!quit) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev) != 0) {
        (void)ImGui_ImplSDL2_ProcessEvent(&ev);
        if (ev.type == SDL_QUIT) {
          quit = true;
        } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                   ev.window.windowID == SDL_GetWindowID(cv.win)) {
          quit = true;
        }
      }
      if (quit) {
        break;
      }
      ImGui_ImplSDLRenderer2_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      if (first) {
        // Real viewport size is known once the first frame runs.
        first = false;
        const ImGuiViewport *vp0 = ImGui::GetMainViewport();
        fit_view(scene.view, static_cast<float>(sc.width), static_cast<float>(sc.height), vp0->WorkSize.x,
                 vp0->WorkSize.y);
      }
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->WorkPos);
      ImGui::SetNextWindowSize(vp->WorkSize);
      constexpr ImGuiWindowFlags kHostFlags =
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoBackground;
      (void)ImGui::Begin("schematic", nullptr, kHostFlags);
      // Interactions: wheel zooms at the cursor, middle-drag pans,
      // F fits, R resets to 100 %, Esc closes.
      if (io.MouseWheel != 0.0f) {
        const float z0 = scene.view.zoom;
        float z1 = z0 * std::pow(1.1f, io.MouseWheel);
        if (z1 < kMinZoom) {
          z1 = kMinZoom;
        } else if (z1 > kMaxZoom) {
          z1 = kMaxZoom;
        }
        const float wx = (io.MousePos.x - scene.view.ox) / z0;
        const float wy = (io.MousePos.y - scene.view.oy) / z0;
        scene.view.zoom = z1;
        scene.view.ox = io.MousePos.x - wx * z1;
        scene.view.oy = io.MousePos.y - wy * z1;
      }
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        scene.view.ox += io.MouseDelta.x;
        scene.view.oy += io.MouseDelta.y;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        fit_view(scene.view, static_cast<float>(sc.width), static_cast<float>(sc.height), vp->WorkSize.x,
                 vp->WorkSize.y);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        scene.view.zoom = 1.0f;
        scene.view.ox = (vp->WorkSize.x - static_cast<float>(sc.width)) * 0.5f;
        scene.view.oy = (vp->WorkSize.y - static_cast<float>(sc.height)) * 0.5f;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        quit = true;
      }
      const float ws = kWireWPx * scene.view.zoom;
      scene.wire_w = (ws < 1.0f) ? 1.0f : ws;
      const float os = kOutlineWPx * scene.view.zoom;
      scene.out_w = (os < 1.0f) ? 1.0f : os;
      scene.dl = ImGui::GetWindowDrawList();
      draw_scene(scene, nl, sc);
      // Overlay: zoom readout + hints.
      {
        char zoom_txt[32];
        (void)std::snprintf(zoom_txt, sizeof zoom_txt, "%d%%", static_cast<int>(scene.view.zoom * 100.0f + 0.5f));
        scene.dl->AddText(font, kHintPx, ImVec2(vp->WorkSize.x - 64.0f, 8.0f), IM_COL32(110, 110, 110, 255),
                          zoom_txt);
        scene.dl->AddText(font, kHintPx, ImVec2(8.0f, vp->WorkSize.y - 22.0f), IM_COL32(110, 110, 110, 255),
                          "wheel: zoom | middle-drag: pan | F: fit | R: 1:1 | Esc: close");
      }
      (void)ImGui::End();
      ImGui::Render();
      (void)SDL_SetRenderDrawColor(cv.ren, 255, 255, 255, 255);
      (void)SDL_RenderClear(cv.ren);
      ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), cv.ren);
      SDL_RenderPresent(cv.ren);
    }
    return true;
  } catch (...) {
    err = "cannot open schematic window (out of memory?)";
    return false;
  }
#else
  (void)nl;
  (void)sc;
  err = "logic draw needs an ImGui canvas build (rebuild with SDL2 + the imgui submodule)";
  return false;
#endif
}

} // namespace wengine::cli::logic_draw
