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

/**
 * Returns HRL_TRUE when the GPU and its driver support hardware ray tracing
 * (VK_KHR_acceleration_structure + VK_KHR_ray_query). Valid after
 * HRL_InitContext(). The environment variable HRL_VULKAN_NO_RAYTRACING=1
 * forces the non ray traced code paths.
 */
HRL_API int HRL_Vulkan_IsRayTracingSupported(void);

/**
 * Selects how the shadows of shadow casting lights are computed.
 *
 * Disabled (default): cascaded shadow maps for the first shadow casting
 * directional light and shadow maps for up to 4 spot lights.
 * Enabled: every shadow casting light (directional, spot and point) traces a
 * shadow ray against the scene acceleration structure. Ignored, with the
 * shadow maps kept, when ray tracing is not supported.
 */
HRL_API void HRL_Vulkan_SetRayTracedShadows(int enabled);

/** Returns HRL_TRUE when ray traced shadows are active. */
HRL_API int HRL_Vulkan_GetRayTracedShadows(void);

/**
 * Distance from the camera, in world units, covered by the cascaded shadow
 * maps (default 150). Shorter distances give sharper shadows.
 */
HRL_API void HRL_Vulkan_SetShadowDistance(float distance);

/** Instances of a foliage kind drawn by the first camera (read back from the GPU, 2 frames late). */
typedef struct HRL_VulkanFoliageStats {
    uint32_t lod_instances[3];   /* mesh LOD 0, 1, 2 (cross-fading instances count in both levels) */
    uint32_t impostor_instances;
} HRL_VulkanFoliageStats;
HRL_API int HRL_Vulkan_GetFoliageStats(HRL_id foliage, HRL_VulkanFoliageStats* stats);

/**
 * Occlusion culling of the 3D meshes, foliage and grass (default on): what is
 * hidden behind the meshes visible in the previous frame and the landscapes is
 * skipped on the GPU. Disabling it is only useful to measure its effect.
 */
HRL_API void HRL_Vulkan_SetOcclusionCulling(int enabled);
HRL_API int HRL_Vulkan_GetOcclusionCulling(void);

/** Name of the GPU selected by HRL, or an empty string before initialization. */
HRL_API const char* HRL_Vulkan_GetDeviceName(void);

/**
 * File used to keep compiled pipelines between runs. Call before
 * HRL_InitContext(): the file is read when it exists and matches the GPU and
 * driver, and (re)written by HRL_Shutdown(). It removes most of the pipeline
 * compilation time of the following runs. NULL or "" (default): no file, unless
 * the HRL_VULKAN_PIPELINE_CACHE environment variable names one.
 */
HRL_API void HRL_Vulkan_SetPipelineCachePath(const char* path);

/**
 * GPU memory used by the process and its current budget, in bytes (the amount
 * usable before the system has to evict memory; it varies with the other
 * applications). Either pointer may be NULL. Returns HRL_FALSE before
 * initialization or with another backend.
 */
HRL_API int HRL_Vulkan_GetMemoryUsage(uint64_t* used_bytes, uint64_t* budget_bytes);

/** Counters of the last rendered frame of a scene (diagnostics, profiling). */
typedef struct HRL_VulkanSceneStats {
    uint32_t instance_count;        /**< 3D meshes of the scene that can be drawn. */
    uint32_t material_count;        /**< Materials used by those meshes. */
    uint32_t drawn_instance_count;  /**< Instances that passed frustum culling, summed over the viewports. */
    uint32_t render_pass_count;     /**< Render passes recorded for the scene. */
    uint32_t scene_rebuilds;        /**< 1 when the cached scene data had to be rebuilt entirely. */
    uint32_t transform_updates;     /**< Instances whose transform was recomputed. */
    uint32_t material_updates;      /**< Materials whose GPU data was recomputed. */
    uint32_t skinned_mesh_count;    /**< Skeletal meshes skinned this frame (their pose changed). */
} HRL_VulkanSceneStats;

/**
 * Fills `stats` for a scene rendered by the Vulkan backend.
 * Returns HRL_FALSE (and zeroes) for an unknown scene or another backend.
 */
HRL_API int HRL_Vulkan_GetSceneStats(HRL_id scene, HRL_VulkanSceneStats* stats);

#ifdef __cplusplus
}
#endif

#endif
