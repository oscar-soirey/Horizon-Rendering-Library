// HRL Vulkan backend - runtime control of the OpenGL-on-Vulkan layer.
#ifndef HRL_VKGL_RUNTIME_H
#define HRL_VKGL_RUNTIME_H

#include <cstdint>

namespace vkgl
{
// Creates the Vulkan device (and the swapchain when a surface callback is set)
// plus the default framebuffer (window back buffer equivalent).
bool Init(uint32_t width, uint32_t height);
void Shutdown();
bool IsInitialized();

// Starts recording a frame if none is open.
void BeginFrame();
// Presents the default framebuffer (when a swapchain exists) and submits.
void EndFrame();
// Resizes the default framebuffer and the swapchain.
void Resize(int width, int height);
} // namespace vkgl

#endif
