// HRL native Vulkan backend - backend table and hrl_vulkan.h entry points.
//
// Scope of the native backend: static 3D meshes with PBR materials, lights,
// shadows, image based lighting, sky, fog, tone mapping, debug primitives,
// picking and screenshots. The features that only exist in the deprecated
// OpenGL backend report HRL_INVALID_BACKEND_OPERATION once (see Unsupported).
#include "vulkan_backend.h"

#include "vk_device.h"
#include "vk_renderer.h"
#include "../../core/backend_api.h"
#include "../../core/render_common.h"
#include "../../hrl_vulkan.h"

#include <stb/stb_image.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
// Reports a missing feature once per feature, not once per call.
void Unsupported(const char* feature)
{
	static std::set<std::string> reported;
	if (!reported.insert(feature).second) return;
	SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING,
		std::string("Vulkan backend: ") + feature + " is not implemented yet (available in the deprecated OpenGL 3.3 backend)");
}

void VK_Init() {}

void VK_InitContext(HRL_uint width, HRL_uint height, void*)
{
	hrlvk::Device& dev = hrlvk::GetDevice();
	if (!dev.Init(width, height))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Vulkan: device initialization failed");
		return;
	}
	if (!hrlvk::RendererInit(width, height))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL, "Vulkan: renderer initialization failed");
		hrlvk::RendererShutdown();
		dev.Shutdown();
	}
}

void VK_Shutdown()
{
	if (!hrlvk::GetDevice().IsInitialized()) return;
	hrlvk::RendererShutdown();
	hrlvk::GetDevice().Shutdown();
}

void VK_Resize(int width, int height)
{
	if (width > 0 && height > 0) hrlvk::RendererResize((uint32_t)width, (uint32_t)height);
}

void VK_RenderSceneFrame(const HRL_SceneFrame& frame)
{
	if (const hrl_scene_t* scene = frame.scene)
	{
		if (scene->voxel_world) Unsupported("voxel worlds");
		if (!scene->vfx_systems.empty()) Unsupported("VFX systems");
		if (!scene->decals.empty()) Unsupported("decals");
		if (scene->global_illumination_enabled) Unsupported("global illumination");
	}
	hrlvk::RendererRenderScene(frame);
}

// Entry points of the legacy (OpenGL) frame path: never called for this
// backend, which implements RHI_RenderSceneFrame.
void VK_LegacyRenderScene(hrl_scene_t*, HRL_id) {}
void VK_LegacyDrawDebug(const DebugRenderer&, float) {}

int VK_CreateMesh(HRL_id id, const HRL_Vertex3D* v, size_t vc, const HRL_uint* i, size_t ic)
{
	return hrlvk::CreateMesh(id, 0, v, vc, i, ic);
}
int VK_CreateMeshLOD(HRL_id id, HRL_uint level, const HRL_Vertex3D* v, size_t vc, const HRL_uint* i, size_t ic)
{
	return hrlvk::CreateMesh(id, level, v, vc, i, ic);
}
int VK_CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* v, size_t vc, const HRL_uint* i, size_t ic, HRL_uint)
{
	return hrlvk::CreateSkeletalMesh(id, v, vc, i, ic);
}
int VK_CreateSpriteMesh(HRL_id)
{
	Unsupported("sprites");
	return HRL_FALSE;
}

HRL_id VK_CreateTextureFromBitmapWithId(HRL_id id, BitmapResult bmp)
{
	if (bmp.width <= 0 || bmp.height <= 0 || bmp.pixels.size() < size_t(bmp.width) * size_t(bmp.height) * 4u)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "Vulkan: invalid bitmap");
		return HRL_INVALID_ID;
	}
	return hrlvk::CreateTexture(id, bmp.pixels.data(), bmp.width, bmp.height);
}
HRL_id VK_CreateTextureFromBitmap(BitmapResult bmp)
{
	return VK_CreateTextureFromBitmapWithId(HRL_INVALID_ID, std::move(bmp));
}
HRL_id VK_CreateTexture(const char* content, const size_t size)
{
	int w = 0, h = 0, channels = 0;
	stbi_uc* data = (content && size) ? stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(content),
		(int)size, &w, &h, &channels, STBI_rgb_alpha) : nullptr;
	if (!data || w <= 0 || h <= 0)
	{
		stbi_image_free(data);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "Texture failed to load");
		return HRL_INVALID_ID;
	}
	// Same row order as the OpenGL backend: the first row in memory is the
	// bottom of the picture (v = 0).
	std::vector<unsigned char> flipped(size_t(w) * size_t(h) * 4u);
	const size_t row = size_t(w) * 4u;
	for (int y = 0; y < h; ++y)
		std::memcpy(flipped.data() + size_t(y) * row, data + size_t(h - 1 - y) * row, row);
	stbi_image_free(data);
	return hrlvk::CreateTexture(HRL_INVALID_ID, flipped.data(), w, h);
}
void VK_SetTextureMinFilter(HRL_id id, HRL_EFilterType f) { hrlvk::SetTextureFilter(id, f, true); }
void VK_SetTextureMagFilter(HRL_id id, HRL_EFilterType f) { hrlvk::SetTextureFilter(id, f, false); }
int VK_IsValidTexture(HRL_id id) { return hrlvk::IsValidTexture(id) ? HRL_TRUE : HRL_FALSE; }

