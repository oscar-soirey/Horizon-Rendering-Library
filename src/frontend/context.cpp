// HRL frontend - initialisation, frame loop, errors, validity queries.
#include "frontend_internal.h"

// Implementation of stb_truetype (the declarations come with object_types.h).
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

HRL_Context ctx_;
HRL_vtable g_Backend;
AsyncResourceLoader g_AsyncResourceLoader;

HRL_Context* GetPrivateContext()
{
	return &ctx_;
}

#ifndef HRL_ENABLE_VULKAN
// Vulkan backend not compiled : the hrl_vulkan.h entry points are inert.
void HRL_Vulkan_SetSurfaceCallback(HRL_VulkanCreateSurfaceCallback, void*) {}
void HRL_Vulkan_SetInstanceExtensions(const char* const*, HRL_uint) {}
void* HRL_Vulkan_GetInstance(void) { return nullptr; }
void* HRL_Vulkan_GetPhysicalDevice(void) { return nullptr; }
void* HRL_Vulkan_GetDevice(void) { return nullptr; }
void* HRL_Vulkan_GetGraphicsQueue(void) { return nullptr; }
uint64_t HRL_Vulkan_GetSurface(void) { return 0; }
int HRL_Vulkan_IsRayTracingSupported(void) { return HRL_FALSE; }
void HRL_Vulkan_SetRayTracedShadows(int) {}
int HRL_Vulkan_GetRayTracedShadows(void) { return HRL_FALSE; }
void HRL_Vulkan_SetShadowDistance(float) {}
const char* HRL_Vulkan_GetDeviceName(void) { return ""; }
int HRL_Vulkan_GetSceneStats(HRL_id, HRL_VulkanSceneStats* stats) { if (stats) *stats = HRL_VulkanSceneStats{}; return HRL_FALSE; }
#endif


//Utils Non-API Functions :
std::vector<HRL_Mesh*> GetSortedSprites(hrl_scene_t* _scene)
{
	std::vector<HRL_Mesh*> sprites_;
	for (const auto& [id, mesh] : _scene->meshes)
	{
		if (mesh->type_ == HRL_SPRITE)
		{
			sprites_.push_back(mesh);
		}
	}

	std::stable_sort(sprites_.begin(), sprites_.end(), [](const HRL_Mesh* a, const HRL_Mesh* b)
	{
		if (a->position_.z == b->position_.z)
		{
			return a->draw_order_ < b->draw_order_;
		}
		return a->position_.z < b->position_.z;
	});

	return sprites_;
}

std::vector<HRL_Light*> GetLightsVector(HRL_id _scene)
{
	auto it = ctx_.scenes.find(_scene);
	if (it == ctx_.scenes.end())
	{
		return {};
	}

	std::vector<HRL_Light*> lvector;
	lvector.reserve(it->second->lights.size());

	for (const auto& [id, light] : it->second->lights)
	{
		lvector.push_back(light);
	}
	return lvector;
}

void UpdateSceneLights(HRL_id _scene)
{
	if (ctx_.scenes.find(_scene) == ctx_.scenes.end())
		return;

	g_Backend.RHI_UpdateLights(GetLightsVector(_scene));
}

void MarkSceneGIGeometryDirty(hrl_scene_t* scene)
{
	if (!scene)
		return;
	++scene->gi_geometry_revision;
	scene->shadows_dirty = true;
}

void MarkSceneGILightingDirty(hrl_scene_t* scene)
{
	if (scene)
		++scene->gi_lighting_revision;
}

void MarkAllScenesGILightingDirty()
{
	for (auto& [sceneId, scene] : ctx_.scenes)
	{
		(void)sceneId;
		MarkSceneGILightingDirty(scene);
	}
}

// La GI ne lit les materiaux que des meshes 3D statiques (voir
// GL_33_GI::EnsureGeometryCache). Modifier un materiau ne doit donc invalider
// que les scenes qui l'utilisent, et non toutes les sondes de toutes les scenes.
void MarkScenesUsingMaterialGILightingDirty(HRL_id materialId)
{
	// Called by every material setter after a real change: backends that cache
	// packed material data compare this revision.
	auto materialIt = ctx_.materials.find(materialId);
	if (materialIt != ctx_.materials.end() && materialIt->second)
		++materialIt->second->revision_;

	for (auto& [sceneId, scene] : ctx_.scenes)
	{
		(void)sceneId;
		if (!scene)
			continue;
		for (const auto& [meshId, mesh] : scene->meshes)
		{
			(void)meshId;
			if (mesh && mesh->type_ == HRL_3D_MESH && mesh->material_ == materialId)
			{
				MarkSceneGILightingDirty(scene);
				break;
			}
		}
	}
}


