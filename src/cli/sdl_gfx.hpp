#pragma once

// Shared SDL2 software-raster helpers for the CLI edge TUs
// (`src/cli/plot_sdl.cpp`, `src/cli/logic_draw_sdl.cpp`).
//
// Src-local (not public API): only TUs in src/cli/ include this, same
// pattern as `src/cli/commands/logic/common.hpp`. Keeps the 5x7 bitmap
// font (no SDL_ttf, no runtime font file) and small 2D primitives in one
// place instead of duplicated per TU. Header-only, allocation-free,
// exception-free; SDL2 types appear only under WENGINE_HAVE_SDL2 so the
// including TUs keep their headless stubs compiling without SDL2.
//
// Glyph coverage is the union the TUs need: A-Z, a-z, 0-9, space and
// `.-+:/()_=`. The lowercase shapes are verbatim the plot window's
// originals, so adopting this header changes no existing pixel.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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

namespace wengine::cli::gfx {

struct Pt {
  int x = 0;
  int y = 0;
};

#ifdef WENGINE_HAVE_SDL2

struct GlyphDef {
  char ch;
  const char *rows[7];
};

inline constexpr GlyphDef kGlyphs[] = {
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {".###.", "#...#", "....#", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", "#....", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "#...#", ".###."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#...#", ".###."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "#####", "##.##", "#.#.#", "#.#.#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "##..#", "#.#.#", "#..##", "#..##", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#####", "#...#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'a', {".....", ".....", ".###.", "....#", ".####", "#...#", ".###."}},
    {'b', {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####."}},
    {'c', {".....", ".....", ".###.", "#...#", "#....", "#...#", ".###."}},
    {'d', {"....#", "....#", ".####", "#...#", "#...#", "#...#", ".###."}},
    {'e', {".....", ".....", ".###.", "#...#", "####.", "#....", ".###."}},
    {'f', {"..##.", "..#..", "..#..", ".###.", "..#..", "..#..", "..#.."}},
    {'g', {".....", ".....", ".####", "#...#", ".###.", "....#", ".###."}},
    {'h', {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"}},
    {'i', {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."}},
    {'j', {"...#.", ".....", "..##.", "...#.", "...#.", "#...#", ".###."}},
    {'k', {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."}},
    {'l', {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'m', {".....", ".....", "#.#.#", "##.##", "#.#.#", "#.#.#", "#.#.#"}},
    {'n', {".....", ".....", ".##..", "#..#.", "#...#", "#...#", "#...#"}},
    {'o', {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."}},
    {'p', {".....", ".....", "####.", "#...#", "####.", "#....", "#...."}},
    {'q', {".....", ".....", ".####", "#...#", ".####", "....#", "....#"}},
    {'r', {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."}},
    {'s', {".....", ".....", ".####", "#....", ".###.", "....#", "####."}},
    {'t', {"..#..", "..#..", ".###.", "..#..", "..#..", "..#..", ".###."}},
    {'u', {".....", ".....", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'v', {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'w', {".....", ".....", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'x', {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"}},
    {'y', {".....", ".....", "#...#", "#...#", ".####", "....#", ".###."}},
    {'z', {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
};

[[nodiscard]] inline const GlyphDef *find_glyph(char c) noexcept {
  for (const auto &g : kGlyphs) {
    if (g.ch == c) {
      return &g;
    }
  }
  return nullptr;
}

inline void draw_char(SDL_Renderer *ren, char c, int x, int y, int scale, unsigned char r, unsigned char g,
                      unsigned char b) noexcept {
  const GlyphDef *gl = find_glyph(c);
  if (gl == nullptr || scale < 1) {
    return;
  }
  (void)SDL_SetRenderDrawColor(ren, r, g, b, 255);
  for (int row = 0; row < 7; ++row) {
    const char *bits = gl->rows[row];
    for (int col = 0; col < 5; ++col) {
      if (bits[col] == '#') {
        const SDL_Rect rc{x + col * scale, y + row * scale, scale, scale};
        (void)SDL_RenderFillRect(ren, &rc);
      }
    }
  }
}

inline void draw_text(SDL_Renderer *ren, const char *s, int x, int y, int scale, unsigned char r, unsigned char g,
                      unsigned char b) noexcept {
  if (s == nullptr) {
    return;
  }
  int cx = x;
  for (const char *p = s; *p != '\0'; ++p) {
    draw_char(ren, *p, cx, y, scale, r, g, b);
    cx += 6 * scale;
  }
}

// Faux-bold: double strike offset by one scaled pixel. Chunky at 5x7,
// still crisp.
inline void draw_text_bold(SDL_Renderer *ren, const char *s, int x, int y, int scale, unsigned char r,
                           unsigned char g, unsigned char b) noexcept {
  draw_text(ren, s, x, y, scale, r, g, b);
  draw_text(ren, s, x + scale, y, scale, r, g, b);
}

[[nodiscard]] inline int text_width(const char *s, int scale) noexcept {
  if (s == nullptr || scale < 1) {
    return 0;
  }
  const std::size_t n = std::strlen(s);
  if (n == 0) {
    return 0;
  }
  return static_cast<int>(n) * 6 * scale - scale;
}

[[nodiscard]] inline int bold_text_width(const char *s, int scale) noexcept { return text_width(s, scale) + scale; }

// Centered thick axis-aligned lines (wires, rules). Width 1 == single line.
inline void hline(SDL_Renderer *ren, int x0, int x1, int y, int w) noexcept {
  if (w < 1) {
    w = 1;
  }
  if (x0 > x1) {
    const int t = x0;
    x0 = x1;
    x1 = t;
  }
  for (int k = 0; k < w; ++k) {
    (void)SDL_RenderDrawLine(ren, x0, y - w / 2 + k, x1, y - w / 2 + k);
  }
}

inline void vline(SDL_Renderer *ren, int x, int y0, int y1, int w) noexcept {
  if (w < 1) {
    w = 1;
  }
  if (y0 > y1) {
    const int t = y0;
    y0 = y1;
    y1 = t;
  }
  for (int k = 0; k < w; ++k) {
    (void)SDL_RenderDrawLine(ren, x - w / 2 + k, y0, x - w / 2 + k, y1);
  }
}

inline void fill_circle(SDL_Renderer *ren, int cx, int cy, int r) noexcept {
  for (int dy = -r; dy <= r; ++dy) {
    const int dx = static_cast<int>(std::lround(std::sqrt(static_cast<double>(r * r - dy * dy))));
    (void)SDL_RenderDrawLine(ren, cx - dx, cy + dy, cx + dx, cy + dy);
  }
}

inline void outline_circle(SDL_Renderer *ren, int cx, int cy, int r) noexcept {
  // Midpoint circle: 8-way symmetry, 1px outline.
  int x = r;
  int y = 0;
  int err = 0;
  while (x >= y) {
    (void)SDL_RenderDrawPoint(ren, cx + x, cy + y);
    (void)SDL_RenderDrawPoint(ren, cx + y, cy + x);
    (void)SDL_RenderDrawPoint(ren, cx - y, cy + x);
    (void)SDL_RenderDrawPoint(ren, cx - x, cy + y);
    (void)SDL_RenderDrawPoint(ren, cx - x, cy - y);
    (void)SDL_RenderDrawPoint(ren, cx - y, cy - x);
    (void)SDL_RenderDrawPoint(ren, cx + y, cy - x);
    (void)SDL_RenderDrawPoint(ren, cx + x, cy - y);
    y += 1;
    err += 1 + 2 * y;
    if (2 * (err - x) + 1 > 0) {
      x -= 1;
      err += 1 - 2 * x;
    }
  }
}

// Even-odd scanline polygon fill (symbol bodies are tiny; no hot path).
inline void fill_poly(SDL_Renderer *ren, const std::vector<Pt> &poly) noexcept {
  if (poly.size() < 3) {
    return;
  }
  int ymin = poly[0].y;
  int ymax = poly[0].y;
  for (const Pt &p : poly) {
    ymin = (p.y < ymin) ? p.y : ymin;
    ymax = (p.y > ymax) ? p.y : ymax;
  }
  std::vector<double> xs;
  for (int y = ymin; y <= ymax; ++y) {
    xs.clear();
    for (std::size_t k = 0; k < poly.size(); ++k) {
      const Pt a = poly[k];
      const Pt b = poly[(k + 1) % poly.size()];
      if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
        const double t = static_cast<double>(y - a.y) / static_cast<double>(b.y - a.y);
        xs.push_back(static_cast<double>(a.x) + t * static_cast<double>(b.x - a.x));
      }
    }
    std::sort(xs.begin(), xs.end());
    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
      const int x0 = static_cast<int>(std::lround(xs[k]));
      const int x1 = static_cast<int>(std::lround(xs[k + 1]));
      if (x1 >= x0) {
        (void)SDL_RenderDrawLine(ren, x0, y, x1, y);
      }
    }
  }
}

[[nodiscard]] inline Pt qbez(Pt a, Pt c, Pt b, double t) noexcept {
  const double u = 1.0 - t;
  const double x = u * u * static_cast<double>(a.x) + 2.0 * u * t * static_cast<double>(c.x) +
                   t * t * static_cast<double>(b.x);
  const double y = u * u * static_cast<double>(a.y) + 2.0 * u * t * static_cast<double>(c.y) +
                   t * t * static_cast<double>(b.y);
  return Pt{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
}

// X of a quadratic bezier at height y. The symbol back-curves using this
// are y-monotonic, so exactly one root lands in [0,1] (clamped
// otherwise). Used to land OR/XOR input stubs exactly on the curved
// border instead of floating short of it.
[[nodiscard]] inline int qbez_x_at_y(Pt a, Pt c, Pt b, int y) noexcept {
  const double y0 = static_cast<double>(a.y);
  const double y1 = static_cast<double>(c.y);
  const double y2 = static_cast<double>(b.y);
  const double A = y0 - 2.0 * y1 + y2;
  const double B = 2.0 * (y1 - y0);
  const double C = y0 - static_cast<double>(y);
  double t = 0.5;
  if (std::fabs(A) < 1e-9) {
    if (std::fabs(B) >= 1e-9) {
      t = -C / B;
    }
  } else {
    const double disc = B * B - 4.0 * A * C;
    if (disc >= 0.0) {
      const double sq = std::sqrt(disc);
      const double t0 = (-B - sq) / (2.0 * A);
      const double t1 = (-B + sq) / (2.0 * A);
      t = (t0 >= 0.0 && t0 <= 1.0) ? t0 : t1;
    }
  }
  if (t < 0.0) {
    t = 0.0;
  } else if (t > 1.0) {
    t = 1.0;
  }
  return qbez(a, c, b, t).x;
}

inline void sample_qbez(std::vector<Pt> &out, Pt a, Pt c, Pt b, int steps) {
  for (int k = 0; k <= steps; ++k) {
    out.push_back(qbez(a, c, b, static_cast<double>(k) / static_cast<double>(steps)));
  }
}

#endif // WENGINE_HAVE_SDL2

} // namespace wengine::cli::gfx
