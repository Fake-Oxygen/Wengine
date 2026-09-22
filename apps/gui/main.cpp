// WEngine GUI entry (PHYSI-13: Dear ImGui + SDL2 + Vulkan).
//
// Blank backend application: opens an SDL2 window with a Vulkan swapchain,
// runs the Dear ImGui frame loop, and shows engine-seam status. Solver and
// simulation panels arrive in follow-up tasks; this TU owns all Vulkan/SDL
// lifetime and never leaks frontend types into the engine.
//
// Vulkan bootstrap below follows the structure of Dear ImGui's
// examples/example_sdl2_vulkan/main.cpp (MIT, (c) Omar Cornut et al.),
// trimmed to the scaffold: no validation layers, FIFO present mode, default
// allocator.
//
// Dependency direction: apps/gui -> wengine_core + wengine_imgui only.
// Build: cmake -S . -B build -DBUILD_GUI=ON && cmake --build build
// Exit codes mirror the CLI convention: 0 ok, 1 runtime failure,
// 2 usage error.

#include "wengine/core/run_config.hpp"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

// ------------------------------------------------------------------
// Vulkan state (scaffold-owned, function-local where possible)
// ------------------------------------------------------------------

namespace {

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical_device = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
std::uint32_t g_queue_family = UINT32_MAX;
VkQueue g_queue = VK_NULL_HANDLE;
VkPipelineCache g_pipeline_cache = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
ImGui_ImplVulkanH_Window g_main_window_data{};
std::uint32_t g_min_image_count = 2;
bool g_swapchain_rebuild = false;

void check_vk_result(VkResult err) {
  if (err != VK_SUCCESS) {
    std::fprintf(stderr, "[vulkan] VkResult=%d\n", err);
    if (err < VK_SUCCESS) {
      std::abort();
    }
  }
}

void setup_vulkan(const char *const *instance_extensions,
                  std::uint32_t instance_extension_count) {
  // Create Vulkan instance without validation layers (scaffold keeps the
  // layer-free path; layers become a dev-mode follow-up).
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.enabledExtensionCount = instance_extension_count;
  create_info.ppEnabledExtensionNames = instance_extensions;
  check_vk_result(vkCreateInstance(&create_info, nullptr, &g_instance));

  // Enumerate GPUs and pick a discrete one when available.
  std::uint32_t gpu_count = 0;
  check_vk_result(
      vkEnumeratePhysicalDevices(g_instance, &gpu_count, nullptr));
  if (gpu_count == 0) {
    std::fprintf(stderr, "[vulkan] no GPUs with Vulkan support\n");
    std::abort();
  }
  std::vector<VkPhysicalDevice> gpus(gpu_count);
  check_vk_result(
      vkEnumeratePhysicalDevices(g_instance, &gpu_count, gpus.data()));

  std::uint32_t use_gpu = 0;
  for (std::uint32_t i = 0; i < gpu_count; ++i) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpus[i], &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      use_gpu = i;
      break;
    }
  }
  g_physical_device = gpus[use_gpu];
  VkPhysicalDeviceProperties chosen_props{};
  vkGetPhysicalDeviceProperties(g_physical_device, &chosen_props);
  std::fprintf(stderr, "[vulkan] GPU: %s\n", chosen_props.deviceName);

  // Find a queue family with graphics support.
  std::uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g_physical_device, &count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  vkGetPhysicalDeviceQueueFamilyProperties(g_physical_device, &count,
                                           queues.data());
  for (std::uint32_t i = 0; i < count; ++i) {
    if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      g_queue_family = i;
      break;
    }
  }
  if (g_queue_family == UINT32_MAX) {
    std::fprintf(stderr, "[vulkan] no graphics queue family\n");
    std::abort();
  }

  // Logical device with the swapchain extension.
  std::vector<const char *> device_extensions;
  device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  const float queue_priority = 1.0F;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = g_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &queue_priority;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<std::uint32_t>(device_extensions.size());
  device_info.ppEnabledExtensionNames = device_extensions.data();
  check_vk_result(
      vkCreateDevice(g_physical_device, &device_info, nullptr, &g_device));
  vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

  // Descriptor pool sized for Dear ImGui's backend.
  constexpr std::uint32_t kPoolSize = 1000;
  const VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kPoolSize},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kPoolSize},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kPoolSize},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kPoolSize},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, kPoolSize},
      {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, kPoolSize}};
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = kPoolSize * 11U;
  pool_info.poolSizeCount =
      static_cast<std::uint32_t>(sizeof(pool_sizes) / sizeof(pool_sizes[0]));
  pool_info.pPoolSizes = pool_sizes;
  check_vk_result(
      vkCreateDescriptorPool(g_device, &pool_info, nullptr, &g_descriptor_pool));
}

