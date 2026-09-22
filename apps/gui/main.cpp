// WEngine GUI entry (Dear ImGui + SDL2 + Vulkan).
//
// Thin assembly only: argument parsing, SDL window lifetime, backend
// lifetime, the Dear ImGui frame loop, and top-level window layout.
// Feature code lives in its own directories:
//
//   backend/      SDL2 + Vulkan bootstrap (VulkanHandles, frame render)
//   controllers/  engine drivers with GUI-owned state (SimController)
//   models/       engine-only evaluate logic (closed-form solver models)
//   panels/       ImGui rendering (sim panel, closed-form panel registry)
//   views/        reserved: graphs, 2D canvas, 3D viewport land here
//
// Dependency direction: apps/gui -> wengine_core + wengine_imgui only.
// Build: cmake -S . -B build -DBUILD_GUI=ON && cmake --build build
// Exit codes mirror the CLI convention: 0 ok, 1 runtime failure,
// 2 usage error.

#include "backend/vk_context.hpp"
#include "panels/closed_form_panels.hpp"
#include "panels/sim_panel.hpp"
#include "views/canvas2d_panel.hpp"
#include "views/frame_source.hpp"
#include "views/graphs_panel.hpp"
#include "views/viewport3d_panel.hpp"
#include "wengine/core/status.hpp"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <cstdio>
#include <string_view>
#include <vector>