void UpdateMaterialIntFlags(HRL_Material* material, const char* name, int value)
{
	if (!material || !name)
		return;
	if (std::strcmp(name, HRL_MATERIAL_PARAM_TWO_SIDED) == 0)
		material->two_sided_ = value != 0;
	else if (std::strcmp(name, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED) == 0)
		material->ss_displacement_enabled_ = value != 0;
}


/// API Implementation ///

void HRL_Init(HRL_E_APIs _api)
{
	g_AsyncResourceLoader.Start();
	bool backendAvailable = false;
#ifdef HRL_ENABLE_VULKAN
	backendAvailable = backendAvailable || (_api == HRL_VULKAN);
#endif
#ifndef HRL_DISABLE_OPENGL33
	backendAvailable = backendAvailable || (_api == HRL_OPENGL_33);
#endif
#ifdef HRL_ENABLE_GLES3
	backendAvailable = backendAvailable || (_api == HRL_OPENGL_ES_30);
#endif
	if (!backendAvailable)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_Init: selected backend is not implemented (or was not compiled in this build)");
		return;
	}

	switch (_api)
	{
#ifndef HRL_DISABLE_OPENGL33
	case HRL_OPENGL_33 :
	{
		g_Backend = GetOpenGL33Backend();
		g_Backend.RHI_Init();
		SetErrorCode(HRL_NO_ERROR, HRL_SEVERITY_WEAK_WARNING,
			"HRL_OPENGL_33 is deprecated: it receives bug fixes only. New features are developed for HRL_VULKAN.");
		break;
	}
#endif
#ifdef HRL_ENABLE_GLES3
	case HRL_OPENGL_ES_30 :
	{
		g_Backend = GetOpenGLES3Backend();
		g_Backend.RHI_Init();
		break;
	}
#endif
	case HRL_OPENGL_45 :
	{
		break;
	}
#ifdef HRL_ENABLE_VULKAN
	case HRL_VULKAN :
	{
		g_Backend = GetVulkanBackend();
		g_Backend.RHI_Init();
		break;
	}
#endif
	case HRL_D3D11 :
	{
		break;
	}
	case HRL_D3D12 :
	{
		break;
	}
	case HRL_METAL :
	{
		break;
	}
	case HRL_NVN :
	{
		break;
	}
	case HRL_GNM :
	{
		break;
	}
	default :
	{
		assert(false && "HRL : Backend not supported");
		break;
	}
	}
}

void HRL_InitContext(HRL_uint _width, HRL_uint _height, void* _loader)
{
	ctx_.window_width  = _width;
	ctx_.window_height = _height;
	// The third parameter is still the OpenGL loader for the OpenGL backend.
	// Vulkan presentation is configured through hrl_vulkan.h and never casts the
	// loader argument to a Vulkan-specific private structure.
	if (g_Backend.RHI_InitContext)
	{
		g_Backend.RHI_InitContext(_width, _height, _loader);
	}
}

