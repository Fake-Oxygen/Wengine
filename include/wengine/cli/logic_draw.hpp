#pragma once

/**
 * @file
 * Standalone schematic raster for `logic draw` / `logic export --bmp`
 * (app edge, CLI-owned). Two independent paths share the pure layout
 * math in logic/layout.hpp (header-only, SDL-free): the interactive
 * window (show_schematic: Dear ImGui canvas over the SDL2 2D renderer -
 * GPU-anti-aliased vectors, TTF gate labels, wheel-zoom plus drag-pan,
 * independent of the WEngineGui ImGui/Vulkan backend and its panels) and
 * the file export (write_bmp: SDL software surface plus SDL_SaveBMP, no
 * new deps; PNG/JPG codecs stay deferred). The window needs an SDL2 plus
 * imgui build and a display (otherwise window_supported() is false and
 * the verb fails loudly with a rebuild hint; close means stop, like
 * `sig run --plot`); the export is headless-safe (off-screen surface, no
 * window, no display; SDL2 build only). No SDL or ImGui types leak into
 * this header, so callers and unit tests compile with or without either
 * backend. Consumers: the logic draw/export verbs.
 */

// Standalone schematic raster for `logic draw` / `logic export --bmp`
// (LOGIC-7, app edge, CLI-owned).
//
// Two independent paths share the pure layout math in
// `wengine/logic/layout.hpp` (header-only, SDL-free):
//
// - Interactive window (`show_schematic`): Dear ImGui canvas over the
//   SDL2 2D renderer — GPU-anti-aliased vectors, TTF gate labels,
//   wheel-zoom + drag-pan. Independent of the WEngineGui (ImGui/Vulkan)
//   backend and of its panels. Needs an SDL2 + imgui build and a
//   display; otherwise `window_supported()` is false and the verb fails
//   loudly with a rebuild hint. Close == stop (like `sig run --plot`).
// - File export (`write_bmp`): SDL software surface + SDL_SaveBMP
//   (zero new deps; PNG/JPG codecs stay deferred per docs/logic.md
//   §12). Headless-safe: renders onto an off-screen surface, no window,
//   no display. Needs an SDL2 build only.
//
// No SDL or ImGui types leak into this header, so callers and unit tests
// compile with or without either backend.

#include <string>

#include "wengine/logic/layout.hpp"
#include "wengine/logic/netlist.hpp"

namespace wengine::cli::logic_draw {

// True when built with SDL2 (BMP export supported).
/**
 * @brief Whether BMP export support is compiled in.
 * @return True with an SDL2 build (write_bmp available); false otherwise.
 */
[[nodiscard]] bool supported() noexcept;

// True when built with SDL2 + the vendored imgui canvas (interactive
// window supported).
/**
 * @brief Whether interactive schematic windows are available.
 * @return True with an SDL2 plus imgui-canvas build; false otherwise
 *         (the verb then fails loudly with a rebuild hint).
 */
[[nodiscard]] bool window_supported() noexcept;

// Open the interactive schematic window. Blocks until the user closes it
// (close == stop). False when unsupported or no display is available
// (headless CI, missing $DISPLAY) — never throws, never aborts. `err`
// names the cause.
/**
 * @brief Open the interactive schematic window (blocks until closed).
 * @param[in] nl Netlist to draw (read-only; never mutated).
 * @param[in] sc Layered schematic for the netlist (read-only).
 * @param[out] err Cause text when returning false.
 * @return True while the window ran; false when unsupported or no
 *         display is available (headless CI, missing display) - never
 *         throws, never aborts. Close means stop, like `sig run --plot`.
 */
bool show_schematic(const wengine::logic::Netlist &nl, const wengine::logic::Schematic &sc, std::string &err);

// Write the schematic to a BMP file via SDL_SaveBMP (zero new deps; PNG/JPG
// codecs stay deferred per docs/logic.md §12). Headless-safe: renders onto
// an off-screen surface, no window, no display. `w_out`/`h_out` receive
// the pixel dimensions. False on SDL errors — never throws.
/**
 * @brief Write the schematic to a BMP file via SDL_SaveBMP.
 * @param[in] path Destination file path.
 * @param[in] nl Netlist to draw (read-only; never mutated).
 * @param[in] sc Layered schematic for the netlist (read-only).
 * @param[out] w_out Image width in pixels.
 * @param[out] h_out Image height in pixels.
 * @param[out] err Cause text when returning false.
 * @return True on success; false on SDL errors - never throws.
 * @note Headless-safe: renders onto an off-screen surface (no window, no
 *       display). Needs an SDL2 build only; PNG/JPG codecs stay deferred.
 */
bool write_bmp(const std::string &path, const wengine::logic::Netlist &nl,
               const wengine::logic::Schematic &sc, int &w_out, int &h_out, std::string &err);

} // namespace wengine::cli::logic_draw
