// HRL editor example: Vulkan surface on the native child window of a Raven
// viewport. Kept in its own translation unit so that <windows.h> / <X11/Xlib.h>
// and their macros stay out of the editor code.
#pragma once
#include <cstdint>

struct EditorNativeWindow
{
	enum System { None, Win32, X11, Wayland } system = None;
	void* display = nullptr;      // HINSTANCE / Display* / wl_display*
	std::uintptr_t window = 0;    // HWND / X11 Window / wl_surface*
};

// Vulkan instance extensions needed to present on that window (static strings).
const char* const* EditorSurfaceExtensions(const EditorNativeWindow& native, std::uint32_t* count);

// HRL_VulkanCreateSurfaceCallback: `user` points to an EditorNativeWindow.
int EditorCreateSurface(void* instance, void* surfaceOut, void* user);
