// HRL native Vulkan backend - scene renderer.
//
// Frame structure (per scene, per viewport):
//   1. shadow passes  : cascaded shadow maps of the first shadow casting
//                       directional light + up to 4 spot light shadow maps
//                       (skipped when ray traced shadows are active)
//   2. forward pass   : HDR colour + object id, reversed-Z depth,
//                       multi-draw-indirect from the scene instance buffer
//   3. sky            : drawn where the depth buffer is still empty
// then once per scene: tone mapping to the LDR scene image, debug overlay,
// and the copy to the swapchain for on-screen scenes.
#ifndef HRL_VK_RENDERER_H
#define HRL_VK_RENDERER_H

#include "../../core/object_types.h"
#include "../../hrl.h"

namespace hrlvk
{
bool RendererInit(uint32_t width, uint32_t height);
void RendererShutdown();
void RendererResize(uint32_t width, uint32_t height);

void RendererBeginFrame();
void RendererRenderScene(hrl_scene_t* scene, HRL_id sceneId);
void RendererDrawDebug(const DebugRenderer& debug, float lineThickness);
void RendererEndFrame();

int CreateMesh(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertexCount, const HRL_uint* indices, size_t indexCount);
void DeleteMeshLODs(HRL_id id);
void DeleteMesh(HRL_id id);

HRL_id CreateTexture(HRL_id id, const unsigned char* rgba, int width, int height);
void DeleteTexture(HRL_id id);
bool IsValidTexture(HRL_id id);
void GetTextureSize(HRL_id id, int* width, int* height);
void SetTextureFilter(HRL_id id, HRL_EFilterType filter, bool minFilter);

void CreateScene(HRL_id id, bool onScreen);
void DeleteScene(HRL_id id);
void ResizeScene(HRL_id id, int width, int height);
void TakeScreenshot(HRL_id scene, const char* path);
HRL_id GetHoveredObject(HRL_id scene, int mouseX, int mouseY, HRL_EMeshType* type);

// Matrices of the last rendered viewport, in the OpenGL clip convention used
// by the public HRL API on every backend.
void GetViewMatrix(float* out);
void GetProjectionMatrix(float* out);

bool RayTracingSupported();
void SetRayTracedShadows(bool enabled);
bool RayTracedShadowsEnabled();
void SetShadowDistance(float distance);
} // namespace hrlvk

#endif
