// HRL Vulkan backend - backend table.
//
// The Vulkan backend executes the same renderer as the OpenGL 3.3 backend
// (see vk_renderer.cpp and vkgl/vkgl.cpp), so every feature, every pass and
// every decision is shared: only the GPU API underneath differs. This file
// adds what is specific to Vulkan: device/swapchain creation, frame
// begin/submit/present and the hrl_vulkan.h accessors.
#include "vulkan_backend.h"

#include "vk_core.h"
#include "vkgl/vkgl_runtime.h"
#include "../../core/utils_functions.h"

#include <string>

// Renderer entry points compiled from the OpenGL 3.3 sources (VKGL_ prefix).
HRL_vtable VKGL_GetOpenGL33Backend();
void VKGL_GL33_InitContext(HRL_uint width, HRL_uint height, void* loader);
void VKGL_GL33_Shutdown();
void VKGL_GL33_WindowResizeCallback(int width, int height);
void VKGL_GL33_ResetFramebuffer();

void VulkanSetSurfaceCallback(HRL_VulkanCreateSurfaceCallback callback, void* user_data)
{
	hrlvk::GetCore().surfaceCallback = callback;
	hrlvk::GetCore().surfaceUser = user_data;
}

void VulkanSetInstanceExtensions(const char* const* extensions, HRL_uint extension_count)
{
	auto& list = hrlvk::GetCore().instanceExtensions;
	list.clear();
	for (HRL_uint i = 0; extensions && i < extension_count; ++i)
		if (extensions[i]) list.emplace_back(extensions[i]);
}

VkInstance VulkanGetInstance() { return hrlvk::GetCore().instance; }
VkPhysicalDevice VulkanGetPhysicalDevice() { return hrlvk::GetCore().physical; }
VkDevice VulkanGetDevice() { return hrlvk::GetCore().device; }
VkQueue VulkanGetGraphicsQueue() { return hrlvk::GetCore().queue; }
VkSurfaceKHR VulkanGetSurface() { return hrlvk::GetCore().surface; }

static void VK_Init() {}

static void VK_InitContext(HRL_uint width, HRL_uint height, void* loader)
{
	if (!vkgl::Init(width, height))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Vulkan: device initialization failed");
		return;
	}
	VKGL_GL33_InitContext(width, height, loader);
}

static void VK_Shutdown()
{
	if (!vkgl::IsInitialized()) return;
	VKGL_GL33_Shutdown();
	vkgl::Shutdown();
}

static void VK_BeginFrame()
{
	vkgl::BeginFrame();
}

static void VK_EndFrame()
{
	VKGL_GL33_ResetFramebuffer();
	vkgl::EndFrame();
}

static void VK_WindowResize(int width, int height)
{
	vkgl::Resize(width, height);
	VKGL_GL33_WindowResizeCallback(width, height);
}

HRL_vtable GetVulkanBackend()
{
	HRL_vtable vtable = VKGL_GetOpenGL33Backend();
	vtable.RHI_Init = VK_Init;
	vtable.RHI_InitContext = VK_InitContext;
	vtable.RHI_Shutdown = VK_Shutdown;
	vtable.RHI_BeginFrame = VK_BeginFrame;
	vtable.RHI_ResetFramebuffer = VK_EndFrame;
	vtable.RHI_WindowResizeCallback = VK_WindowResize;
	return vtable;
}