void VK_CreateScene(HRL_id id, int onScreen) { hrlvk::CreateScene(id, onScreen != 0); }

HRL_id VK_CreateShader(const char*, size_t, const char*, size_t)
{
	Unsupported("custom shaders (surface functions)");
	return HRL_INVALID_ID;
}
HRL_id VK_CreateShaderWithId(HRL_id, const char*, size_t, const char*, size_t)
{
	Unsupported("custom shaders (surface functions)");
	return HRL_INVALID_ID;
}
// The built-in shaders that have a Vulkan implementation.
int VK_IsValidShader(HRL_id shader)
{
	return (shader == HRL_MESH_3D_SHADER || shader == HRL_SKINNED_3D_MESH_SHADER || shader == HRL_DEFAULT_POST_PROCESS_SHADER)
		? HRL_TRUE : HRL_FALSE;
}

void VK_GetModelMatrix(HRL_Mesh* mesh, float* out)
{
	if (!mesh || !out) return;
	const glm::mat4 m = CalculateModelMatrix(mesh);
	std::memcpy(out, &m, sizeof(m));
}

void VK_CreatePostProcess(HRL_id, int) {} // the exposure of the material is read by the tone mapper
void VK_NoopId(HRL_id) {}
void VK_Noop() {}
void VK_UpdateLights(const std::vector<HRL_Light*>&) {} // lights are read from the scene each frame
void VK_FogChanged(HRL_id, hrl_fog_t*) {}
void VK_BindViewport(HRL_Viewport*) {}
void VK_BindMaterial(HRL_Material*) {}
void VK_DrawMesh(HRL_Mesh*) {}
void VK_EnablePicking(HRL_id, int) {} // the object id buffer is always rendered
void VK_SetAntialiasing(int samples) { if (samples > 1) Unsupported("MSAA"); }
int VK_GISupported(int) { return HRL_FALSE; }
uint32_t VK_GIMethods() { return 0; }
} // namespace

HRL_vtable GetVulkanBackend()
{
	HRL_vtable t{};
	t.RHI_Init = VK_Init;
	t.RHI_InitContext = VK_InitContext;
	t.RHI_Shutdown = VK_Shutdown;
	t.RHI_WindowResizeCallback = VK_Resize;
	t.RHI_BeginFrame = hrlvk::RendererBeginFrame;
	t.RHI_RenderScene = VK_LegacyRenderScene;
	t.RHI_RenderSceneFrame = VK_RenderSceneFrame;
	t.RHI_ResetFramebuffer = hrlvk::RendererEndFrame;
	t.RHI_BindViewport = VK_BindViewport;
	t.RHI_ComputeFrameMatrices = VK_Noop;
	t.RHI_BindMaterial = VK_BindMaterial;
	t.RHI_DrawMesh = VK_DrawMesh;
	t.RHI_CreateMesh = VK_CreateMesh;
	t.RHI_CreateSkeletalMesh = VK_CreateSkeletalMesh;
	t.RHI_CreateMeshLOD = VK_CreateMeshLOD;
	t.RHI_DeleteMeshLODs = hrlvk::DeleteMeshLODs;
	t.RHI_CreateSpriteMesh = VK_CreateSpriteMesh;
	t.RHI_DeleteMesh = hrlvk::DeleteMesh;
	t.RHI_UpdateLights = VK_UpdateLights;
	t.RHI_DeleteLight = VK_NoopId;
	t.RHI_CreateTexture = VK_CreateTexture;
	t.RHI_CreateTextureFromBitmap = VK_CreateTextureFromBitmap;
	t.RHI_CreateTextureFromBitmapWithId = VK_CreateTextureFromBitmapWithId;
	t.RHI_DeleteTexture = hrlvk::DeleteTexture;
	t.RHI_GetTextureSize = hrlvk::GetTextureSize;
	t.RHI_SetTextureMinFilter = VK_SetTextureMinFilter;
	t.RHI_SetTextureMaxFilter = VK_SetTextureMagFilter;
	t.RHI_CreateScene = VK_CreateScene;
	t.RHI_DeleteScene = hrlvk::DeleteScene;
	t.RHI_BindScene = VK_NoopId;
	t.RHI_ClearScene = VK_Noop;
	t.RHI_ResizeSceneTexture = hrlvk::ResizeScene;
	t.RHI_EnableColorPickingBuffer = VK_EnablePicking;
	t.RHI_SetAntialiasingMode = VK_SetAntialiasing;
	t.RHI_CreateShader = VK_CreateShader;
	t.RHI_CreateShaderWithId = VK_CreateShaderWithId;
	t.RHI_DeleteShader = VK_NoopId;
	t.RHI_GetProjectionMatrix = hrlvk::GetProjectionMatrix;
	t.RHI_GetViewMatrix = hrlvk::GetViewMatrix;
	t.RHI_GetModelMatrix = VK_GetModelMatrix;
	t.RHI_CreatePostProcess = VK_CreatePostProcess;
	t.RHI_DeletePostProcess = VK_NoopId;
	t.RHI_IsGlobalIlluminationMethodSupported = VK_GISupported;
	t.RHI_GetGlobalIlluminationSupportedMethods = VK_GIMethods;
	t.RHI_FogPropertyChanged = VK_FogChanged;
	t.RHI_DrawDebug = VK_LegacyDrawDebug;
	t.RHI_IsValidTexture = VK_IsValidTexture;
	t.RHI_ReadTexturePixels = hrlvk::ReadTexturePixels;
	t.RHI_IsValidShader = VK_IsValidShader;
	t.RHI_GetHoveredObject = hrlvk::GetHoveredObject;
	return t;
}

