#pragma once

// Live signal plot window for `sig run --plot` (app edge, CLI-owned).
//
// In-process 2D window (SDL2 software renderer when built with SDL2 —
// never the ImGui/Vulkan GUI backend, never an external app): every
// emitted tick is copied into a rolling history and drawn at a throttled
// display rate (~30 Hz). Observes only; simulation numerics, step timing,
// and determinism are untouched (wall-clock throttling never feeds back
// into the run). Window close is a step-boundary stop request, like
// Ctrl-C.
//
// Build contract (mirrors the TUI's optional Curses dep): with SDL2
// available the window opens; without it `supported()` is false and
// `open()` fails loudly so the caller reports a rebuild hint instead of
// crashing. No SDL types leak into this header, so callers and unit
// tests compile with or without SDL2.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wengine::cli {

class PlotWindow {
public:
  // Rolling samples kept per source serial (matches the Python plotter
  // default history); series beyond kMaxSeries are ignored for drawing
  // (the transport bound is 64 channels, so the full batch always fits).
  static constexpr std::size_t kHistory = 1024;
  static constexpr std::size_t kMaxSeries = 64;

  PlotWindow() = default;
  PlotWindow(const PlotWindow &) = delete;
  PlotWindow &operator=(const PlotWindow &) = delete;
  ~PlotWindow() { close(); }

  // True when built with SDL2 (window support compiled in).
  [[nodiscard]] static bool supported() noexcept;

  // Create the window. False when unsupported or no display is
  // available (headless CI, missing $DISPLAY) — never throws, never aborts.
  [[nodiscard]] bool open(std::string_view title);
  [[nodiscard]] bool is_open() const noexcept { return window_ != nullptr; }
  void close() noexcept;

  // Copy one emitted tick (sim time + dense serial/value arrays in slot
  // order, as in world::Snapshot). Allocation-bounded after warmup;
  // never throws (drops on allocation failure — best-effort observer).
  void push(double sim_time, const std::uint64_t *serials, const double *values, std::size_t n) noexcept;

  // Pump window events. Returns false once the user asked to quit
  // (close button / Alt+F4) — the driver treats it as a step-boundary
  // stop, like Ctrl-C. Never throws.
  [[nodiscard]] bool poll() noexcept;

  // Redraw when due (internally throttled to ~30 Hz wall-clock).
  // Never throws.
  void render() noexcept;

  // True once poll() observed a quit request.
  [[nodiscard]] bool should_close() const noexcept { return quit_; }

  // Total pushed sample rows (for the `plot:` summary line).
  [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }

  // Idle sleep for the hold-open loop (keeps CPU out of a busy spin).
  static void wait_frame() noexcept;

private:
  void *window_ = nullptr;   // SDL_Window (opaque: no SDL in this header)
  void *renderer_ = nullptr; // SDL_Renderer (opaque)
  bool quit_ = false;
  std::uint64_t samples_ = 0;
  std::uint64_t last_draw_ms_ = 0;
  // Flat series storage: serials[i] owns up to kHistory (t, v) pairs.
  // std::vector here keeps the header dependency-free; growth happens
  // only for new serials (batch serials are stable, so steady-state
  // push is allocation-free). The concrete element type lives in
  // src/cli/plot_sdl.cpp (file-local); this stays an opaque pointer.
  void *series_ = nullptr; // std::vector<PlotSeries>* (opaque)
};

} // namespace wengine::cli
