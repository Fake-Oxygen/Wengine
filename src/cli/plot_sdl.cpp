// Live signal plot window (see wengine/cli/plot.hpp).
//
// SDL2 software 2D renderer only — deliberately NOT the ImGui/Vulkan GUI
// backend: one window, polled from the `sig run` step loop, throttled to
// ~30 Hz wall-clock. Without WENGINE_HAVE_SDL2 every method is a stub so
// the CLI still builds (open() fails loudly at runtime).

#include "wengine/cli/plot.hpp"

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

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wengine::cli {

#ifdef WENGINE_HAVE_SDL2

struct PlotSeries {
  std::uint64_t serial = 0;
  std::deque<double> t;
  std::deque<double> v;
};

namespace {

// 8-color palette (dark background): white first, then saturated hues.
constexpr unsigned char kPalette[8][3] = {
    {236, 240, 241}, {46, 204, 113}, {241, 196, 15}, {26, 188, 156},
    {155, 89, 182}, {230, 126, 34}, {231, 76, 60}, {52, 152, 219},
};

using SeriesVec = std::vector<PlotSeries>;

// Embedded 5x7 bitmap font for axis tick values + titles: keeps the
// plotter dependency-free (no SDL_ttf, no runtime font file to locate).
// Covers only what the axes print: digits, ".", "-", "+", "e" float
// spellings ("%.3g"), and the title alphabet. Unknown chars render
// blank (advance only), never fail.
struct GlyphDef {
  char ch;
  const char *rows[7];
};

constexpr GlyphDef kGlyphs[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {".###.", "#...#", "....#", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "#...#", ".###."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'e', {".....", ".....", ".###.", "#...#", "####.", "#....", ".###."}},
    {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'i', {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."}},
    {'m', {".....", ".....", "#.#.#", "##.##", "#.#.#", "#.#.#", "#.#.#"}},
    {'u', {".....", ".....", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'l', {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'a', {".....", ".....", ".###.", "....#", ".####", "#...#", ".###."}},
    {'t', {"..#..", "..#..", ".###.", "..#..", "..#..", "..#..", ".###."}},
    {'o', {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."}},
    {'n', {".....", ".....", ".##..", "#..#.", "#...#", "#...#", "#...#"}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'s', {".....", ".....", ".####", "#....", ".###.", "....#", "####."}},
    {'r', {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."}},
};

[[nodiscard]] const GlyphDef *find_glyph(char c) noexcept {
  for (const auto &g : kGlyphs) {
    if (g.ch == c) {
      return &g;
    }
  }
  return nullptr;
}

void draw_char(SDL_Renderer *ren, char c, int x, int y, int scale, unsigned char r, unsigned char g, unsigned char b) noexcept {
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

[[nodiscard]] int text_width(const char *s, int scale) noexcept {
  if (s == nullptr || scale < 1) {
    return 0;
  }
  const std::size_t n = std::strlen(s);
  if (n == 0) {
    return 0;
  }
  return static_cast<int>(n) * 6 * scale - scale;
}

void draw_text(SDL_Renderer *ren, const char *s, int x, int y, int scale, unsigned char r, unsigned char g, unsigned char b) noexcept {
  if (s == nullptr) {
    return;
  }
  int cx = x;
  for (const char *p = s; *p != '\0'; ++p) {
    draw_char(ren, *p, cx, y, scale, r, g, b);
    cx += 6 * scale;
  }
}

void draw_frame(SDL_Renderer *ren, const SeriesVec &series, std::uint64_t samples) noexcept {
  int w = 960;
  int h = 600;
  (void)SDL_GetRendererOutputSize(ren, &w, &h);
  if (w <= 0 || h <= 0) {
    return;
  }

  // Background.
  (void)SDL_SetRenderDrawColor(ren, 16, 20, 28, 255);
  (void)SDL_RenderClear(ren);

  // Data range over the visible history.
  bool any = false;
  double tmin = 0.0;
  double tmax = 0.0;
  double vmin = 0.0;
  double vmax = 0.0;
  for (const auto &s : series) {
    if (s.t.empty()) {
      continue;
    }
    if (!any) {
      tmin = s.t.front();
      tmax = s.t.back();
      vmin = s.v.front();
      vmax = s.v.front();
      any = true;
    } else {
      tmin = (s.t.front() < tmin) ? s.t.front() : tmin;
      tmax = (s.t.back() > tmax) ? s.t.back() : tmax;
    }
    for (double v : s.v) {
      vmin = (v < vmin) ? v : vmin;
      vmax = (v > vmax) ? v : vmax;
    }
  }
  if (!any) {
    SDL_RenderPresent(ren);
    return;
  }
  if (!(tmax > tmin)) {
    tmin -= 1.0;
    tmax += 1.0;
  }
  double pad = (vmax - vmin) * 0.1;
  if (!(pad > 0.0)) {
    pad = (std::fabs(vmax) > 1.0) ? std::fabs(vmax) * 0.1 : 1.0;
  }
  vmin -= pad;
  vmax += pad;

  const double dw = static_cast<double>(w);
  const double dh = static_cast<double>(h);
  const double ml = 68.0;
  const double mr = 16.0;
  const double mt = 26.0;
  const double mb = 52.0;
  const auto map_x = [&](double t) {
    return ml + (t - tmin) / (tmax - tmin) * (dw - ml - mr);
  };
  const auto map_y = [&](double v) {
    return mt + (1.0 - (v - vmin) / (vmax - vmin)) * (dh - mt - mb);
  };

  // Grid: horizontal vmin/mid/vmax + zero line when in range, vertical thirds.
  (void)SDL_SetRenderDrawColor(ren, 60, 66, 78, 255);
  for (int k = 0; k <= 4; ++k) {
    const double v = vmin + (vmax - vmin) * static_cast<double>(k) / 4.0;
    const int y = static_cast<int>(map_y(v));
    (void)SDL_RenderDrawLine(ren, static_cast<int>(ml), y,
                             static_cast<int>(dw - mr), y);
  }
  for (int k = 0; k <= 4; ++k) {
    const double t = tmin + (tmax - tmin) * static_cast<double>(k) / 4.0;
    const int x = static_cast<int>(map_x(t));
    (void)SDL_RenderDrawLine(ren, x, static_cast<int>(mt), x,
                             static_cast<int>(dh - mb));
  }
  if (vmin <= 0.0 && vmax >= 0.0) {
    const int y = static_cast<int>(map_y(0.0));
    (void)SDL_SetRenderDrawColor(ren, 120, 126, 140, 255);
    (void)SDL_RenderDrawLine(ren, static_cast<int>(ml), y,
                             static_cast<int>(dw - mr), y);
  }

  // Series polylines.
  std::size_t color = 0;
  for (const auto &s : series) {
    if (s.t.size() < 1) {
      ++color;
      continue;
    }
    const unsigned char *c = kPalette[color % 8];
    (void)SDL_SetRenderDrawColor(ren, c[0], c[1], c[2], 255);
    int px = static_cast<int>(map_x(s.t.front()));
    int py = static_cast<int>(map_y(s.v.front()));
    for (std::size_t k = 1; k < s.t.size(); ++k) {
      const int x = static_cast<int>(map_x(s.t[k]));
      const int y = static_cast<int>(map_y(s.v[k]));
      (void)SDL_RenderDrawLine(ren, px, py, x, y);
      px = x;
      py = y;
    }
    ++color;
  }

  // Border.
  (void)SDL_SetRenderDrawColor(ren, 90, 96, 110, 255);
  const SDL_Rect border{static_cast<int>(ml), static_cast<int>(mt), static_cast<int>(dw - ml - mr), static_cast<int>(dh - mt - mb)};
  (void)SDL_RenderDrawRect(ren, &border);

  // Axis tick values (same positions as the grid) + axis titles.
  char buf[16];
  for (int k = 0; k <= 4; ++k) {
    const double v = vmin + (vmax - vmin) * static_cast<double>(k) / 4.0;
    (void)std::snprintf(buf, sizeof buf, "%.3g", v);
    const int tw = text_width(buf, 1);
    draw_text(ren, buf, static_cast<int>(ml) - 8 - tw, static_cast<int>(map_y(v)) - 3, 1, 150, 156, 170);
  }
  for (int k = 0; k <= 4; ++k) {
    const double t = tmin + (tmax - tmin) * static_cast<double>(k) / 4.0;
    (void)std::snprintf(buf, sizeof buf, "%.3g", t);
    const int tw = text_width(buf, 1);
    draw_text(ren, buf, static_cast<int>(map_x(t)) - tw / 2, static_cast<int>(dh - mb) + 6, 1, 150, 156, 170);
  }
  draw_text(ren, "Value (SI units)", 8, 4, 2, 236, 240, 241);
  const char *xtitle = "Simulation time (s)";
  const int xw = text_width(xtitle, 2);
  draw_text(ren, xtitle, (w - xw) / 2, h - 20, 2, 236, 240, 241);

  // Legend (only when several sources share the window): color swatch +
  // "s<serial>" per live series, top-right inside the plot area. Capped
  // to the plot height with a "+N more" tail. Series iterate in slot
  // order with the same color counter as the curves above, so swatches
  // always match.
  std::size_t n_live = 0;
  for (const auto &s : series) {
    if (!s.t.empty()) {
      ++n_live;
    }
  }
  if (n_live > 1) {
    const int sw = 12;
    const int row_h = 11;
    const int lpad = 6;
    const int plot_h = static_cast<int>(dh - mt - mb);
    int max_rows = (plot_h - 16 - 2 * lpad) / row_h;
    if (max_rows < 2) {
      max_rows = 2;
    }
    const std::size_t shown = (n_live > static_cast<std::size_t>(max_rows)) ? static_cast<std::size_t>(max_rows - 1) : n_live;
    const std::size_t hidden = n_live - shown;
    char leg[32];
    int leg_w = 0;
    auto leg_measure = [&](std::uint64_t id) {
      (void)std::snprintf(leg, sizeof leg, "s%llu", static_cast<unsigned long long>(id));
      const int tw = text_width(leg, 1);
      if (tw > leg_w) {
        leg_w = tw;
      }
    };
    std::size_t measured = 0;
    for (const auto &s : series) {
      if (s.t.empty() || measured >= shown) {
        continue;
      }
      leg_measure(s.serial);
      ++measured;
    }
    if (hidden > 0) {
      (void)std::snprintf(leg, sizeof leg, "+%llu more", static_cast<unsigned long long>(hidden));
      const int tw = text_width(leg, 1);
      if (tw > leg_w) {
        leg_w = tw;
      }
    }
    const std::size_t box_rows = shown + (hidden > 0 ? 1 : 0);
    const int box_w = lpad * 2 + sw + 6 + leg_w;
    const int box_h = lpad * 2 + static_cast<int>(box_rows) * row_h - 4;
    const int bx = static_cast<int>(dw - mr) - box_w - 8;
    const int by = static_cast<int>(mt) + 8;
    (void)SDL_SetRenderDrawColor(ren, 24, 28, 38, 255);
    const SDL_Rect box{bx, by, box_w, box_h};
    (void)SDL_RenderFillRect(ren, &box);
    (void)SDL_SetRenderDrawColor(ren, 90, 96, 110, 255);
    (void)SDL_RenderDrawRect(ren, &box);
    std::size_t lcolor = 0;
    std::size_t row = 0;
    for (const auto &s : series) {
      if (s.t.empty()) {
        ++lcolor;
        continue;
      }
      if (row < shown) {
        const unsigned char *c = kPalette[lcolor % 8];
        const int ry = by + lpad + static_cast<int>(row) * row_h + 2;
        (void)SDL_SetRenderDrawColor(ren, c[0], c[1], c[2], 255);
        const SDL_Rect swatch{bx + lpad, ry, sw, 7};
        (void)SDL_RenderFillRect(ren, &swatch);
        (void)std::snprintf(leg, sizeof leg, "s%llu", static_cast<unsigned long long>(s.serial));
        draw_text(ren, leg, bx + lpad + sw + 6, ry, 1, 236, 240, 241);
        ++row;
      }
      ++lcolor;
    }
    if (hidden > 0) {
      const int ry = by + lpad + static_cast<int>(shown) * row_h + 2;
      (void)std::snprintf(leg, sizeof leg, "+%llu more", static_cast<unsigned long long>(hidden));
      draw_text(ren, leg, bx + lpad + sw + 6, ry, 1, 150, 156, 170);
    }
  }

  (void)samples; // reserved for a future overlay readout
  SDL_RenderPresent(ren);
}

} // namespace

bool PlotWindow::supported() noexcept { return true; }

bool PlotWindow::open(std::string_view title) {
  if (window_ != nullptr) {
    return true;
  }
  close();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    return false;
  }
  const std::string name(title.empty() ? "sig run --plot" : title);
  SDL_Window *win = SDL_CreateWindow(name.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (win == nullptr) {
    SDL_Quit();
    return false;
  }
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (ren == nullptr) {
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  }
  if (ren == nullptr) {
    SDL_DestroyWindow(win);
    SDL_Quit();
    return false;
  }
  SeriesVec *vec = nullptr;
  try {
    vec = new SeriesVec();
  } catch (...) {
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return false;
  }
  window_ = win;
  renderer_ = ren;
  series_ = vec;
  quit_ = false;
  samples_ = 0;
  last_draw_ms_ = 0;
  return true;
}

void PlotWindow::close() noexcept {
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(static_cast<SDL_Renderer *>(renderer_));
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(static_cast<SDL_Window *>(window_));
    window_ = nullptr;
    SDL_Quit();
  }
  delete static_cast<SeriesVec *>(series_);
  series_ = nullptr;
  quit_ = false;
}

void PlotWindow::push(double sim_time, const std::uint64_t *serials, const double *values, std::size_t n) noexcept {
  if (window_ == nullptr || serials == nullptr || values == nullptr) {
    return;
  }
  try {
    auto *vec = static_cast<SeriesVec *>(series_);
    if (vec == nullptr) {
      return;
    }
    for (std::size_t k = 0; k < n; ++k) {
      const std::uint64_t id = serials[k];
      PlotSeries *found = nullptr;
      for (auto &s : *vec) {
        if (s.serial == id) {
          found = &s;
          break;
        }
      }
      if (found == nullptr) {
        if (vec->size() >= kMaxSeries) {
          continue; // over the drawing bound: ignore (never grows here)
        }
        vec->push_back(PlotSeries{});
        vec->back().serial = id;
        found = &vec->back();
      }
      found->t.push_back(sim_time);
      found->v.push_back(values[k]);
      while (found->t.size() > kHistory) {
        found->t.pop_front();
        found->v.pop_front();
      }
    }
    samples_ += static_cast<std::uint64_t>(n);
  } catch (...) {
    // Best-effort observer: allocation failure drops the tick, never throws.
  }
}

bool PlotWindow::poll() noexcept {
  if (window_ == nullptr || quit_) {
    return false;
  }
  SDL_Event ev;
  while (SDL_PollEvent(&ev) != 0) {
    if (ev.type == SDL_QUIT) {
      quit_ = true;
    } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) {
      quit_ = true;
    }
  }
  return !quit_;
}

void PlotWindow::render() noexcept {
  if (window_ == nullptr || quit_) {
    return;
  }
  const std::uint64_t now = SDL_GetTicks64();
  if (last_draw_ms_ != 0 && now - last_draw_ms_ < 33) {
    return; // ~30 Hz display cap (receive path keeps draining)
  }
  last_draw_ms_ = now;
  auto *vec = static_cast<SeriesVec *>(series_);
  auto *ren = static_cast<SDL_Renderer *>(renderer_);
  if (vec != nullptr && ren != nullptr) {
    draw_frame(ren, *vec, samples_);
  }
}

void PlotWindow::wait_frame() noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

#else // !WENGINE_HAVE_SDL2: dependency-free stub (CLI builds headless).

bool PlotWindow::supported() noexcept { return false; }

bool PlotWindow::open(std::string_view) { return false; }

void PlotWindow::close() noexcept {}

void PlotWindow::push(double, const std::uint64_t *, const double *, std::size_t) noexcept {}

bool PlotWindow::poll() noexcept { return false; }

void PlotWindow::render() noexcept {}

void PlotWindow::wait_frame() noexcept {}

#endif

} // namespace wengine::cli
