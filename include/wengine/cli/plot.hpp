#pragma once

/**
 * @file
 * Live signal plot window for `sig run --plot` (app edge, CLI-owned). An
 * in-process 2D window (SDL2 software renderer when built with SDL2 -
 * never the ImGui/Vulkan GUI backend, never an external app): every
 * emitted tick is copied into a rolling history and drawn at a throttled
 * display rate (about 30 Hz). The plotter observes only: simulation
 * numerics, step timing, and determinism are untouched (wall-clock
 * throttling never feeds back into the run). Window close is a
 * step-boundary stop request, like Ctrl-C. Build contract (mirrors the
 * optional Curses dep): with SDL2 the window opens; without it
 * supported() is false and open() fails loudly so the caller reports a
 * rebuild hint instead of crashing. No SDL types leak into this header,
 * so callers and unit tests compile with or without SDL2. Consumers: the
 * `sig run` batch driver (plot mode) and its summary line.
 */

/**
 * @file
 * Live signal plot window for `sig run --plot` (app edge, CLI-owned):
 * an in-process 2D SDL2 window - never the ImGui/Vulkan GUI backend,
 * never an external app - that copies every emitted tick into a rolling
 * history and draws at a throttled display rate (~30 Hz). Observes
 * only: simulation numerics, step timing, and determinism are
 * untouched (wall-clock throttling never feeds back into the run);
 * window close is a step-boundary stop request, like Ctrl-C. Build
 * contract mirrors the TUI's optional Curses dep: with SDL2 available
 * the window opens; without it supported() is false and open() fails
 * loudly so the caller reports a rebuild hint instead of crashing. No
 * SDL types leak into this header, so callers and unit tests compile
 * with or without SDL2. Consumers: the `sig run` command and its
 * hold-open loop; windowed verification is manual (screenshots).
 */

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

/**
 * @brief Rolling-history plot window driven by emitted ticks.
 * @note Observer only: never mutates simulation state, never perturbs
 *       step timing (the driver calls push/poll/render between
 *       steps). Not copyable (owns the SDL window); moves are not
 *       needed (one window per process).
 */
class PlotWindow {
public:
  // Rolling samples kept per source serial (matches the Python plotter
  // default history); series beyond kMaxSeries are ignored for drawing
  // (the transport bound is 64 channels, so the full batch always fits).
  /**
   * @brief Rolling samples kept per source serial (matches the Python
   *        plotter default history).
   */
  static constexpr std::size_t kHistory = 1024;
  /**
   * @brief Series cap for drawing (the transport bound is 64 channels,
   *        so a full batch always fits).
   */
  static constexpr std::size_t kMaxSeries = 64;

  /**
   * @brief Construct a closed, inert window (call open() to create it).
   */
  PlotWindow() = default;
  /**
   * @brief Copy construction is deleted: the window owns SDL handles
   *        and series storage.
   */
  PlotWindow(const PlotWindow &) = delete;
  /**
   * @brief Copy assignment is deleted (same reason as the copy).
   */
  PlotWindow &operator=(const PlotWindow &) = delete;
  /**
   * @brief Destructor: closes the window if still open.
   */
  ~PlotWindow() { close(); }

  // True when built with SDL2 (window support compiled in).
  /**
   * @brief Whether the window backend is compiled in (SDL2 build).
   * @return True when SDL2 support was compiled in; false means open()
   *         will fail and the caller should print a rebuild hint.
   */
  [[nodiscard]] static bool supported() noexcept;

  // Create the window. False when unsupported or no display is
  // available (headless CI, missing $DISPLAY) — never throws, never aborts.
  /**
   * @brief Create the plot window.
   * @param[in] title Window title (copied by SDL).
   * @return True on success; false when unsupported or no display is
   *         available (headless CI, missing $DISPLAY) - never throws,
   *         never aborts.
   */
  [[nodiscard]] bool open(std::string_view title);
  /**
   * @brief Whether the window is currently open.
   * @return True between a successful open() and close().
   */
  [[nodiscard]] bool is_open() const noexcept { return window_ != nullptr; }
  /**
   * @brief Close the window and release its resources (idempotent).
   */
  void close() noexcept;

  // Copy one emitted tick (sim time + dense serial/value arrays in slot
  // order, as in world::Snapshot). Allocation-bounded after warmup;
  // never throws (drops on allocation failure — best-effort observer).
  /**
   * @brief Copy one emitted tick into the rolling history.
   * @param[in] sim_time Engine simulation time in seconds (SI).
   * @param[in] serials Dense serial array in slot order (as in
   *                   world::Snapshot).
   * @param[in] values SI values in the same order as serials.
   * @param[in] n Number of (serial, value) rows in this tick.
   * @note Allocation-bounded after warmup; never throws - an allocation
   *       failure drops the tick (best-effort observer, never a sim
   *       failure).
   */
  void push(double sim_time, const std::uint64_t *serials, const double *values, std::size_t n) noexcept;

  // Pump window events. Returns false once the user asked to quit
  // (close button / Alt+F4) — the driver treats it as a step-boundary
  // stop, like Ctrl-C. Never throws.
  /**
   * @brief Pump window events once.
   * @return False once the user asked to quit (close button / Alt+F4) -
   *         the driver treats it as a step-boundary stop, like Ctrl-C.
   *         Never throws.
   */
  [[nodiscard]] bool poll() noexcept;

  // Redraw when due (internally throttled to ~30 Hz wall-clock).
  // Never throws.
  /**
   * @brief Redraw when due (internally throttled to ~30 Hz wall-clock).
   * @note Never throws; display throttling is a UI clock, independent
   *       of sim dt and packet rate (see docs/delivery.md).
   */
  void render() noexcept;

  // True once poll() observed a quit request.
  /**
   * @brief Whether a quit request was observed.
   * @return True once poll() saw the user ask to quit (latched).
   */
  [[nodiscard]] bool should_close() const noexcept { return quit_; }

  // Total pushed sample rows (for the `plot:` summary line).
  /**
   * @brief Total pushed sample rows.
   * @return Row count across all ticks (for the `plot:` summary line).
   */
  [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }

  // Idle sleep for the hold-open loop (keeps CPU out of a busy spin).
  /**
   * @brief Idle sleep for the hold-open loop (keeps CPU out of a busy
   *        spin while a finished run parks its final frame).
   */
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
