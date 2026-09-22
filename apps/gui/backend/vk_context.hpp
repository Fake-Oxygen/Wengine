#pragma once

// SDL2 + Vulkan bootstrap for the Dear ImGui backend.
//
// Owns the Vulkan instance, device, swapchain window state, and per-frame
// render/present. Extracted verbatim from the original single-TU main.cpp
// during the apps/gui directory reorganization; behavior is unchanged
// (same ImGui-example-derived sequence: no validation layers, FIFO present
// mode, default allocator).
//
// Main-loop integration: the SDL event pump calls request_rebuild() on
// resize, then queries the drawable size itself (SDL stays in main.cpp)
// and hands it to apply_rebuild(), which resizes or keeps the request
// pending when the size is not yet valid.

#include <cstdint>
#include <vulkan/vulkan.h>

struct ImDrawData;

namespace wengine::gui::backend {

void check_vk_result(VkResult err);

void setup_vulkan(const char *const *instance_extensions,
                  std::uint32_t instance_extension_count);
void setup_vulkan_window(VkSurfaceKHR surface, int width, int height);

// Resize handshake: request on SDL_WINDOWEVENT_RESIZED, then apply with
// the fresh drawable size once it is valid (non-zero).
void request_rebuild();
[[nodiscard]] bool rebuild_requested();
void apply_rebuild(int width, int height);

void set_clear_color(float r, float g, float b, float a);
void wait_idle();

struct VulkanHandles {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  std::uint32_t queue_family = UINT32_MAX;
  VkQueue queue = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::uint32_t min_image_count = 0;
  std::uint32_t image_count = 0;
};

[[nodiscard]] VulkanHandles handles();

void cleanup_vulkan_window();
void cleanup_vulkan();

void frame_render(ImDrawData *draw_data);
void frame_present();

} // namespace wengine::gui::backend