namespace {

void print_help() {
  std::printf("WEngineGui (Dear ImGui + SDL2 + Vulkan)\n"
              "Usage: WEngineGui [--help]\n"
              "Opens the engine GUI.\n");
}

} // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_help();
      return 0;
    }
    std::fprintf(stderr, "WEngineGui: unknown argument '%s'\n", argv[i]);
    print_help();
    return 2;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // SDL2 does not load the Vulkan loader lazily for the extension query:
  // the explicit LoadLibrary call must come first.
  if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
    std::fprintf(stderr, "SDL_Vulkan_LoadLibrary failed: %s\n",
                 SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // Instance extensions required by the SDL2 Vulkan backend.
  unsigned int sdl_ext_count = 0;
  if (SDL_Vulkan_GetInstanceExtensions(nullptr, &sdl_ext_count, nullptr) ==
      SDL_FALSE) {
    std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions count failed: %s\n",
                 SDL_GetError());
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }
  std::vector<const char *> extensions(sdl_ext_count);
  if (SDL_Vulkan_GetInstanceExtensions(
          nullptr, &sdl_ext_count,
          sdl_ext_count > 0 ? extensions.data() : nullptr) == SDL_FALSE) {
    std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions failed: %s\n",
                 SDL_GetError());
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }
  wengine::gui::backend::setup_vulkan(extensions.data(), sdl_ext_count);

  SDL_Window *window = SDL_CreateWindow(
      "WEngine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    wengine::gui::backend::cleanup_vulkan();
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (SDL_Vulkan_CreateSurface(window, wengine::gui::backend::handles().instance,
                               &surface) == SDL_FALSE) {
    std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed\n");
    SDL_DestroyWindow(window);
    wengine::gui::backend::cleanup_vulkan();
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }
  int draw_w = 0;
  int draw_h = 0;
  SDL_Vulkan_GetDrawableSize(window, &draw_w, &draw_h);
  wengine::gui::backend::setup_vulkan_window(surface, draw_w, draw_h);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr; // no persistent layout; a later task may point
                            // this at an XDG config path.
  ImGui::StyleColorsDark();

  const wengine::gui::backend::VulkanHandles vk =
      wengine::gui::backend::handles();
  ImGui_ImplSDL2_InitForVulkan(window);
  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.Instance = vk.instance;
  init_info.PhysicalDevice = vk.physical_device;
  init_info.Device = vk.device;
  init_info.QueueFamily = vk.queue_family;
  init_info.Queue = vk.queue;
  init_info.PipelineCache = vk.pipeline_cache;
  init_info.DescriptorPool = vk.descriptor_pool;
  init_info.RenderPass = vk.render_pass;
  init_info.Subpass = 0;
  init_info.MinImageCount = vk.min_image_count;
  init_info.ImageCount = vk.image_count;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.Allocator = nullptr;
  init_info.CheckVkResultFn = wengine::gui::backend::check_vk_result;
  ImGui_ImplVulkan_Init(&init_info);
  ImGui_ImplVulkan_CreateFontsTexture();

  wengine::gui::SimController sim;
  wengine::gui::SimPanel sim_panel;
  sim_panel.sync_from(sim.config());
  wengine::gui::Viewport3dPanel viewport3d;
  wengine::gui::GraphsPanel graphs;
  wengine::gui::Canvas2dPanel canvas2d;
  // Shared frame stream: the recorder adapts every emitted snapshot
  // through the solver registry; views read link::Frames only.
  wengine::gui::FrameHistory frames;
  wengine::gui::FrameRecorder recorder(frames);
  sim.add_sink(recorder);

  wengine::gui::backend::set_clear_color(0.10F, 0.12F, 0.16F, 1.00F);

  bool done = false;
  while (!done) {
    SDL_Event event{};
    while (SDL_PollEvent(&event) != 0) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        done = true;
      } else if (event.type == SDL_WINDOWEVENT &&
                 event.window.windowID == SDL_GetWindowID(window)) {
        if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
          done = true;
        } else if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
          SDL_Delay(10);
        } else if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
          wengine::gui::backend::request_rebuild();
        }
      }
    }
    if (wengine::gui::backend::rebuild_requested()) {
      int next_w = 0;
      int next_h = 0;
      SDL_Vulkan_GetDrawableSize(window, &next_w, &next_h);
      wengine::gui::backend::apply_rebuild(next_w, next_h);
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Play mode folds wall-clock time into fixed steps; the controller
    // auto-pauses at the step budget, so Reset + Play matches headless.
    if (sim.playing()) {
      const wengine::core::Status advance_st =
          sim.advance_frame(static_cast<double>(io.DeltaTime));
      if (!advance_st.ok()) {
        sim.set_playing(false);
      }
    }

    // Adapt once per frame at the engine/GUI boundary: every view below
    // consumes link::Frames, never Snapshots.
    recorder.set_solver(sim.config().solver);
    const wengine::link::Frame frame =
        wengine::gui::adapt_frame(sim.snapshot(), sim.config().solver);

    // Startup layout (1600x900): sim + live views open, calculators and
    // plots collapsed one click away, room for future panels on the right.
    ImGui::SetNextWindowPos(ImVec2(20.0F, 20.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("WEngine");
    ImGui::Text("Backend: Dear ImGui v1.91.9b + SDL2 + Vulkan");
    ImGui::Text("Framerate: %.1f fps", static_cast<double>(io.Framerate));
    sim_panel.draw(sim);
    sim_panel.draw_status(sim, frame);
    ImGui::End();

    // No ini file (IniFilename=nullptr): place once per launch, no overlap.
    ImGui::SetNextWindowPos(ImVec2(880.0F, 20.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    ImGui::Begin("Solvers");
    if (ImGui::BeginTabBar("ClosedForm")) {
      for (const auto &panel : wengine::gui::kClosedFormPanels) {
        // Registry names are string literals: NUL-terminated and stable.
        if (ImGui::BeginTabItem(panel.name.data())) {
          panel.draw();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::End();

    // Frame-driven and read-only: cameras cannot perturb the sim.
    viewport3d.draw(frame);

    // Observer-fed series: every emitted snapshot appends points, pausing
    // freezes the plots without touching sim timing.
    graphs.draw(frames);

    // Top-down world view with trails from the shared history.
    canvas2d.draw(frame, frames);

    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    const bool minimized =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0U;
    if (!minimized) {
      wengine::gui::backend::frame_render(draw_data);
      wengine::gui::backend::frame_present();
    }
  }

  wengine::gui::backend::wait_idle();
  ImGui_ImplVulkan_DestroyFontsTexture();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  // Note: cleanup_vulkan_window() (ImGui_ImplVulkanH_DestroyWindow) already
  // destroys the surface; destroying it again here would double-free.
  wengine::gui::backend::cleanup_vulkan_window();
  wengine::gui::backend::cleanup_vulkan();
  SDL_DestroyWindow(window);
  SDL_Vulkan_UnloadLibrary();
  SDL_Quit();
  return 0;
}