void HRL_Shutdown()
{
	HRL_WaitForAllAsyncResources();
	g_AsyncResourceLoader.Stop();
	// Destroy flat resources that are referenced by scene viewports first.
	{
		std::vector<HRL_id> ids;
		ids.reserve(ctx_.post_processes.size());
		for (const auto& [id, pp] : ctx_.post_processes)
		{ (void)pp; ids.push_back(id); }
		for (HRL_id id : ids) HRL_DeletePostProcess(id);
	}
	{
		std::vector<HRL_id> ids;
		ids.reserve(ctx_.widgets.size());
		for (const auto& [id, widget] : ctx_.widgets)
		{ (void)widget; ids.push_back(id); }
		for (HRL_id id : ids) HRL_DeleteWidget(id);
	}

	//supprimer tous les objets de toutes les scenes
	for (const auto& [scene_id, scene] : ctx_.scenes)
	{
		for (const auto& [id, mesh] : scene->meshes)
		{
			g_Backend.RHI_DeleteMesh(id);
			delete mesh;
		}
		scene->meshes.clear();

		for (const auto& [id, light] : scene->lights)
		{
			g_Backend.RHI_DeleteLight(id);
			delete light;
		}
		scene->lights.clear();

		for (const auto& [id, landscape] : scene->landscapes)
		{
			(void)id;
			delete landscape;
		}
		scene->landscapes.clear();

		std::vector<HRL_id> vfx_system_ids;
		vfx_system_ids.reserve(scene->vfx_systems.size());
		for (const auto& [id, system] : scene->vfx_systems) { (void)system; vfx_system_ids.push_back(id); }
		for (HRL_id id : vfx_system_ids) HRL_DeleteVFXSystem(id);

		for (const auto& [id, fog] : scene->volumetric_fogs)
		{
			delete fog;
		}
		scene->volumetric_fogs.clear();

		for (const auto& [id, decal] : scene->decals)
		{
			delete decal;
		}
		scene->decals.clear();

		for (const auto& [id, viewport] : scene->viewports)
		{
			delete viewport;
		}
		scene->viewports.clear();

		for (const auto& [id, camera] : scene->cameras)
		{
			delete camera;
		}
		scene->cameras.clear();

		// Release backend scene resources before the scene object itself disappears.
		g_Backend.RHI_DeleteScene(scene_id);
		delete scene;
	}
	ctx_.scenes.clear();
	ctx_.debug_renderers.clear();

	//nettoyer les caches flat
	ctx_.meshes.clear();
	ctx_.landscapes.clear();
	ctx_.lights.clear();
	ctx_.volumetric_fogs.clear();
	ctx_.decals.clear();
	ctx_.vfx_systems.clear();
	ctx_.vfx_emitters.clear();
	ctx_.vfx_curves.clear();
	ctx_.vfx_has_frame_time = false;
	ctx_.vfx_last_frame_time = 0.0;
	ctx_.viewports.clear();
	ctx_.cameras.clear();
	ctx_.post_processes.clear();
	ctx_.pending_screenshots.clear();

	for (const auto& [id, material] : ctx_.materials)
	{
		if (material)
		{
			for (HRL_id textureId : material->owned_textures_)
			{
				if (textureId != HRL_INVALID_ID)
					HRL_DeleteTexture(textureId);
			}
		}
		delete material;
	}
	ctx_.materials.clear();

	for (const auto& [id, font] : ctx_.fonts)
	{
		delete font;
	}
	ctx_.fonts.clear();
	ctx_.async_resource_states.clear();

	g_Backend.RHI_Shutdown();
}

void ProcessAsyncResourceUploads()
{
  AsyncResourceResult result;
  while (g_AsyncResourceLoader.TryPopResult(result))
  {
    auto it = ctx_.async_resource_states.find(result.id);
    if (it == ctx_.async_resource_states.end()) continue;
    if (!result.success)
    {
      it->second = static_cast<int>(AsyncResourceState::Failed);
      SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, result.error.empty() ? "Async resource preparation failed" : result.error);
      continue;
    }
    if (result.type == AsyncResourceResult::Type::Texture)
    {
      const HRL_id created = g_Backend.RHI_CreateTextureFromBitmapWithId ? g_Backend.RHI_CreateTextureFromBitmapWithId(result.id, std::move(result.bitmap)) : HRL_INVALID_ID;
      it->second = created == result.id ? static_cast<int>(AsyncResourceState::Ready) : static_cast<int>(AsyncResourceState::Failed);
      if (created != result.id) SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Async texture GPU upload failed");
    }
    else
    {
      const HRL_id created = g_Backend.RHI_CreateShaderWithId ? g_Backend.RHI_CreateShaderWithId(result.id, result.vert.data(), result.vert.size(), result.frag.data(), result.frag.size()) : HRL_INVALID_ID;
      it->second = created == result.id ? static_cast<int>(AsyncResourceState::Ready) : static_cast<int>(AsyncResourceState::Failed);
      if (created != result.id) SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "Async shader compilation/upload failed");
    }
  }
}

void HRL_BeginFrame()
{
	// Completed CPU-side loads are uploaded to the active OpenGL context here.
	ProcessAsyncResourceUploads();
	if (g_Backend.RHI_BeginFrame)
		g_Backend.RHI_BeginFrame();
	const auto now = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
	if (ctx_.vfx_has_frame_time)
	{
		const float dt = static_cast<float>(glm::clamp(seconds - ctx_.vfx_last_frame_time, 0.0, 0.1));
		for (auto& [scene_id, scene] : ctx_.scenes)
		{
			(void)scene_id;
			if (!scene || scene->screen_messages.empty())
				continue;
			for (auto& message : scene->screen_messages)
				message.remaining_seconds -= dt;
			scene->screen_messages.erase(
				std::remove_if(scene->screen_messages.begin(), scene->screen_messages.end(),
					[](const HRL_ScreenMessage& message) { return message.remaining_seconds <= 0.0f; }),
				scene->screen_messages.end());
		}
		for (const auto& [id, system] : ctx_.vfx_systems)
		{
			(void)id;
			if (system && system->auto_update_)
				SimulateVFXSystem(system, dt);
		}
	}
	ctx_.vfx_last_frame_time = seconds;
	ctx_.vfx_has_frame_time = true;
}