void setup_vulkan_window(VkSurfaceKHR surface, int width, int height) {
  g_main_window_data.Surface = surface;

  // Surface format selection.
  VkBool32 present_supported = VK_FALSE;
  check_vk_result(vkGetPhysicalDeviceSurfaceSupportKHR(
      g_physical_device, g_queue_family, g_main_window_data.Surface,
      &present_supported));
  if (present_supported != VK_TRUE) {
    std::fprintf(stderr, "[vulkan] surface not supported by queue\n");
    std::abort();
  }
  const VkFormat request_formats[] = {VK_FORMAT_B8G8R8A8_UNORM,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_FORMAT_B8G8R8_UNORM,
                                      VK_FORMAT_R8G8B8_UNORM};
  const VkColorSpaceKHR request_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
  g_main_window_data.SurfaceFormat =
      ImGui_ImplVulkanH_SelectSurfaceFormat(
          g_physical_device, g_main_window_data.Surface, request_formats,
          static_cast<std::uint32_t>(sizeof(request_formats) /
                                     sizeof(request_formats[0])),
          request_color_space);
  const VkPresentModeKHR request_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
  g_main_window_data.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
      g_physical_device, g_main_window_data.Surface, request_modes,
      static_cast<std::uint32_t>(sizeof(request_modes) /
                                 sizeof(request_modes[0])));

  // Swapchain + render pass, sized to the drawable area.
  ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_physical_device,
                                         g_device, &g_main_window_data,
                                         g_queue_family, nullptr, width, height,
                                         g_min_image_count);
}

void cleanup_vulkan_window() {
  ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device, &g_main_window_data,
                                  nullptr);
}

void cleanup_vulkan() {
  vkDestroyDescriptorPool(g_device, g_descriptor_pool, nullptr);
  vkDestroyDevice(g_device, nullptr);
  vkDestroyInstance(g_instance, nullptr);
  g_instance = VK_NULL_HANDLE;
  g_physical_device = VK_NULL_HANDLE;
  g_device = VK_NULL_HANDLE;
  g_queue = VK_NULL_HANDLE;
  g_queue_family = UINT32_MAX;
  g_descriptor_pool = VK_NULL_HANDLE;
}

void frame_render(ImDrawData *draw_data) {
  ImGui_ImplVulkanH_Window *wd = &g_main_window_data;
  VkSemaphore image_acquired =
      wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
  VkSemaphore render_complete =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkResult err = vkAcquireNextImageKHR(g_device, wd->Swapchain, UINT64_MAX,
                                       image_acquired, VK_NULL_HANDLE,
                                       &wd->FrameIndex);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    g_swapchain_rebuild = true;
    return;
  }
  check_vk_result(err);

  ImGui_ImplVulkanH_Frame *fd = &wd->Frames[wd->FrameIndex];
  check_vk_result(
      vkWaitForFences(g_device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
  check_vk_result(vkResetFences(g_device, 1, &fd->Fence));
  check_vk_result(vkResetCommandPool(g_device, fd->CommandPool, 0));

  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &begin_info));

  VkRenderPassBeginInfo render_begin{};
  render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_begin.renderPass = wd->RenderPass;
  render_begin.framebuffer = fd->Framebuffer;
  render_begin.renderArea.extent.width = static_cast<std::uint32_t>(wd->Width);
  render_begin.renderArea.extent.height =
      static_cast<std::uint32_t>(wd->Height);
  render_begin.clearValueCount = 1;
  render_begin.pClearValues = &wd->ClearValue;
  vkCmdBeginRenderPass(fd->CommandBuffer, &render_begin,
                       VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
  vkCmdEndRenderPass(fd->CommandBuffer);
  check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &image_acquired;
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &fd->CommandBuffer;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_complete;
  check_vk_result(vkQueueSubmit(g_queue, 1, &submit, fd->Fence));
}

void frame_present() {
  if (g_swapchain_rebuild) {
    return;
  }
  ImGui_ImplVulkanH_Window *wd = &g_main_window_data;
  VkSemaphore render_complete =
      wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_complete;
  present.swapchainCount = 1;
  present.pSwapchains = &wd->Swapchain;
  present.pImageIndices = &wd->FrameIndex;
  VkResult err = vkQueuePresentKHR(g_queue, &present);
  if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
    g_swapchain_rebuild = true;
    return;
  }
  check_vk_result(err);
  wd->SemaphoreIndex = (wd->SemaphoreIndex + 1U) % wd->ImageCount;
}

