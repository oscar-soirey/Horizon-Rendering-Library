/**
 * Copyright (c) 2025-2026 Oscar Soirey
 * https://github.com/oscar-soirey/Horizon-Rendering-Library
 *
 * Vulkan-specific public API for HRL.
 *
 * IMPORTANT: this header intentionally does not include the Vulkan SDK (or any
 * other third-party header). Vulkan types remain private to the implementation.
 */
#ifndef HRL_VULKAN_IMPL
#define HRL_VULKAN_IMPL

#include "hrl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque callback used by HRL to create the presentation surface for the
 * application's windowing system.
 *
 * `instance` is the Vulkan VkInstance represented as an opaque pointer.
 * `surface_out` points to storage for a Vulkan VkSurfaceKHR handle.
 * The callback returns the Vulkan VkResult value as an int.
 *
 * The application can cast these opaque values back to Vulkan types in the
 * source file that includes <vulkan/vulkan.h>. HRL itself never exposes that
 * third-party header through this public API.
 */
typedef int (*HRL_VulkanCreateSurfaceCallback)(
    void* instance,
    void* surface_out,
    void* user_data);

/**
 * Configure the callback used to create the VkSurfaceKHR.
 * Must be called before HRL_InitContext() for presentation rendering.
 */
HRL_API void HRL_Vulkan_SetSurfaceCallback(
    HRL_VulkanCreateSurfaceCallback callback,
    void* user_data);

/**
 * Provide the Vulkan instance extensions required by the host window system.
 * The strings are copied by HRL, so the caller may release its own array after
 * this function returns.
 */
HRL_API void HRL_Vulkan_SetInstanceExtensions(
    const char* const* extensions,
    HRL_uint extension_count);

/** Opaque Vulkan VkInstance handle, or NULL before initialization. */
HRL_API void* HRL_Vulkan_GetInstance(void);

/** Opaque Vulkan VkPhysicalDevice handle, or NULL before initialization. */
HRL_API void* HRL_Vulkan_GetPhysicalDevice(void);

/** Opaque Vulkan VkDevice handle, or NULL before initialization. */
HRL_API void* HRL_Vulkan_GetDevice(void);

/** Opaque Vulkan VkQueue handle, or NULL before initialization. */
HRL_API void* HRL_Vulkan_GetGraphicsQueue(void);

/**
 * Raw Vulkan VkSurfaceKHR handle value.
 *
 * On platforms/configurations where Vulkan uses pointer-backed
 * non-dispatchable handles, the returned integer contains the handle bits.
 */
HRL_API uint64_t HRL_Vulkan_GetSurface(void);

#ifdef __cplusplus
}
#endif

#endif