void HRL_EndFrame()
{
	ProcessGizmoInput();
	//appels à RHI_DrawMesh, HRI_BindMaterial, etc...
	for (const auto& [scene_id, scene] : ctx_.scenes)
	{
		if (!scene)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_EndFrame: Tried to bind an invalid scene");
			continue;
		}
		// Backends isolated from the frontend state receive the whole frame of
		// the scene in one structure (see core/backend_api.h).
		if (g_Backend.RHI_RenderSceneFrame)
		{
			HRL_SceneFrame frame;
			frame.sceneId = scene_id;
			frame.scene = scene;
			frame.materials = &ctx_.materials;
			auto debugIt = ctx_.debug_renderers.find(scene_id);
			if (debugIt != ctx_.debug_renderers.end())
				frame.debug = &debugIt->second;
			frame.debugLineThickness = ctx_.debug_line_thickness;
			auto screenshotIt = ctx_.pending_screenshots.find(scene_id);
			if (screenshotIt != ctx_.pending_screenshots.end())
				frame.screenshotPath = screenshotIt->second.c_str();
			g_Backend.RHI_RenderSceneFrame(frame);
			ctx_.pending_screenshots.erase(scene_id);
			ctx_.debug_renderers.erase(scene_id);
			continue;
		}

		// Legacy backends (OpenGL). Each scene owns its own light list. The
		// backend UBO is shared by the context, so refresh it immediately before
		// rendering this scene.
		UpdateSceneLights(scene_id);
		g_Backend.RHI_RenderScene(scene, scene_id);

		// Debug primitives are owned by the HRL context, while the backend
		// renderer has its own private context. Draw them explicitly here so
		// they are not lost between HRL and RHI_RenderScene().
		auto debugIt = ctx_.debug_renderers.find(scene_id);
		if (debugIt != ctx_.debug_renderers.end())
		{
			g_Backend.RHI_DrawDebug(debugIt->second, ctx_.debug_line_thickness);
		}

		// Screenshot requests are captured after the scene has completed its
		// post-process/UI passes for the frame.
		auto screenshotIt = ctx_.pending_screenshots.find(scene_id);
		if (screenshotIt != ctx_.pending_screenshots.end())
		{
			if (g_Backend.RHI_TakeScreenshot)
				g_Backend.RHI_TakeScreenshot(scene_id, screenshotIt->second.c_str());
			ctx_.pending_screenshots.erase(screenshotIt);
		}

		// Debug primitives are one-frame submissions. The backend consumes them
		// while rendering the complete scene (all viewports) above.
		ctx_.debug_renderers.erase(scene_id);
	}

	// Mouse transitions are frame-scoped. Keep the held state until the next input event.
	ctx_.mouseLeftPressed = false;
	ctx_.mouseLeftReleased = false;
	if (!ctx_.mouseLeftDown)
		ctx_.mouseCaptureWidget = HRL_INVALID_ID;
	if (!ctx_.mouseLeftDown && ctx_.mouseCaptureGizmo == HRL_INVALID_ID)
		ctx_.mouseCaptureGizmoPart = HRL_GIZMO_PART_NONE;
	if (g_Backend.RHI_ResetFramebuffer)
		g_Backend.RHI_ResetFramebuffer();
}

void HRL_WindowResizeCallback(int _width, int _height)
{
	ctx_.window_width  = _width;
	ctx_.window_height = _height;
	g_Backend.RHI_WindowResizeCallback(_width, _height);
}


HRL_EError HRL_GetLastError(const char** _detail, HRL_ESeverity* _severity)
{
	//on stocke dans une var statique pour eviter un use after free
	static std::string detail;
	detail     = ctx_.last_error.detail;
	*_detail   = detail.c_str();
	*_severity = ctx_.last_error.severity;
	return ctx_.last_error.code;
}

constexpr const char* errors_str[]={
	"HRL_NO_ERROR",
	"HRL_INVALID_ID",
	"HRL_INVALID_ENUM",
	"HRL_INVALID_VALUE",
	"HRL_INVALID_OPERATION",
	"HRL_INVALID_BACKEND_OPERATION",
	"HRL_SHADER_COMPILE_FAIL",
	"HRL_OUT_OF_MEMORY",
	"HRL_INVALID_FILE_FORMAT"
};
constexpr int HRL_ERROR_BASE = 0x0070;
constexpr int HRL_ERROR_COUNT = sizeof(errors_str) / sizeof(errors_str[0]);