void print_help() {
  std::printf("WEngineGui (Dear ImGui + SDL2 + Vulkan)\n"
              "Usage: WEngineGui [--help]\n"
              "Opens the engine GUI. Solver and simulation panels are tracked\n"
              "as follow-up tasks under the GUI epic.\n");
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
  setup_vulkan(extensions.data(), sdl_ext_count);

  SDL_Window *window = SDL_CreateWindow(
      "WEngine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    cleanup_vulkan();
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (SDL_Vulkan_CreateSurface(window, g_instance, &surface) == SDL_FALSE) {
    std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed\n");
    SDL_DestroyWindow(window);
    cleanup_vulkan();
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 1;
  }
  int draw_w = 0;
  int draw_h = 0;
  SDL_Vulkan_GetDrawableSize(window, &draw_w, &draw_h);
  setup_vulkan_window(surface, draw_w, draw_h);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr; // scaffold keeps no persistent layout; later
                            // tasks may point this at an XDG config path.
  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForVulkan(window);
  ImGui_ImplVulkan_InitInfo init_info{};
  init_info.Instance = g_instance;
  init_info.PhysicalDevice = g_physical_device;
  init_info.Device = g_device;
  init_info.QueueFamily = g_queue_family;
  init_info.Queue = g_queue;
  init_info.PipelineCache = g_pipeline_cache;
  init_info.DescriptorPool = g_descriptor_pool;
  init_info.RenderPass = g_main_window_data.RenderPass;
  init_info.Subpass = 0;
  init_info.MinImageCount = g_min_image_count;
  init_info.ImageCount = g_main_window_data.ImageCount;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.Allocator = nullptr;
  init_info.CheckVkResultFn = check_vk_result;
  ImGui_ImplVulkan_Init(&init_info);
  ImGui_ImplVulkan_CreateFontsTexture();

  const wengine::core::RunConfig defaults{};
  const char *solver_name =
      defaults.solver.empty() ? "(none)" : defaults.solver.data();

  const ImVec4 clear_color =
      ImVec4(0.10F, 0.12F, 0.16F, 1.00F);
  g_main_window_data.ClearValue.color.float32[0] = clear_color.x;
  g_main_window_data.ClearValue.color.float32[1] = clear_color.y;
  g_main_window_data.ClearValue.color.float32[2] = clear_color.z;
  g_main_window_data.ClearValue.color.float32[3] = clear_color.w;

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
          g_swapchain_rebuild = true;
        }
      }
    }
    if (g_swapchain_rebuild) {
      int next_w = 0;
      int next_h = 0;
      SDL_Vulkan_GetDrawableSize(window, &next_w, &next_h);
      if (next_w > 0 && next_h > 0) {
        ImGui_ImplVulkan_SetMinImageCount(g_min_image_count);
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            g_instance, g_physical_device, g_device, &g_main_window_data,
            g_queue_family, nullptr, next_w, next_h, g_min_image_count);
        g_main_window_data.FrameIndex = 0;
        g_swapchain_rebuild = false;
      }
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("WEngine");
    ImGui::Text("Backend: Dear ImGui v1.91.9b + SDL2 + Vulkan");
    ImGui::Text("Framerate: %.1f fps", static_cast<double>(io.Framerate));
    ImGui::Separator();
    ImGui::Text("Engine seam: core::RunConfig defaults");
    ImGui::Text("dt=%.4fs steps=%llu particles=%llu", defaults.loop.dt,
                static_cast<unsigned long long>(defaults.steps),
                static_cast<unsigned long long>(defaults.particles));
    ImGui::Text("solver=%s", solver_name);
    ImGui::Separator();
    ImGui::TextWrapped("Solver and simulation panels arrive with the "
                       "GUI follow-up tasks.");
    ImGui::End();

    ImGui::Render();
    ImDrawData *draw_data = ImGui::GetDrawData();
    const bool minimized =
        (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0U;
    if (!minimized) {
      frame_render(draw_data);
      frame_present();
    }
  }

  check_vk_result(vkDeviceWaitIdle(g_device));
  ImGui_ImplVulkan_DestroyFontsTexture();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  // Note: cleanup_vulkan_window() (ImGui_ImplVulkanH_DestroyWindow) already
  // destroys the surface; destroying it again here would double-free.
  cleanup_vulkan_window();
  cleanup_vulkan();
  SDL_DestroyWindow(window);
  SDL_Vulkan_UnloadLibrary();
  SDL_Quit();
  return 0;
}
