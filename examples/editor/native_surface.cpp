#include "native_surface.h"

#include <vulkan/vulkan_core.h>
#if defined(_WIN32)
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#elif !defined(__APPLE__)
#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
struct wl_display;
struct wl_surface;
#include <vulkan/vulkan_wayland.h>
#endif

const char* const* EditorSurfaceExtensions(const EditorNativeWindow& native, std::uint32_t* count)
{
#if defined(_WIN32)
	static const char* const win32[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
	(void)native;
	*count = 2;
	return win32;
#elif !defined(__APPLE__)
	static const char* const x11[] = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
	static const char* const wayland[] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
	*count = 2;
	return native.system == EditorNativeWindow::Wayland ? wayland : x11;
#else
	(void)native;
	*count = 0;
	return nullptr;
#endif
}

int EditorCreateSurface(void* instance, void* surfaceOut, void* user)
{
	const EditorNativeWindow* native = static_cast<const EditorNativeWindow*>(user);
	VkInstance vkInstance = static_cast<VkInstance>(instance);
	VkSurfaceKHR* surface = static_cast<VkSurfaceKHR*>(surfaceOut);
#if defined(_WIN32)
	VkWin32SurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	info.hinstance = native->display ? static_cast<HINSTANCE>(native->display) : GetModuleHandleW(nullptr);
	info.hwnd = reinterpret_cast<HWND>(native->window);
	return static_cast<int>(vkCreateWin32SurfaceKHR(vkInstance, &info, nullptr, surface));
#elif !defined(__APPLE__)
	if (native->system == EditorNativeWindow::Wayland)
	{
		VkWaylandSurfaceCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
		info.display = static_cast<wl_display*>(native->display);
		info.surface = reinterpret_cast<wl_surface*>(native->window);
		return static_cast<int>(vkCreateWaylandSurfaceKHR(vkInstance, &info, nullptr, surface));
	}
	VkXlibSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
	info.dpy = static_cast<Display*>(native->display);
	info.window = static_cast<Window>(native->window);
	return static_cast<int>(vkCreateXlibSurfaceKHR(vkInstance, &info, nullptr, surface));
#else
	(void)native; (void)vkInstance; (void)surface;
	return static_cast<int>(VK_ERROR_EXTENSION_NOT_PRESENT);
#endif
}