const char* HRL_ErrorEnumToString(HRL_EError err)
{
	int index = static_cast<int>(err) - HRL_ERROR_BASE;

	if (index < 0 || index >= HRL_ERROR_COUNT)
		return "UNKNOWN_ERROR";

	return errors_str[index];
}

constexpr const char* severity_str[]={
	"HRL_SEVERITY_WEAK_WARNING",
	"HRL_SEVERITY_WARNING",
	"HRL_SEVERITY_ERROR",
	"HRL_SEVERITY_FATAL"
};
constexpr int HRL_SEVERITY_BASE = 0x0080;
constexpr int HRL_SEVERITY_COUNT = sizeof(severity_str) / sizeof(severity_str[0]);

const char* HRL_SeverityEnumToString(HRL_ESeverity sev)
{
	int index = static_cast<int>(sev) - HRL_SEVERITY_BASE;

	if (index < 0 || index >= HRL_SEVERITY_COUNT)
		return "UNKNOWN_SEVERITY";

	return severity_str[index];
}

void HRL_RegisterErrorCallback(HRL_CErrorCallback _callback)
{
	ctx_.error_callback = _callback;
}


//MATRICES
void HRL_GetProjectionMatrix(float *aa)
{
	g_Backend.RHI_GetProjectionMatrix(aa);
}

void HRL_GetViewMatrix(float *aa)
{
	g_Backend.RHI_GetViewMatrix(aa);
}

void HRL_GetModelMatrix(HRL_id _meshid, float *aa)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetModelMatrix: Invalid ID");
		return;
	}
	g_Backend.RHI_GetModelMatrix(it->second, aa);
}

//Debug

//Is Valid Functions
int HRL_IsValidMesh(HRL_id _id)
{
	auto it = ctx_.meshes.find(_id);
	if (it == ctx_.meshes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidLight(HRL_id _id)
{
	auto it = ctx_.lights.find(_id);
	if (it == ctx_.lights.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidVolumetricFog(HRL_id _id)
{
	auto it = ctx_.volumetric_fogs.find(_id);
	if (it == ctx_.volumetric_fogs.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidTexture(HRL_id _id)
{
	//Backend request
	return g_Backend.RHI_IsValidTexture(_id);
}

int HRL_IsValidScene(HRL_id _id)
{
	auto it = ctx_.scenes.find(_id);
	if (it == ctx_.scenes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidPostProcess(HRL_id _id)
{
	auto it = ctx_.post_processes.find(_id);
	if (it == ctx_.post_processes.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidShader(HRL_id _id)
{
	//Backend request
	return g_Backend.RHI_IsValidShader(_id);
}

int HRL_IsValidMaterial(HRL_id _id)
{
	auto it = ctx_.materials.find(_id);
	if (it == ctx_.materials.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidViewport(HRL_id _id)
{
	auto it = ctx_.viewports.find(_id);
	if (it == ctx_.viewports.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidCamera(HRL_id _id)
{
	auto it = ctx_.cameras.find(_id);
	if (it == ctx_.cameras.end())
	{
		return 0;
	}
	return 1;
}

int HRL_IsValidFont(HRL_id _id)
{
	auto it = ctx_.fonts.find(_id);
	if (it == ctx_.fonts.end())
	{
		return 0;
	}
	return 1;
}


void HRL_TakeScreenshot(HRL_id _sceneid, const char *_target_path)
{
	if (_target_path == nullptr || *_target_path == '\0')
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_TakeScreenshot: target path is empty");
		return;
	}
	if (ctx_.scenes.find(_sceneid) == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_TakeScreenshot: invalid scene ID");
		return;
	}
	ctx_.pending_screenshots[_sceneid] = _target_path;
}

void HRL_ReloadTexture(HRL_id _textureid, const char *_data, size_t _bufferSize)
{

}

void HRL_SetAntialiasingMode(HRL_uint _mode)
{
	if (_mode != HRL_ANTIALIASING_OFF && _mode != HRL_ANTIALIASING_2X &&
		_mode != HRL_ANTIALIASING_4X && _mode != HRL_ANTIALIASING_8X)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAntialiasingMode: expected OFF, 2X, 4X or 8X");
		return;
	}
	if (g_Backend.RHI_SetAntialiasingMode)
		g_Backend.RHI_SetAntialiasingMode((int)_mode);
}

void HRL_SetDefaultFramebuffer(HRL_uint _framebuffer)
{
#ifdef HRL_ENABLE_GLES3
	GLES3_SetDefaultFramebuffer(_framebuffer);
#else
	(void)_framebuffer;
#endif
}
