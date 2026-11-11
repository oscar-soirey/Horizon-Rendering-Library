// HRL native Vulkan backend - scene renderer.
//
// Entry points used by vulkan_backend.cpp. The frame structure is described
// in vk_renderer.cpp (RendererRenderScene) and docs/ARCHITECTURE.md.
#ifndef HRL_VK_RENDERER_H
#define HRL_VK_RENDERER_H

#include "../../core/backend_api.h"
#include "../../hrl_vulkan.h"

namespace hrlvk
{
bool RendererInit(uint32_t width, uint32_t height);
void RendererShutdown();
void RendererResize(uint32_t width, uint32_t height);

void RendererBeginFrame();
void RendererRenderScene(const HRL_SceneFrame& frame);
void RendererEndFrame();

// rayTraced: build the acceleration structure of the mesh (LOD 0, when supported).
int CreateMesh(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertexCount, const HRL_uint* indices, size_t indexCount, bool rayTraced = true);
int CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertexCount, const HRL_uint* indices, size_t indexCount);
void DeleteMeshLODs(HRL_id id);
void DeleteMesh(HRL_id id);

HRL_id CreateTexture(HRL_id id, const unsigned char* rgba, int width, int height);
void DeleteTexture(HRL_id id);
bool IsValidTexture(HRL_id id);
void GetTextureSize(HRL_id id, int* width, int* height);
int ReadTexturePixels(HRL_id id, std::vector<unsigned char>* rgba, int* width, int* height);
void SetTextureFilter(HRL_id id, HRL_EFilterType filter, bool minFilter);

void CreateScene(HRL_id id, bool onScreen);
void DeleteScene(HRL_id id);
void ResizeScene(HRL_id id, int width, int height);
HRL_id GetHoveredObject(HRL_id scene, int mouseX, int mouseY, HRL_EMeshType* type);

// Matrices of the last rendered viewport, in the OpenGL clip convention used
// by the public HRL API on every backend.
void GetViewMatrix(float* out);
void GetProjectionMatrix(float* out);

bool GetSceneStats(HRL_id scene, HRL_VulkanSceneStats& stats);

// File the pipeline cache is loaded from / saved to (null or empty: none).
void SetPipelineCachePath(const char* path);

bool RayTracingSupported();
void SetRayTracedShadows(bool enabled);
bool RayTracedShadowsEnabled();
void SetShadowDistance(float distance);
void SetOcclusionCulling(bool enabled);
bool GetFoliageStats(HRL_id foliage, HRL_VulkanFoliageStats& stats);
bool OcclusionCulling();
} // namespace hrlvk

#endif