// ---------------------------------------------------------------------------
// hrl_vulkan.h
// ---------------------------------------------------------------------------
void HRL_Vulkan_SetSurfaceCallback(HRL_VulkanCreateSurfaceCallback callback, void* user_data)
{
	hrlvk::GetDevice().surfaceCallback = callback;
	hrlvk::GetDevice().surfaceUser = user_data;
}

void HRL_Vulkan_SetInstanceExtensions(const char* const* extensions, HRL_uint extension_count)
{
	auto& list = hrlvk::GetDevice().instanceExtensions;
	list.clear();
	for (HRL_uint i = 0; extensions && i < extension_count; ++i)
		if (extensions[i]) list.emplace_back(extensions[i]);
}

void* HRL_Vulkan_GetInstance(void) { return reinterpret_cast<void*>(hrlvk::GetDevice().instance); }
void* HRL_Vulkan_GetPhysicalDevice(void) { return reinterpret_cast<void*>(hrlvk::GetDevice().physical); }
void* HRL_Vulkan_GetDevice(void) { return reinterpret_cast<void*>(hrlvk::GetDevice().device); }
void* HRL_Vulkan_GetGraphicsQueue(void) { return reinterpret_cast<void*>(hrlvk::GetDevice().queue); }
uint64_t HRL_Vulkan_GetSurface(void) { return (uint64_t)hrlvk::GetDevice().surface; }

int HRL_Vulkan_IsRayTracingSupported(void) { return hrlvk::RayTracingSupported() ? HRL_TRUE : HRL_FALSE; }
void HRL_Vulkan_SetRayTracedShadows(int enabled) { hrlvk::SetRayTracedShadows(enabled != 0); }
int HRL_Vulkan_GetRayTracedShadows(void) { return hrlvk::RayTracedShadowsEnabled() ? HRL_TRUE : HRL_FALSE; }
void HRL_Vulkan_SetShadowDistance(float distance) { hrlvk::SetShadowDistance(distance); }
void HRL_Vulkan_SetOcclusionCulling(int enabled) { hrlvk::SetOcclusionCulling(enabled != 0); }
int HRL_Vulkan_GetFoliageStats(HRL_id foliage, HRL_VulkanFoliageStats* stats)
{
	if (!stats) return HRL_FALSE;
	return hrlvk::GetFoliageStats(foliage, *stats) ? HRL_TRUE : HRL_FALSE;
}
int HRL_Vulkan_GetOcclusionCulling(void) { return hrlvk::OcclusionCulling() ? HRL_TRUE : HRL_FALSE; }
const char* HRL_Vulkan_GetDeviceName(void) { return hrlvk::GetDevice().caps.deviceName.c_str(); }

void HRL_Vulkan_SetPipelineCachePath(const char* path) { hrlvk::SetPipelineCachePath(path); }

int HRL_Vulkan_GetMemoryUsage(uint64_t* used_bytes, uint64_t* budget_bytes)
{
	uint64_t used = 0, budget = 0;
	const bool ok = hrlvk::GetDevice().IsInitialized();
	if (ok) hrlvk::GetDevice().MemoryUsage(used, budget);
	if (used_bytes) *used_bytes = used;
	if (budget_bytes) *budget_bytes = budget;
	return ok ? HRL_TRUE : HRL_FALSE;
}

int HRL_Vulkan_GetSceneStats(HRL_id scene, HRL_VulkanSceneStats* stats)
{
	if (!stats) return HRL_FALSE;
	return hrlvk::GetSceneStats(scene, *stats) ? HRL_TRUE : HRL_FALSE;
}
