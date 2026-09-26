/**
 * Contient l'implémentation des fichiers :
 * hrl.h, hrl_gl.h, hrl_vulkan.h, hrl_d3d.h
 */

#include "hrl.h"
#include "hrl_gl.h"

#include "core/backend_vtable.h"
#include "core/object_types.h"
#include "core/utils_functions.h"
#include "core/widgets.h"

#include "backend/opengl33/gl33_backend.h"

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <new>
#include <cstdio>
#include <limits>
#include <set>
#include <tuple>
#include <cstring>

// Internal FBX decoding dependency. Add ufbx to the build; HRL only consumes
// its public header here and converts the decoded data to HRL_Vertex3D.
#include <ufbx/ufbx.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

//pour le texte
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>


static HRL_Context ctx_;

//vtable utilisée pour appeller les fonctions, ne doit jamais etre modifiée apres Init()
static HRL_vtable g_Backend;

/**
 * Owns the ufbx scene kept alive for the duration of the public FBX resource API.
 * HRL_FBXTextureInfo/HRL_FBXMaterialInfo intentionally expose borrowed strings/data
 * from this scene, so callers must keep this object alive while using those pointers.
 */
struct HRL_FBXResources
{
	ufbx_scene* scene = nullptr;
	std::vector<HRL_FBXTextureInfo> textures;
	std::vector<HRL_FBXMaterialInfo> materials;
	std::vector<const ufbx_material*> material_sources;
	std::unordered_map<const ufbx_texture*, HRL_uint> texture_indices;
};


HRL_Context* GetPrivateContext()
{
	return &ctx_;
}



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

static void UpdateSceneLights(HRL_id _scene)
{
	if (ctx_.scenes.find(_scene) == ctx_.scenes.end())
		return;

	g_Backend.RHI_UpdateLights(GetLightsVector(_scene));
}

static void MarkSceneGIGeometryDirty(hrl_scene_t* scene)
{
	if (!scene)
		return;
	++scene->gi_geometry_revision;
	scene->shadows_dirty = true;
}

static void MarkSceneGILightingDirty(hrl_scene_t* scene)
{
	if (scene)
		++scene->gi_lighting_revision;
}

static void MarkAllScenesGILightingDirty()
{
	for (auto& [sceneId, scene] : ctx_.scenes)
	{
		(void)sceneId;
		MarkSceneGILightingDirty(scene);
	}
}



/// API Implementation ///

void HRL_Init(HRL_E_APIs _api)
{
	if (_api != HRL_OPENGL_33)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_Init: selected backend is not implemented");
		return;
	}

	switch (_api)
	{
	case HRL_OPENGL_33 :
	{
		g_Backend = GetOpenGL33Backend();
		g_Backend.RHI_Init();
		break;
	}
	case HRL_OPENGL_45 :
	{
		break;
	}
	case HRL_VULKAN :
	{
		break;
	}
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
	g_Backend.RHI_InitContext(_width, _height, _loader);
}

void HRL_Shutdown()
{
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
	ctx_.lights.clear();
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

	g_Backend.RHI_Shutdown();
}

void HRL_BeginFrame()
{
	//g_Backend.RHI_BeginFrame();
}

void HRL_EndFrame()
{
	//appels à RHI_DrawMesh, HRI_BindMaterial, etc...
	for (const auto& [scene_id, scene] : ctx_.scenes)
	{
		if (!scene)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_EndFrame: Tried to bind an invalid scene");
			continue;
		}
		/**
				g_Backend.RHI_BindScene(scene_id);
				g_Backend.RHI_ClearScene();

				for (const auto& [id, viewport] : scene->viewports)
				{
					//camera can be nullptr, just continue if not initialized
					if (!viewport->camera_)
					{
						continue;
					}

					g_Backend.RHI_BindViewport(viewport);
					g_Backend.RHI_ComputeFrameMatrices();

					// --- Draw Sprites --- //
					for (const auto& sprite : GetSortedSprites(scene))
					{
						auto mat_it = ctx_.materials.find(sprite->material_);
						if (mat_it == ctx_.materials.end())
						{
							SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_EndFrame: tried to draw mesh, material not found");
							continue;
						}
						g_Backend.RHI_BindMaterial(mat_it->second);

						g_Backend.RHI_DrawMesh(sprite);
					}

					auto it_debug = ctx_.debug_renderers.find(scene_id);
					if (it_debug != ctx_.debug_renderers.end())
					{
						g_Backend.RHI_DrawDebug(it_debug->second, ctx_.debug_line_thickness);
					}

					//clear le debug a chaque frame
					ctx_.debug_renderers.clear();

					// --- Mettre ici le draw des mesh 3D --- //
				}
			}*/
		// Each scene owns its own light list. The backend UBO is shared by the
		// context, so refresh it immediately before rendering this scene.
		UpdateSceneLights(scene_id);
		g_Backend.RHI_RenderScene(scene, scene_id);

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
	//g_Backend.RHI_ResetFramebuffer();
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


//Meshes//
HRL_id HRL_CreateMeshSprite(HRL_id _sceneid)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateMeshSprite: invalid scene ID");
		return HRL_INVALID_ID;
	}
	//A sprite is the same internal object type as every other mesh.
	//Its geometry is a small 3D plane and its sprite-specific state is only the UV region.
	const HRL_Vertex3D vertices[4] = {
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}
	};
	const HRL_uint indices[6] = {0, 1, 2, 2, 3, 0};

	auto* m = new HRL_Mesh();
	m->scene_ = _sceneid;
	m->type_  = HRL_SPRITE;
	m->triangle_count_ = 2;
	m->bounds_center_ = glm::vec3(0.f);
	m->bounds_radius_ = std::sqrt(0.5f);

	HRL_id newId = GenerateHRL_ID();
	const bool spriteCreated = g_Backend.RHI_CreateSpriteMesh
		? (g_Backend.RHI_CreateSpriteMesh(newId) == HRL_TRUE)
		: (g_Backend.RHI_CreateMesh(newId, vertices, 4, indices, 6) == HRL_TRUE);
	if (!spriteCreated)
	{
		delete m;
		return HRL_INVALID_ID;
	}
	it_scene->second->meshes.emplace(newId, m);
	ctx_.meshes.emplace(newId, m);

	return newId;
}

void HRL_SetMeshPivotPoint(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshPivotPoint: invalid ID");
		return;
	}
	it->second->pivot_point_ = {x, y, z};
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_SetSpriteRegion(HRL_id _meshid, float min_u, float min_v, float max_u, float max_v)
{
	if (min_u > max_u || min_v > max_v)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: minimum cannot be greater than maximum");
		return;
	}
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPRITE)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: trying to set region on a non-sprite mesh");
		return;
	}
	it->second->region_[0] = min_u;
	it->second->region_[1] = min_v;
	it->second->region_[2] = max_u;
	it->second->region_[3] = max_v;
}

void HRL_DeleteMesh(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMesh: invalid ID");
		return;
	}

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->meshes.erase(_meshid);
		if (it->second->type_ != HRL_SPRITE)
			MarkSceneGIGeometryDirty(scene_it->second);
	}

	g_Backend.RHI_DeleteMesh(_meshid);
	delete it->second;
	ctx_.meshes.erase(it);
}

void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshMaterial: invalid ID");
		return;
	}
	it->second->material_ = _matid;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ == HRL_3D_MESH)
		MarkSceneGILightingDirty(scene_it->second);
}

void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshRotation: invalid ID");
		return;
	}
	//glm attend : X-pitch, Y-yaw, Z-roll.
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshScale: invalid ID");
		return;
	}
	it->second->scale_ = glm::vec3(x, y, z);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}


static glm::mat4 HRL_InternalMeshModelMatrix(const HRL_Mesh* mesh)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, mesh->position_);
	model = glm::translate(model, mesh->pivot_point_);
	model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::translate(model, -mesh->pivot_point_);
	model = glm::scale(model, mesh->scale_);
	return model;
}

float HRL_GetMeshCameraDistance(HRL_id _meshid)
{
	auto meshIt = ctx_.meshes.find(_meshid);
	if (meshIt == ctx_.meshes.end() || !meshIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: invalid mesh ID");
		return -1.f;
	}

	auto sceneIt = ctx_.scenes.find(meshIt->second->scene_);
	if (sceneIt == ctx_.scenes.end() || !sceneIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: mesh scene no longer exists");
		return -1.f;
	}

	const glm::mat4 model = HRL_InternalMeshModelMatrix(meshIt->second);
	const glm::vec3 center = glm::vec3(model * glm::vec4(meshIt->second->bounds_center_, 1.f));
	float minimumDistance = std::numeric_limits<float>::infinity();

	for (const auto& [viewportId, viewport] : sceneIt->second->viewports)
	{
		(void)viewportId;
		if (!viewport || !viewport->camera_)
			continue;
		const float distance = glm::length(center - viewport->camera_->position_);
		minimumDistance = std::min(minimumDistance, distance);
	}

	if (!std::isfinite(minimumDistance))
	{
		for (const auto& [cameraId, camera] : sceneIt->second->cameras)
		{
			(void)cameraId;
			if (!camera) continue;
			minimumDistance = std::min(minimumDistance, glm::length(center - camera->position_));
		}
	}

	if (!std::isfinite(minimumDistance))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetMeshCameraDistance: scene has no camera");
		return -1.f;
	}

	return minimumDistance;
}


namespace { static bool RebuildLODs(HRL_Mesh* mesh); }

static HRL_Mesh* GetMeshForLOD(HRL_id id, const char* errorMessage)
{
	auto it = ctx_.meshes.find(id);
	if (it == ctx_.meshes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, errorMessage);
		return nullptr;
	}
	if (it->second->type_ == HRL_SPRITE || it->second->type_ == HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"LOD generation is currently supported for static 3D meshes only");
		return nullptr;
	}
	return it->second;
}

static void MarkMeshSceneShadowsDirty(HRL_Mesh* mesh)
{
	if (!mesh) return;
	auto sceneIt = ctx_.scenes.find(mesh->scene_);
	if (sceneIt != ctx_.scenes.end())
		sceneIt->second->shadows_dirty = true;
}

void HRL_SetMeshLODAutomatic(HRL_id id, int enabled)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODAutomatic: invalid mesh ID");
	if (!mesh) return;
	mesh->lod_automatic_ = enabled != HRL_FALSE;
	mesh->last_lod_level_ = 0;
	if (mesh->lod_automatic_ && mesh->lods_.size() < mesh->lod_levels_)
		HRL_ForceMeshLODRebuild(id);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMode(HRL_id id, HRL_ELODMode mode)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMode: invalid mesh ID");
	if (!mesh) return;
	if (mode != HRL_LOD_DISTANCE && mode != HRL_LOD_SCREEN_SIZE)
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMode: invalid mode");
		return;
	}
	mesh->lod_mode_ = mode;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODLevels(HRL_id id, HRL_uint levels)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODLevels: invalid mesh ID");
	if (!mesh) return;
	if (levels < 1 || levels > 8)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODLevels: levels must be in [1,8]");
		return;
	}
	mesh->lod_levels_ = levels;
	HRL_ForceMeshLODRebuild(id);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance <= 0.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODDistance: expected finite value > 0");
		return;
	}
	if (distance >= mesh->lod_max_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODDistance: base distance must be smaller than max distance");
		return;
	}
	mesh->lod_base_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScale(HRL_id id, float scale)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScale: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(scale) || scale <= 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScale: expected finite value > 1");
		return;
	}
	mesh->lod_distance_scale_ = scale;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMinDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMinDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance < 0.0f || distance >= mesh->lod_max_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMinDistance: expected 0 <= value < max distance");
		return;
	}
	mesh->lod_min_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODMaxDistance(HRL_id id, float distance)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODMaxDistance: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(distance) || distance <= 0.0f || distance <= mesh->lod_min_distance_ || distance <= mesh->lod_base_distance_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODMaxDistance: expected value > min distance and base distance");
		return;
	}
	mesh->lod_max_distance_ = distance;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScreenThreshold(HRL_id id, float threshold)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScreenThreshold: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(threshold) || threshold <= 0.0f || threshold > 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScreenThreshold: expected value in (0,1]");
		return;
	}
	mesh->lod_screen_threshold_ = threshold;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODScreenScale(HRL_id id, float scale)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODScreenScale: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(scale) || scale <= 0.0f || scale >= 1.0f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODScreenScale: expected value in (0,1)");
		return;
	}
	mesh->lod_screen_scale_ = scale;
	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODHysteresis(HRL_id id, float hysteresis)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODHysteresis: invalid mesh ID");
	if (!mesh) return;
	if (!std::isfinite(hysteresis) || hysteresis < 0.0f || hysteresis > 0.49f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODHysteresis: expected value in [0,0.49]");
		return;
	}
	mesh->lod_hysteresis_ = hysteresis;
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_SetMeshLODOverride(HRL_id id, int level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_SetMeshLODOverride: invalid mesh ID");
	if (!mesh) return;
	if (level < -1 || level > 7)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetMeshLODOverride: expected -1 or [0,7]");
		return;
	}
	mesh->lod_override_ = level;
	mesh->last_lod_level_ = level < 0 ? 0 : std::min(level, (int)mesh->lods_.size() - 1);
	MarkMeshSceneShadowsDirty(mesh);
}

void HRL_ForceMeshLODRebuild(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_ForceMeshLODRebuild: invalid mesh ID");
	if (!mesh) return;
	if (!RebuildLODs(mesh))
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_ForceMeshLODRebuild: unable to generate LODs");
		return;
	}

	if (g_Backend.RHI_DeleteMeshLODs)
		g_Backend.RHI_DeleteMeshLODs(id);

	size_t uploaded = 1; // LOD 0 already exists in the GPU resource.
	for (size_t level = 1; level < mesh->lods_.size(); ++level)
	{
		auto& data = mesh->lods_[level];
		if (!g_Backend.RHI_CreateMeshLOD ||
			g_Backend.RHI_CreateMeshLOD(id, (HRL_uint)level,
				data.vertices.data(), data.vertices.size(),
				data.indices.data(), data.indices.size()) != HRL_TRUE)
		{
			SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
				"HRL_ForceMeshLODRebuild: failed to upload generated LOD");
			break;
		}
		++uploaded;
	}
	if (uploaded < mesh->lods_.size())
		mesh->lods_.resize(uploaded);

	mesh->last_lod_level_ = 0;
	MarkMeshSceneShadowsDirty(mesh);
}

HRL_uint HRL_GetMeshLODCount(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODCount: invalid mesh ID");
	return mesh ? (HRL_uint)mesh->lods_.size() : 0;
}

size_t HRL_GetMeshLODVertexCount(HRL_id id, HRL_uint level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODVertexCount: invalid mesh ID");
	if (!mesh) return 0;
	if (level >= mesh->lods_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetMeshLODVertexCount: invalid level");
		return 0;
	}
	return mesh->lods_[level].vertices.size();
}

size_t HRL_GetMeshLODTriangleCount(HRL_id id, HRL_uint level)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODTriangleCount: invalid mesh ID");
	if (!mesh) return 0;
	if (level >= mesh->lods_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetMeshLODTriangleCount: invalid level");
		return 0;
	}
	return mesh->lods_[level].indices.size() / 3u;
}

int HRL_GetMeshLODLevel(HRL_id id)
{
	HRL_Mesh* mesh = GetMeshForLOD(id, "HRL_GetMeshLODLevel: invalid mesh ID");
	return mesh ? mesh->last_lod_level_ : 0;
}

void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteDrawOrder: invalid ID");
		return;
	}
	it->second->draw_order_ = _draworder;
}



//Lights//
HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_ELightType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLight: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto* l = new HRL_Light();
	l->scene_ = _sceneid;
	l->type_ = _type;

	HRL_id newId = GenerateHRL_ID();
	l->id_ = newId;
	it_scene->second->lights.emplace(newId, l);
	ctx_.lights.emplace(newId, l);
	MarkSceneGILightingDirty(it_scene->second);

	UpdateSceneLights(_sceneid);

	return newId;
}

void HRL_DeleteLight(HRL_id _lightid)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLight: invalid ID");
		return;
	}

	const HRL_id owner_scene = it->second->scene_;
	g_Backend.RHI_DeleteLight(_lightid);
	auto scene_it = ctx_.scenes.find(owner_scene);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->lights.erase(_lightid);
		MarkSceneGILightingDirty(scene_it->second);
		scene_it->second->shadows_dirty = true;
	}

	delete it->second;
	ctx_.lights.erase(it);

	UpdateSceneLights(owner_scene);
}

void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightColor: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->color_ = glm::vec4(x, y, z, 0.f);

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightIntensity(HRL_id _lightid, float i)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightIntensity: invalid ID");
		return;
	}
	it->second->intensity_ = i;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightAttenuation(HRL_id _lightid, float a)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightAttenuation: invalid ID");
		return;
	}
	it->second->attenuation_ = a;

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
		MarkSceneGILightingDirty(scene_it->second);

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightLocation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->position_ = glm::vec4(x, y, z, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightRotation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->rotation_ = glm::vec4(pitch, yaw, roll, 0.f);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightCastShadows(HRL_id _lightid, int _enable)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightCastShadows: invalid ID");
		return;
	}
	it->second->cast_shadows_ = (_enable != HRL_FALSE);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }
	UpdateSceneLights(it->second->scene_);
}

void HRL_SetLightShadowBias(HRL_id _lightid, float _bias)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: invalid ID");
		return;
	}
	if (!std::isfinite(_bias) || _bias < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowBias: bias must be finite and non-negative");
		return;
	}
	it->second->shadow_bias_ = _bias;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
	{
		MarkSceneGILightingDirty(scene_it->second);
		scene_it->second->shadows_dirty = true;
	}
}

void HRL_SetLightShadowResolution(HRL_id _lightid, int _resolution)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: invalid ID");
		return;
	}
	if (_resolution < 128 || (_resolution & (_resolution - 1)) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLightShadowResolution: resolution must be a power of two and at least 128");
		return;
	}
	it->second->shadow_resolution_ = _resolution;
}

void HRL_SetSpotLightInnerCutoff(HRL_id _lightid, float inner_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightInnerCutoff: light is not of type SpotLight");
		return;
	}

	it->second->innerCutoff = inner_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}

void HRL_SetSpotLightOuterCutoff(HRL_id _lightid, float outer_cutoff)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: invalid ID");
		return;
	}
	if (it->second->type_ != HRL_SPOT_LIGHT)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpotLightOuterCutoff: light is not of type SpotLight");
		return;
	}

	it->second->outerCutoff = outer_cutoff;
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end()) { MarkSceneGILightingDirty(scene_it->second); scene_it->second->shadows_dirty = true; }

	UpdateSceneLights(it->second->scene_);
}




//Textures//
HRL_id HRL_CreateTexture(const char* _fileContent, size_t _bufferSize)
{
	//gestion des erreurs auto par le backend
	return g_Backend.RHI_CreateTexture(_fileContent, _bufferSize);
}
void HRL_DeleteTexture(HRL_id _textureid)
{
	g_Backend.RHI_DeleteTexture(_textureid);
	MarkAllScenesGILightingDirty();
}


void HRL_GetTextureSize(HRL_id _textureid, int *_width, int *_height)
{
	if (_width && _height)
	{
		g_Backend.RHI_GetTextureSize(_textureid, _width, _height);
	}
	else
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetTextureSize: width or height are not a valid pointer");
	}
}

void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMinFilter(_textureid, _filter);
}
void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_EFilterType _filter)
{
	g_Backend.RHI_SetTextureMaxFilter(_textureid, _filter);
}

HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
	float _font_size, float _wrap_width,
	float r, float g, float b,
	float bg_r, float bg_g, float bg_b, float bg_a
)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: invalid font ID");
		return HRL_INVALID_ID;
	}

	BitmapResult bmp = GenerateBitmap(_text, &it->second->info, it->second->ttf_buffer,
			_font_size, _wrap_width,
			r, g, b,
			bg_r, bg_g, bg_b, bg_a
	);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: bitmap generation failed, pixels is empty");
		return HRL_INVALID_ID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}


void HRL_ClearScreen()
{
	/**for (const auto& s : ctx_.scenes)
	{
		if (!s.second)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_ClearScreen: tried to clear an invalid scene object");
			return;
		}
		g_Backend.RHI_BindScene(s.first);
		g_Backend.RHI_ResetFramebuffer();
	}*/
}



//Scenes//
HRL_id HRL_CreateScene(int _renderOnScreen)
{
	auto* scene = new hrl_scene_t();
	scene->draw_on_screen = _renderOnScreen;

	HRL_id newId = GenerateHRL_ID();
	ctx_.scenes.emplace(newId, scene);

	g_Backend.RHI_CreateScene(newId, _renderOnScreen);
	return newId;


}

void HRL_DeleteScene(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteScene: invalid scene ID");
		return;
	}

	//on collecte les IDs d'abord pour eviter l'invalidation d'iterateur
	std::vector<HRL_id> mesh_ids, light_ids, viewport_ids, camera_ids;
	for (const auto& [id, mesh]     : it->second->meshes)     mesh_ids.push_back(id);
	for (const auto& [id, light]    : it->second->lights)     light_ids.push_back(id);
	for (const auto& [id, viewport] : it->second->viewports)  viewport_ids.push_back(id);
	for (const auto& [id, camera]   : it->second->cameras)    camera_ids.push_back(id);

	//delete every objects that ows the scene
	for (auto id : mesh_ids)     HRL_DeleteMesh(id);
	for (auto id : light_ids)    HRL_DeleteLight(id);
	for (auto id : viewport_ids) HRL_DeleteViewport(id);
	for (auto id : camera_ids)   HRL_DeleteCamera(id);

	delete it->second;

	ctx_.pending_screenshots.erase(_sceneid);
	g_Backend.RHI_DeleteScene(_sceneid);

	ctx_.scenes.erase(it);
}


static bool IsValidGlobalIlluminationMethod(HRL_EGlobalIlluminationMethod method)
{
	return method >= HRL_GI_NONE && method <= HRL_GI_RAY_TRACING;
}

void HRL_SetGlobalIlluminationEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationEnabled: invalid scene ID");
		return;
	}

	if (_enable == HRL_FALSE)
	{
		it->second->global_illumination_enabled = false;
		return;
	}

	if (it->second->global_illumination_method == HRL_GI_NONE)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationEnabled: scene GI method is HRL_GI_NONE");
		return;
	}

	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported ||
		!g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)it->second->global_illumination_method))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationEnabled: selected GI method is not supported by the active backend");
		return;
	}

	it->second->global_illumination_enabled = true;
}

void HRL_SetGlobalIlluminationMethod(HRL_id _sceneid, HRL_EGlobalIlluminationMethod _method)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationMethod: invalid scene ID");
		return;
	}

	if (!IsValidGlobalIlluminationMethod(_method))
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationMethod: invalid GI method");
		return;
	}

	if (_method == HRL_GI_NONE)
	{
		it->second->global_illumination_method = HRL_GI_NONE;
		it->second->global_illumination_enabled = false;
		return;
	}

	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported ||
		!g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)_method))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalIlluminationMethod: selected GI method is not supported by the active backend");
		return;
	}

	it->second->global_illumination_method = _method;
}

int HRL_IsGlobalIlluminationEnabled(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_IsGlobalIlluminationEnabled: invalid scene ID");
		return HRL_FALSE;
	}
	return it->second->global_illumination_enabled ? HRL_TRUE : HRL_FALSE;
}

HRL_EGlobalIlluminationMethod HRL_GetGlobalIlluminationMethod(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetGlobalIlluminationMethod: invalid scene ID");
		return HRL_GI_NONE;
	}
	return it->second->global_illumination_method;
}

int HRL_IsGlobalIlluminationMethodSupported(HRL_EGlobalIlluminationMethod _method)
{
	if (!IsValidGlobalIlluminationMethod(_method) || _method == HRL_GI_NONE)
		return HRL_FALSE;
	if (!g_Backend.RHI_IsGlobalIlluminationMethodSupported)
		return HRL_FALSE;
	return g_Backend.RHI_IsGlobalIlluminationMethodSupported((int)_method) ? HRL_TRUE : HRL_FALSE;
}

uint32_t HRL_GetGlobalIlluminationSupportedMethods()
{
	return g_Backend.RHI_GetGlobalIlluminationSupportedMethods
		? g_Backend.RHI_GetGlobalIlluminationSupportedMethods()
		: 0u;
}

void HRL_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	g_Backend.RHI_ResizeSceneTexture(_sceneid, _width, _height);
}

void HRL_EnableColorPickingBuffer(HRL_id _sceneid, int _enable)
{
	g_Backend.RHI_EnableColorPickingBuffer(_sceneid, _enable);
}

void HRL_SetSkySphereEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereEnabled: invalid scene ID");
		return;
	}
	it->second->sky_sphere_enabled = (_enable != HRL_FALSE);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereColors(
	HRL_id _sceneid,
	float top_r, float top_g, float top_b,
	float horizon_r, float horizon_g, float horizon_b,
	float bottom_r, float bottom_g, float bottom_b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereColors: invalid scene ID");
		return;
	}

	it->second->sky_top_color = glm::vec3(top_r, top_g, top_b);
	it->second->sky_horizon_color = glm::vec3(horizon_r, horizon_g, horizon_b);
	it->second->sky_bottom_color = glm::vec3(bottom_r, bottom_g, bottom_b);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereRotation(HRL_id _sceneid, float pitch, float yaw, float roll)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereRotation: invalid scene ID");
		return;
	}
	it->second->sky_rotation = glm::vec3(pitch, yaw, roll);
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetSkySphereTexture(HRL_id _sceneid, HRL_id _textureid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSkySphereTexture: invalid scene ID");
		return;
	}
	it->second->sky_texture = _textureid;
	MarkSceneGILightingDirty(it->second);
}

void HRL_SetEnvironmentMappingEnabled(HRL_id _sceneid, int _enable)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetEnvironmentMappingEnabled: invalid scene ID");
		return;
	}
	it->second->environment_mapping_enabled = (_enable != HRL_FALSE);
}

void HRL_SetEnvironmentMap(HRL_id _sceneid, HRL_id _textureid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetEnvironmentMap: invalid scene ID");
		return;
	}
	it->second->environment_texture = _textureid;
}


//Post Process//
HRL_id HRL_CreatePostProcess(HRL_id _viewport, HRL_id _matid, int priority)
{
	auto it_viewport = ctx_.viewports.find(_viewport);
	if (it_viewport == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid viewport ID");
		return HRL_INVALID_ID;
	}

	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid material ID");
		return HRL_INVALID_ID;
	}

	auto pp = new HRL_PostProcess();
	pp->id_ = GenerateHRL_ID();
	pp->material_ = _matid;
	pp->priority_ = priority;

	HRL_id newId = pp->id_;

	ctx_.post_processes.emplace(newId, pp);
	it_viewport->second->post_processes.emplace(newId, pp);

	g_Backend.RHI_CreatePostProcess(_matid, priority);

	return newId;
}
void HRL_DeletePostProcess(HRL_id _postid)
{
	auto it = ctx_.post_processes.find(_postid);
	if (it == ctx_.post_processes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeletePostProcess: invalid ID");
		return;
	}

	// Remove all viewport references before freeing the object.
	for (auto& [viewport_id, viewport] : ctx_.viewports)
	{
		(void)viewport_id;
		if (viewport) viewport->post_processes.erase(_postid);
	}

	g_Backend.RHI_DeletePostProcess(_postid);
	delete it->second;
	ctx_.post_processes.erase(it);
}




//Shaders//
HRL_id HRL_CreateShader(const char *_vertContent, size_t _vertSize, const char *_fragContent, size_t _fragSize)
{
	//on return directement l'id, le backend gere les erreurs et retourne InvalidID en cas d'erreur
	return g_Backend.RHI_CreateShader(_vertContent, _vertSize, _fragContent, _fragSize);
}

void HRL_DeleteShader(HRL_id _shaderid)
{
	g_Backend.RHI_DeleteShader(_shaderid);
}


//Materials//
HRL_id HRL_CreateMaterial(HRL_id _shaderid)
{
	auto* m = new HRL_Material();
	m->shader_ = _shaderid;

	HRL_id newId = GenerateHRL_ID();
	ctx_.materials.emplace(newId, m);

	return newId;
}

void HRL_DeleteMaterial(HRL_id _matid)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMaterial: invalid ID");
		return;
	}
	if (it->second)
	{
		for (HRL_id textureId : it->second->owned_textures_)
		{
			if (textureId != HRL_INVALID_ID)
				HRL_DeleteTexture(textureId);
		}
	}
	delete it->second;
	ctx_.materials.erase(it);
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetInt: invalid ID");
		return;
	}
	//si la clée n'existe pas, elle est créée
	it->second->intParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid)
{
	auto it_mat = ctx_.materials.find(_matid);
	if (it_mat == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetTexture: invalid material ID");
		return;
	}

	//on vérifie que la texture existe au moment de RHI_BindMaterial, car ici on a pas acces aux textures
	it_mat->second->textureParams_[_uniformName] = _textureid;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetBool: invalid ID");
		return;
	}
	it->second->intParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetFloat: invalid ID");
		return;
	}
	it->second->floatParams_[_uniformName] = a;
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec2: invalid ID");
		return;
	}
	it->second->vec2Params_[_uniformName] = glm::vec2(x, y);
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec3: invalid ID");
		return;
	}
	it->second->vec3Params_[_uniformName] = glm::vec3(x, y, z);
	MarkAllScenesGILightingDirty();
}

void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec4: invalid ID");
		return;
	}
	it->second->vec4Params_[_uniformName] = glm::vec4(x, y, z, w);
	MarkAllScenesGILightingDirty();
}



HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid scene ID");
		return HRL_INVALID_ID;
	}

	auto it = ctx_.cameras.find(_cameraid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid camera ID");
		return HRL_INVALID_ID;
	}
	HRL_Camera* cam = it->second;

	if (cam->scene_ != _sceneid)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateViewport: camera belongs to another scene");
		return HRL_INVALID_ID;
	}

	auto* v = new HRL_Viewport(cam, x, y, _width, _height);

	HRL_id newId = GenerateHRL_ID();
	v->scene_ = _sceneid;
	it_scene->second->viewports.emplace(newId, v);
	ctx_.viewports.emplace(newId, v);

	return newId;
}

void HRL_DeleteViewport(HRL_id _viewportid)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteViewport: invalid ID");
		return;
	}

	// Destroy viewport-owned resources before the viewport itself.
	std::vector<HRL_id> postIds;
	for (const auto& [id, pp] : it->second->post_processes)
	{ (void)pp; postIds.push_back(id); }
	for (HRL_id id : postIds) HRL_DeletePostProcess(id);

	std::vector<HRL_id> widgetIds;
	for (const auto& [id, widget] : it->second->widgets)
	{ (void)widget; widgetIds.push_back(id); }
	for (HRL_id id : widgetIds) HRL_DeleteWidget(id);

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->viewports.find(_viewportid);
		if (sit != scene->viewports.end())
		{
			scene->viewports.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.viewports.erase(it);
}

void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid)
{
	auto viewport_it = ctx_.viewports.find(_viewportid);
	if (viewport_it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid viewport ID");
		return;
	}

	auto cam_it = ctx_.cameras.find(_camid);
	if (cam_it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid camera ID");
		return;
	}
	if (viewport_it->second->scene_ != HRL_INVALID_ID &&
		cam_it->second->scene_ != viewport_it->second->scene_)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: camera belongs to another scene");
		return;
	}

	viewport_it->second->camera_ = cam_it->second;
}

void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height)
{
	auto it = ctx_.viewports.find(_viewportid);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportRect: invalid ID");
		return;
	}
	it->second->x_      = x;
	it->second->y_      = y;
	it->second->width_  = _width;
	it->second->height_ = _height;
}



HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_ECameraType _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (_type == HRL_ORTHO || _type == HRL_PERSPECTIVE)
	{
		auto* cam = new HRL_Camera(
			//type, position, rotation
			_type,
			glm::vec3(1.f),
			glm::vec3(0.f),

			//fov vertical, near plane, far plane
			1000.f,
			0.01f,
			1000.f
			);
		HRL_id newId = GenerateHRL_ID();
		cam->scene_ = _sceneid;
		it_scene->second->cameras.emplace(newId, cam);
		ctx_.cameras.emplace(newId, cam);
		return newId;
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid camera type");
		return HRL_INVALID_ID;
	}
}

void HRL_DeleteCamera(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteCamera: invalid ID");
		return;
	}

	// Invalidate every viewport reference before deleting the camera.
	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport && viewport->camera_ == it->second)
			viewport->camera_ = nullptr;
	}

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->cameras.find(_camid);
		if (sit != scene->cameras.end())
		{
			scene->cameras.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.cameras.erase(it);
}


void HRL_SetCameraType(HRL_id _camid, HRL_ECameraType _type)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraType: invalid ID");
		return;
	}
	it->second->type_ = _type;
}

void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraOrthoVertical: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_ORTHO)
	{
		it->second->value_ = _height;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraOrthoVertical: camera is not of type Ortho");
	}
}

void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraPerspectiveFov: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_PERSPECTIVE)
	{
		it->second->value_ = _fov;
	}
	else
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_WARNING, "HRL_SetCameraPerspectiveFov: camera is not of type Perspective");
	}
}

void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraNearPlane: invalid ID");
		return;
	}
	it->second->near_plane_ = _nearPlane;
}

void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraFarPlane: invalid ID");
		return;
	}
	it->second->far_plane_ = _farPlane;
}

void HRL_SetCameraLocation(HRL_id _camid, float x, float y, float z)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
}

void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraRotation: invalid ID");
		return;
	}
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
}


//EFFECTS
//BLOOM

//FOG
void HRL_SetFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogEnabled: invalid scene ID");
		return;
	}
	it->second->fog.enabled = enable;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogMode(HRL_id scene, HRL_EFogType mode)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogMode: invalid scene ID");
		return;
	}
	it->second->fog.mode = mode;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.r = r;
	it->second->fog.g = g;
	it->second->fog.b = b;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.density = density;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogLinearRange(HRL_id scene, float start, float end)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.range_start = start;
	it->second->fog.range_end = end;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetVolumetricFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogEnabled: invalid scene ID"); return; }
	it->second->volumetric_fog.enabled = (enable != HRL_FALSE);
}

void HRL_SetVolumetricFogPosition(HRL_id scene, float x, float y, float z)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogPosition: invalid scene ID"); return; }
	it->second->volumetric_fog.position = glm::vec3(x, y, z);
}

void HRL_SetVolumetricFogRadius(HRL_id scene, float radius)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogRadius: invalid scene ID"); return; }
	if (!std::isfinite(radius) || radius <= 0.f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogRadius: radius must be > 0"); return; }
	it->second->volumetric_fog.radius = radius;
}

void HRL_SetVolumetricFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogDensity: invalid scene ID"); return; }
	if (!std::isfinite(density) || density < 0.f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogDensity: density must be >= 0"); return; }
	it->second->volumetric_fog.density = density;
}

void HRL_SetVolumetricFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogColor: invalid scene ID"); return; }
	it->second->volumetric_fog.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetVolumetricFogSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricFogSteps: invalid scene ID"); return; }
	it->second->volumetric_fog.steps = std::max<HRL_uint>(4u, std::min<HRL_uint>(64u, steps));
}

void HRL_SetGodRaysEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysEnabled: invalid scene ID"); return; }
	it->second->god_rays.enabled = (enable != HRL_FALSE);
}

void HRL_SetGodRaysPosition(HRL_id scene, float x, float y, float z)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysPosition: invalid scene ID"); return; }
	it->second->god_rays.position = glm::vec3(x, y, z);
}

void HRL_SetGodRaysColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysColor: invalid scene ID"); return; }
	it->second->god_rays.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetGodRaysDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDensity: invalid scene ID"); return; }
	it->second->god_rays.density = glm::clamp(density, 0.f, 2.f);
}

void HRL_SetGodRaysDecay(HRL_id scene, float decay)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDecay: invalid scene ID"); return; }
	it->second->god_rays.decay = glm::clamp(decay, 0.8f, 0.999f);
}

void HRL_SetGodRaysWeight(HRL_id scene, float weight)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysWeight: invalid scene ID"); return; }
	it->second->god_rays.weight = glm::clamp(weight, 0.f, 4.f);
}

void HRL_SetGodRaysSamples(HRL_id scene, HRL_uint samples)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysSamples: invalid scene ID"); return; }
	it->second->god_rays.samples = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, samples));
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

void HRL_SetDebugLineThickness(float a)
{
	ctx_.debug_line_thickness = a;
}

void HRL_DrawDebugSegment(HRL_id _sceneid, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugSegment: invalid scene ID");
		return;
	}

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x, a_y, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x, b_y, b_z, r, g, b);
}

void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_EDebugRenderingType _mode, const float *vertices_x, const float *vertices_y, const float *vertices_z, int vertices_count, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid scene ID");
		return;
	}

	if (_mode == HRL_DEBUG_SOLID)
	{
		for (int i=1; i < vertices_count-1; i++)
		{
			//pivot
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
			//i
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			//i+1
			ctx_.debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}
	}
	else if (_mode == HRL_DEBUG_HOLLOW)
	{
		for (int i=0; i < vertices_count - 1; i++)
		{
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}

		//line entre le dernier et le 0 pour refermer
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[vertices_count-1], vertices_y[vertices_count-1], vertices_z[vertices_count-1], r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
	}
	else
	{
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid mode, expected HRL_DebugSolid or HRL_DebugHollow");
	}
}

void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float center_x, float center_y, float center_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCircle: invalid scene ID"); return; }

	int seg = segments;
	auto* vx = (float*)alloca(seg * sizeof(float));
	auto* vy = (float*)alloca(seg * sizeof(float));
	auto* vz = (float*)alloca(seg * sizeof(float));

	for (int i = 0; i < seg; i++)
	{
		float angle = (float)(2.f * M_PI * i) / seg;
		vx[i] = center_x + radius * cosf(angle);
		vy[i] = center_y + radius * sinf(angle);
		vz[i] = center_z;
	}

	HRL_DrawDebugPolygon(_sceneid, _mode, vx, vy, vz, seg, r, g, b);
}

void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_EDebugRenderingType _mode, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCapsule: invalid scene ID"); return; }

	int half_seg = segments / 2;

	// direction A→B
	float dx = b_x - a_x;
	float dy = b_y - a_y;
	float len = sqrtf(dx * dx + dy * dy);

	// vecteur unitaire perpendiculaire
	float nx = 0.f, ny = 0.f;
	if (len > 1e-6f) { nx = -dy / len; ny = dx / len; }

	// angle de l'axe A→B
	float base_angle = atan2f(dy, dx);

	// demi-cercle autour de B — face opposée à A
	// plage : [base_angle - PI/2 → base_angle + PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle - (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a0), b_y + radius * sinf(a0), b_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a1), b_y + radius * sinf(a1), b_z, r, g, b);
	}

	// demi-cercle autour de A — face opposée à B
	// plage : [base_angle + PI/2 → base_angle + 3PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a0), a_y + radius * sinf(a0), a_z, r, g, b);
		ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a1), a_y + radius * sinf(a1), a_z, r, g, b);
	}

	// deux segments latéraux reliant les demi-cercles
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + nx * radius, a_y + ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x + nx * radius, b_y + ny * radius, b_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - nx * radius, a_y - ny * radius, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x - nx * radius, b_y - ny * radius, b_z, r, g, b);

}

void HRL_DrawDebugPoint(HRL_id _sceneid, float a_x, float a_y, float a_z, float size, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPoint: invalid scene ID"); return; }

	float h = size * 0.5f;

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x - h, a_y,     a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x + h, a_y,     a_z, r, g, b);

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y - h, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y + h, a_z, r, g, b);
}


//Text//
HRL_id HRL_CreateFont(const char *data, size_t _data_size)
{
	HRL_id newId = GenerateHRL_ID();
	auto* font = new HRL_Font();

	font->ttf_buffer.assign(data, data+_data_size);

	int ok = stbtt_InitFont(
		&font->info,
		font->ttf_buffer.data(),
		stbtt_GetFontOffsetForIndex(font->ttf_buffer.data(), 0)
	);

	if (!ok)
	{
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_CreateFont: failed to parse TTF data");
		delete font;
		return HRL_INVALID_ID;
	}

	ctx_.fonts.emplace(newId, font);

	return newId;
}

void HRL_DeleteFont(HRL_id _fontid)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteFont: invalid ID");
		return;
	}
	delete it->second;
	ctx_.fonts.erase(it);
}





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

void HRL_MaterialSetEmissiveColor(HRL_id matid, float r, float g, float b, float a)
{

}

HRL_id HRL_CreateMesh3D(HRL_id _sceneid, const HRL_Vertex3D* _vertices, size_t _vertexCount, const HRL_uint* _indices, size_t _indexCount)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (!_vertices || _vertexCount == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertices must be non-null and vertex count must be greater than zero");
		return HRL_INVALID_ID;
	}

	if (_indexCount == 0 && (_vertexCount % 3) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: non-indexed vertex count must be a multiple of 3");
		return HRL_INVALID_ID;
	}

	if (_indexCount > 0)
	{
		if (!_indices || (_indexCount % 3) != 0)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: indexed meshes require a non-null index array and an index count multiple of 3");
			return HRL_INVALID_ID;
		}
		for (size_t i = 0; i < _indexCount; ++i)
		{
			if ((size_t)_indices[i] >= _vertexCount)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: index references a vertex outside the vertex array");
				return HRL_INVALID_ID;
			}
		}
	}

	for (size_t i = 0; i < _vertexCount; ++i)
	{
		const HRL_Vertex3D& v = _vertices[i];
		for (float value : v.position)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex position contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.normal)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex normal contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.uv)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex UV contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.tangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex tangent contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : v.bitangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateMesh3D: vertex bitangent contains NaN or infinity"); return HRL_INVALID_ID; }
	}

	HRL_id newId = GenerateHRL_ID();
	if (g_Backend.RHI_CreateMesh(newId, _vertices, _vertexCount, _indices, _indexCount) != HRL_TRUE)
	{
		return HRL_INVALID_ID;
	}

	auto* mesh = new HRL_Mesh();
	mesh->scene_ = _sceneid;
	mesh->type_ = HRL_3D_MESH;
	mesh->triangle_count_ = _indexCount > 0 ? (_indexCount / 3u) : (_vertexCount / 3u);

	HRL_MeshLODData baseLOD;
	baseLOD.vertices.assign(_vertices, _vertices + _vertexCount);
	if (_indexCount > 0) baseLOD.indices.assign(_indices, _indices + _indexCount);
	else {
		baseLOD.indices.resize(_vertexCount);
		for (size_t i = 0; i < _vertexCount; ++i) baseLOD.indices[i] = (HRL_uint)i;
	}
	mesh->lods_.push_back(std::move(baseLOD));

	glm::vec3 minPoint(std::numeric_limits<float>::max());
	glm::vec3 maxPoint(std::numeric_limits<float>::lowest());
	bool validBounds = true;
	for (size_t i = 0; i < _vertexCount; ++i)
	{
		const glm::vec3 p(_vertices[i].position[0], _vertices[i].position[1], _vertices[i].position[2]);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
		{
			validBounds = false;
			break;
		}
		minPoint = glm::min(minPoint, p);
		maxPoint = glm::max(maxPoint, p);
	}
	if (validBounds)
	{
		mesh->bounds_center_ = (minPoint + maxPoint) * 0.5f;
		float radius2 = 0.f;
		for (size_t i = 0; i < _vertexCount; ++i)
		{
			const glm::vec3 p(_vertices[i].position[0], _vertices[i].position[1], _vertices[i].position[2]);
			radius2 = std::max(radius2, glm::dot(p - mesh->bounds_center_, p - mesh->bounds_center_));
		}
		mesh->bounds_radius_ = std::sqrt(radius2);
	}
	it_scene->second->meshes.emplace(newId, mesh);
	MarkSceneGIGeometryDirty(it_scene->second);
	ctx_.meshes.emplace(newId, mesh);

	return newId;
}

namespace {

static glm::vec3 LODPosition(const HRL_Vertex3D& v) { return glm::vec3(v.position[0],v.position[1],v.position[2]); }
static glm::vec3 LODNormal(const HRL_Vertex3D& v) { return glm::vec3(v.normal[0],v.normal[1],v.normal[2]); }
static glm::vec3 LODTangent(const HRL_Vertex3D& v) { return glm::vec3(v.tangent[0],v.tangent[1],v.tangent[2]); }
static glm::vec3 LODBitangent(const HRL_Vertex3D& v) { return glm::vec3(v.bitangent[0],v.bitangent[1],v.bitangent[2]); }
static glm::vec3 LODSafeNormalize(const glm::vec3& v,const glm::vec3& fallback){float l2=glm::dot(v,v);if(!std::isfinite(l2)||l2<1e-10f)return fallback;return v/std::sqrt(l2);}
static void LODSetVertex(HRL_Vertex3D& d,const glm::vec3&p,const glm::vec3&n,const glm::vec2&uv,const glm::vec3&t,const glm::vec3&b){d.position[0]=p.x;d.position[1]=p.y;d.position[2]=p.z;d.normal[0]=n.x;d.normal[1]=n.y;d.normal[2]=n.z;d.uv[0]=uv.x;d.uv[1]=uv.y;d.tangent[0]=t.x;d.tangent[1]=t.y;d.tangent[2]=t.z;d.bitangent[0]=b.x;d.bitangent[1]=b.y;d.bitangent[2]=b.z;}

static bool GenerateLODLevel(const HRL_MeshLODData& source, float ratio, HRL_MeshLODData& out)
{
	out.vertices.clear();
	out.indices.clear();
	if (source.vertices.size() < 3 || source.indices.size() < 3)
		return false;

	const double r = std::clamp((double)ratio, 0.01, 1.0);
	const size_t target = std::max<size_t>(
		3,
		std::min(source.vertices.size(),
			(size_t)std::llround((double)source.vertices.size() * r)));
	if (target >= source.vertices.size())
	{
		out = source;
		return true;
	}

	glm::vec3 minP(std::numeric_limits<float>::max());
	glm::vec3 maxP(std::numeric_limits<float>::lowest());
	for (const auto& v : source.vertices)
	{
		const glm::vec3 p = LODPosition(v);
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
			return false;
		minP = glm::min(minP, p);
		maxP = glm::max(maxP, p);
	}

	const glm::vec3 extent = glm::max(maxP - minP, glm::vec3(1e-5f));
	const double e[3] = { extent.x, extent.y, extent.z };
	int dims[3] = { 1, 1, 1 };
	bool fixed[3] = { false, false, false };

	// Allocate the target number of spatial cells according to object aspect
	// ratio. Thin geometry (planes/lines) keeps its thin axis at one cell rather
	// than wasting the 3D grid budget on empty volume.
	for (;;)
	{
		double activeProduct = 1.0;
		int activeCount = 0;
		double activeExtentProduct = 1.0;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!fixed[axis])
			{
				++activeCount;
				activeExtentProduct *= e[axis];
			}
		}
		(void)activeProduct;
		if (activeCount == 0)
			break;

		const double scale = std::pow((double)target / std::max(activeExtentProduct, 1e-30), 1.0 / activeCount);
		bool clampedAxis = false;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (fixed[axis])
				continue;
			const double raw = scale * e[axis];
			if (raw < 1.0)
			{
				dims[axis] = 1;
				fixed[axis] = true;
				clampedAxis = true;
			}
		}
		if (!clampedAxis)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!fixed[axis])
					dims[axis] = std::max(1, (int)std::lround(scale * e[axis]));
			}
			break;
		}
	}

	// Make the actual grid product close to the requested cell count. Rounding
	// can leave a few cells unused or create slightly more cells than requested;
	// that is intentional and keeps aspect ratio stable.
	const size_t maxDim = (size_t)std::numeric_limits<int>::max();
	for (int axis = 0; axis < 3; ++axis)
		dims[axis] = std::clamp(dims[axis], 1, (int)std::min(target, maxDim));

	struct Accumulator
	{
		glm::vec3 p{0}, n{0}, t{0}, b{0};
		glm::vec2 uv{0};
		uint32_t count = 0;
	};

	struct CellKey
	{
		int x, y, z;
		bool operator==(const CellKey& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};
	struct CellKeyHash
	{
		size_t operator()(const CellKey& key) const noexcept
		{
			size_t h = std::hash<int>{}(key.x);
			h ^= std::hash<int>{}(key.y) + (h << 6) + (h >> 2);
			h ^= std::hash<int>{}(key.z) + (h << 6) + (h >> 2);
			return h;
		}
	};

	std::unordered_map<CellKey, uint32_t, CellKeyHash> cells;
	std::vector<Accumulator> acc;
	std::vector<uint32_t> map(source.vertices.size());
	cells.reserve(std::min(target * 2u, source.vertices.size() * 2u));
	acc.reserve(std::min(target, source.vertices.size()));

	auto key = [&](const glm::vec3& p)
	{
		const glm::vec3 q = (p - minP) / extent;
		const int x = std::clamp((int)std::floor(q.x * dims[0]), 0, dims[0] - 1);
		const int y = std::clamp((int)std::floor(q.y * dims[1]), 0, dims[1] - 1);
		const int z = std::clamp((int)std::floor(q.z * dims[2]), 0, dims[2] - 1);
		return CellKey{x, y, z};
	};

	for (size_t i = 0; i < source.vertices.size(); ++i)
	{
		const auto& v = source.vertices[i];
		auto [it, inserted] = cells.emplace(key(LODPosition(v)), (uint32_t)acc.size());
		if (inserted)
			acc.emplace_back();
		map[i] = it->second;
		Accumulator& a = acc[it->second];
		a.p += LODPosition(v);
		a.n += LODNormal(v);
		a.t += LODTangent(v);
		a.b += LODBitangent(v);
		a.uv += glm::vec2(v.uv[0], v.uv[1]);
		++a.count;
	}

	out.vertices.resize(acc.size());
	for (size_t i = 0; i < acc.size(); ++i)
	{
		const auto& a = acc[i];
		const float inv = 1.0f / std::max(1u, a.count);
		const glm::vec3 n = LODSafeNormalize(a.n * inv, {0, 1, 0});
		glm::vec3 t = a.t * inv;
		t -= n * glm::dot(n, t);
		t = LODSafeNormalize(t, {1, 0, 0});
		const glm::vec3 b = LODSafeNormalize(glm::cross(n, t), {0, 0, 1});
		LODSetVertex(out.vertices[i], a.p * inv, n, a.uv * inv, t, b);
	}

	std::set<std::tuple<HRL_uint, HRL_uint, HRL_uint>> seen;
	out.indices.reserve(source.indices.size());
	for (size_t i = 0; i + 2 < source.indices.size(); i += 3)
	{
		const HRL_uint a = map[source.indices[i]];
		const HRL_uint b = map[source.indices[i + 1]];
		const HRL_uint c = map[source.indices[i + 2]];
		if (a == b || b == c || a == c)
			continue;

		HRL_uint x = a, y = b, z = c;
		if (x > y) std::swap(x, y);
		if (y > z) std::swap(y, z);
		if (x > y) std::swap(x, y);
		if (!seen.emplace(x, y, z).second)
			continue;

		out.indices.push_back(a);
		out.indices.push_back(b);
		out.indices.push_back(c);
	}

	return out.indices.size() >= 3 && out.vertices.size() >= 3;
}

static bool RebuildLODs(HRL_Mesh* mesh)
{
	if(!mesh||mesh->type_==HRL_SPRITE||mesh->lods_.empty())return false;const size_t desired=std::max<size_t>(1,mesh->lod_levels_);HRL_MeshLODData base=mesh->lods_[0];mesh->lods_.clear();mesh->lods_.push_back(std::move(base));for(size_t level=1;level<desired;++level){HRL_MeshLODData lod;if(!GenerateLODLevel(mesh->lods_[0],std::pow(0.5f,(float)level),lod))break;if(lod.indices.size()>=mesh->lods_.back().indices.size())break;mesh->lods_.push_back(std::move(lod));}mesh->last_lod_level_=std::clamp(mesh->last_lod_level_,0,(int)mesh->lods_.size()-1);return true;
}

static glm::vec3 HRLFBX_ToVec3(const ufbx_vec3& v)
{
	return glm::vec3((float)v.x, (float)v.y, (float)v.z);
}

static glm::vec2 HRLFBX_ToVec2(const ufbx_vec2& v)
{
	return glm::vec2((float)v.x, (float)v.y);
}

static glm::vec3 HRLFBX_TransformPosition(const ufbx_matrix& matrix, const ufbx_vec3& value)
{
	return HRLFBX_ToVec3(ufbx_transform_position(&matrix, value));
}

static glm::vec3 HRLFBX_TransformDirection(const ufbx_matrix& matrix, const ufbx_vec3& value)
{
	return HRLFBX_ToVec3(ufbx_transform_direction(&matrix, value));
}

static glm::vec3 HRLFBX_NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback)
{
	const float length2 = glm::dot(value, value);
	if (!std::isfinite(length2) || length2 <= 0.0000001f)
		return fallback;
	return glm::normalize(value);
}

static void HRLFBX_BuildFallbackTangentSpace(
	const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
	const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2,
	const glm::vec3& normal,
	glm::vec3& tangent,
	glm::vec3& bitangent)
{
	const glm::vec3 edge1 = p1 - p0;
	const glm::vec3 edge2 = p2 - p0;
	const glm::vec2 duv1 = uv1 - uv0;
	const glm::vec2 duv2 = uv2 - uv0;

	const float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
	if (std::isfinite(determinant) && std::fabs(determinant) > 0.000001f)
	{
		const float inv = 1.0f / determinant;
		tangent = (edge1 * duv2.y - edge2 * duv1.y) * inv;
		bitangent = (edge2 * duv1.x - edge1 * duv2.x) * inv;
	}
	else
	{
		const glm::vec3 reference = std::fabs(normal.y) < 0.99f
			? glm::vec3(0.f, 1.f, 0.f)
			: glm::vec3(1.f, 0.f, 0.f);
		tangent = glm::cross(reference, normal);
		bitangent = glm::cross(normal, tangent);
	}

	tangent = HRLFBX_NormalizeOrFallback(tangent, glm::vec3(1.f, 0.f, 0.f));
	tangent = HRLFBX_NormalizeOrFallback(
		tangent - normal * glm::dot(normal, tangent),
		glm::vec3(1.f, 0.f, 0.f));
	bitangent = HRLFBX_NormalizeOrFallback(bitangent, glm::cross(normal, tangent));
}

static bool HRLFBX_AppendMesh(
	std::vector<HRL_Vertex3D>& output,
	const ufbx_mesh* mesh,
	const ufbx_matrix& geometry_to_world)
{
	if (!mesh)
		return false;

	const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&geometry_to_world);

	if (mesh->num_triangles > 0)
		output.reserve(output.size() + mesh->num_triangles * 3);

	if (mesh->max_face_triangles == 0)
		return false;

	std::vector<uint32_t> triangleIndices((size_t)mesh->max_face_triangles * 3u);

	// HRL_Vertex3D has no material information, so the converter deliberately
	// walks the complete polygon list instead of splitting by FBX material parts.
	// ufbx_triangulate_face() returns the number of triangles, not the number of
	// written indices.
	for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
	{
		const ufbx_face& face = mesh->faces.data[faceIndex];
		if (face.num_indices < 3)
			continue;

		const uint32_t triangleCount = ufbx_triangulate_face(
			triangleIndices.data(), triangleIndices.size(), mesh, face);
		if (triangleCount == 0)
			continue;

		for (uint32_t tri = 0; tri < triangleCount; ++tri)
		{
			const size_t triangleBase = (size_t)tri * 3u;
			HRL_Vertex3D vertices[3]{};
			glm::vec3 positions[3];
			glm::vec3 normals[3];
			glm::vec2 uvs[3];
			glm::vec3 tangents[3];
			glm::vec3 bitangents[3];

			for (size_t corner = 0; corner < 3; ++corner)
			{
				const uint32_t index = triangleIndices[triangleBase + corner];
				if ((size_t)index >= mesh->num_indices)
				{
					positions[corner] = glm::vec3(0.f);
					normals[corner] = glm::vec3(0.f);
					uvs[corner] = glm::vec2(0.f);
					tangents[corner] = glm::vec3(0.f);
					bitangents[corner] = glm::vec3(0.f);
					continue;
				}

				ufbx_vec3 position{};
				if (mesh->vertex_position.exists)
				{
					position = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
				}
				else
				{
					const uint32_t vertexIndex = mesh->vertex_indices.data[index];
					if ((size_t)vertexIndex >= mesh->vertices.count)
						return false;
					position = mesh->vertices.data[vertexIndex];
				}

				positions[corner] = HRLFBX_TransformPosition(geometry_to_world, position);

				if (mesh->vertex_normal.exists)
				{
					normals[corner] = HRLFBX_TransformDirection(
						normalMatrix,
						ufbx_get_vertex_vec3(&mesh->vertex_normal, index));
				}
				else
				{
					normals[corner] = glm::vec3(0.f);
				}

				uvs[corner] = mesh->vertex_uv.exists
					? HRLFBX_ToVec2(ufbx_get_vertex_vec2(&mesh->vertex_uv, index))
					: glm::vec2(0.f);

				tangents[corner] = mesh->vertex_tangent.exists
					? HRLFBX_TransformDirection(
						geometry_to_world,
						ufbx_get_vertex_vec3(&mesh->vertex_tangent, index))
					: glm::vec3(0.f);

				bitangents[corner] = mesh->vertex_bitangent.exists
					? HRLFBX_TransformDirection(
						geometry_to_world,
						ufbx_get_vertex_vec3(&mesh->vertex_bitangent, index))
					: glm::vec3(0.f);
			}

			const glm::vec3 faceNormal = HRLFBX_NormalizeOrFallback(
				glm::cross(positions[1] - positions[0], positions[2] - positions[0]),
				glm::vec3(0.f, 1.f, 0.f));

			for (size_t corner = 0; corner < 3; ++corner)
				normals[corner] = HRLFBX_NormalizeOrFallback(normals[corner], faceNormal);

			glm::vec3 fallbackTangent;
			glm::vec3 fallbackBitangent;
			HRLFBX_BuildFallbackTangentSpace(
				positions[0], positions[1], positions[2],
				uvs[0], uvs[1], uvs[2], normals[0],
				fallbackTangent, fallbackBitangent);

			for (size_t corner = 0; corner < 3; ++corner)
			{
				glm::vec3 tangent = mesh->vertex_tangent.exists
					? tangents[corner] : fallbackTangent;
				tangent = HRLFBX_NormalizeOrFallback(tangent, fallbackTangent);
				tangent = HRLFBX_NormalizeOrFallback(
					tangent - normals[corner] * glm::dot(normals[corner], tangent),
					fallbackTangent);

				const glm::vec3 expectedBitangent = glm::cross(normals[corner], tangent);
				glm::vec3 bitangent = mesh->vertex_bitangent.exists
					? bitangents[corner] : fallbackBitangent;
				bitangent = HRLFBX_NormalizeOrFallback(bitangent, expectedBitangent);
				if (glm::dot(expectedBitangent, bitangent) < 0.f)
					bitangent = -expectedBitangent;

				vertices[corner].position[0] = positions[corner].x;
				vertices[corner].position[1] = positions[corner].y;
				vertices[corner].position[2] = positions[corner].z;
				vertices[corner].normal[0] = normals[corner].x;
				vertices[corner].normal[1] = normals[corner].y;
				vertices[corner].normal[2] = normals[corner].z;
				vertices[corner].uv[0] = uvs[corner].x;
				vertices[corner].uv[1] = uvs[corner].y;
				vertices[corner].tangent[0] = tangent.x;
				vertices[corner].tangent[1] = tangent.y;
				vertices[corner].tangent[2] = tangent.z;
				vertices[corner].bitangent[0] = bitangent.x;
				vertices[corner].bitangent[1] = bitangent.y;
				vertices[corner].bitangent[2] = bitangent.z;
			}

			output.push_back(vertices[0]);
			output.push_back(vertices[1]);
			output.push_back(vertices[2]);
		}
	}

	return true;
}

}

HRL_Vertex3D* HRL_GetVertex3DFromFBX(const char* _data, size_t _bufferSize, size_t* _vertexCount)
{
	if (_vertexCount)
		*_vertexCount = 0;

	if (!_data || _bufferSize == 0 || !_vertexCount)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_GetVertex3DFromFBX: data, buffer size and vertex count output must be valid");
		return nullptr;
	}

	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.generate_missing_normals = true;
	options.normalize_normals = true;
	options.load_external_files = true;
	options.ignore_missing_external_files = true;
	options.evaluate_skinning = true;
	options.evaluate_caches = true;
	options.ignore_geometry = false;
	options.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
	options.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
	options.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
	options.target_unit_meters = 1.0;

	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_GetVertex3DFromFBX: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	HRL_Vertex3D* result = nullptr;
	try
	{
		std::vector<HRL_Vertex3D> vertices;
		size_t meshesWithFaces = 0;
		size_t meshesWithTriangles = 0;
		size_t meshParts = 0;
		size_t totalFaces = 0;
		size_t totalTriangles = 0;

		std::unordered_set<const ufbx_mesh*> convertedMeshes;

		// Convert every mesh attached to a node. Visibility is a rendering
		// concern and must not prevent extraction from an FBX file.
		for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
		{
			const ufbx_node* node = scene->nodes.data[nodeIndex];
			if (!node || !node->mesh)
				continue;

			const ufbx_mesh* mesh = node->mesh;
			convertedMeshes.insert(mesh);
			meshesWithFaces += mesh->num_faces > 0 ? 1 : 0;
			meshesWithTriangles += mesh->num_triangles > 0 ? 1 : 0;
			totalFaces += mesh->num_faces;
			totalTriangles += mesh->num_triangles;
			meshParts += mesh->material_parts.count;

			HRLFBX_AppendMesh(vertices, mesh, node->geometry_to_world);
		}

		// Handle mesh elements which have no node connection.
		for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex)
		{
			const ufbx_mesh* mesh = scene->meshes.data[meshIndex];
			if (!mesh || convertedMeshes.find(mesh) != convertedMeshes.end())
				continue;

			meshesWithFaces += mesh->num_faces > 0 ? 1 : 0;
			meshesWithTriangles += mesh->num_triangles > 0 ? 1 : 0;
			totalFaces += mesh->num_faces;
			totalTriangles += mesh->num_triangles;
			meshParts += mesh->material_parts.count;

			HRLFBX_AppendMesh(vertices, mesh, ufbx_identity_matrix);
		}

		if (vertices.empty())
		{
			char detail[512];
			snprintf(detail, sizeof(detail),
				"HRL_GetVertex3DFromFBX: no convertible polygon geometry "
				"(meshes=%zu, nodes=%zu, meshes_with_faces=%zu, meshes_with_triangles=%zu, "
				"mesh_parts=%zu, faces=%zu, triangles=%zu)",
				scene->meshes.count, scene->nodes.count,
				meshesWithFaces, meshesWithTriangles, meshParts, totalFaces, totalTriangles);
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail);
			return nullptr;
		}

		result = new (std::nothrow) HRL_Vertex3D[vertices.size()];
		if (!result)
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
				"HRL_GetVertex3DFromFBX: failed to allocate vertex buffer");
			return nullptr;
		}

		std::copy(vertices.begin(), vertices.end(), result);
		*_vertexCount = vertices.size();
	}
	catch (const std::bad_alloc&)
	{
		delete[] result;
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_GetVertex3DFromFBX: failed to allocate temporary conversion data");
		return nullptr;
	}

	ufbx_free_scene(scene);
	return result;
}

void HRL_FreeVertex3DFromFBX(HRL_Vertex3D* _vertices)
{
	delete[] _vertices;
}

namespace {

static HRL_SkeletalBoneTransform HRLSkeletal_TransformFromUFBX(const ufbx_transform& transform)
{
	HRL_SkeletalBoneTransform result{};
	result.translation[0] = (float)transform.translation.x;
	result.translation[1] = (float)transform.translation.y;
	result.translation[2] = (float)transform.translation.z;
	result.rotation[0] = (float)transform.rotation.x;
	result.rotation[1] = (float)transform.rotation.y;
	result.rotation[2] = (float)transform.rotation.z;
	result.rotation[3] = (float)transform.rotation.w;
	result.scale[0] = (float)transform.scale.x;
	result.scale[1] = (float)transform.scale.y;
	result.scale[2] = (float)transform.scale.z;
	return result;
}

static glm::mat4 HRLSkeletal_TransformToMatrix(const HRL_SkeletalBoneTransform& transform)
{
	const glm::vec3 translation(transform.translation[0], transform.translation[1], transform.translation[2]);
	const glm::quat rotation(
		transform.rotation[3],
		transform.rotation[0],
		transform.rotation[1],
		transform.rotation[2]);
	const glm::vec3 scale(transform.scale[0], transform.scale[1], transform.scale[2]);
	glm::mat4 matrix(1.f);
	matrix = glm::translate(matrix, translation);
	matrix *= glm::mat4_cast(rotation);
	matrix = glm::scale(matrix, scale);
	return matrix;
}

static glm::mat4 HRLSkeletal_UFBXMatrixToGLM(const ufbx_matrix& source)
{
	glm::mat4 result(1.f);
	result[0] = glm::vec4((float)source.cols[0].x, (float)source.cols[0].y, (float)source.cols[0].z, 0.f);
	result[1] = glm::vec4((float)source.cols[1].x, (float)source.cols[1].y, (float)source.cols[1].z, 0.f);
	result[2] = glm::vec4((float)source.cols[2].x, (float)source.cols[2].y, (float)source.cols[2].z, 0.f);
	result[3] = glm::vec4((float)source.cols[3].x, (float)source.cols[3].y, (float)source.cols[3].z, 1.f);
	return result;
}

static glm::mat4 HRLSkeletal_ArrayToGLM(const float source[16])
{
	glm::mat4 result(1.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = source[col * 4 + row];
	return result;
}

static glm::mat4 HRLSkeletal_LerpMatrix(const glm::mat4& a, const glm::mat4& b, float alpha)
{
	const float invAlpha = 1.0f - alpha;
	glm::mat4 result(0.f);
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			result[col][row] = a[col][row] * invAlpha + b[col][row] * alpha;
	return result;
}

static void HRLSkeletal_SetDefaultTransform(HRL_SkeletalBoneTransform& transform)
{
	transform.translation[0] = 0.f;
	transform.translation[1] = 0.f;
	transform.translation[2] = 0.f;
	transform.rotation[0] = 0.f;
	transform.rotation[1] = 0.f;
	transform.rotation[2] = 0.f;
	transform.rotation[3] = 1.f;
	transform.scale[0] = 1.f;
	transform.scale[1] = 1.f;
	transform.scale[2] = 1.f;
}

static HRL_SkeletalBoneTransform HRLSkeletal_LerpTransform(
	const HRL_SkeletalBoneTransform& a,
	const HRL_SkeletalBoneTransform& b,
	float alpha)
{
	HRL_SkeletalBoneTransform result{};
	const glm::vec3 ta(a.translation[0], a.translation[1], a.translation[2]);
	const glm::vec3 tb(b.translation[0], b.translation[1], b.translation[2]);
	const glm::vec3 sa(a.scale[0], a.scale[1], a.scale[2]);
	const glm::vec3 sb(b.scale[0], b.scale[1], b.scale[2]);
	const glm::quat qa(a.rotation[3], a.rotation[0], a.rotation[1], a.rotation[2]);
	const glm::quat qb(b.rotation[3], b.rotation[0], b.rotation[1], b.rotation[2]);
	const glm::vec3 t = glm::mix(ta, tb, alpha);
	const glm::vec3 s = glm::mix(sa, sb, alpha);
	const glm::quat q = glm::normalize(glm::slerp(qa, qb, alpha));

	result.translation[0] = t.x;
	result.translation[1] = t.y;
	result.translation[2] = t.z;
	result.rotation[0] = q.x;
	result.rotation[1] = q.y;
	result.rotation[2] = q.z;
	result.rotation[3] = q.w;
	result.scale[0] = s.x;
	result.scale[1] = s.y;
	result.scale[2] = s.z;
	return result;
}

static void HRLSkeletal_BuildPoseFramesForAnimation(
    HRL_SkeletalMesh* mesh,
    HRL_SkeletalAnimationInternal& animation)
{
    if (!mesh || mesh->bones_.empty() || animation.public_.frameCount == 0 || animation.frames_.empty())
    {
        animation.pose_frames_.clear();
        return;
    }

    const size_t boneCount = mesh->bones_.size();
    const size_t frameCount = animation.public_.frameCount;
    animation.pose_frames_.assign(frameCount * boneCount, glm::mat4(1.f));

    if (animation.pose_matrices_storage_.size() == frameCount * boneCount * 16u)
    {
        for (size_t frame = 0; frame < frameCount; ++frame)
        {
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
            {
                const float* src = animation.pose_matrices_storage_.data() + (frame * boneCount + boneIndex) * 16u;
                animation.pose_frames_[frame * boneCount + boneIndex] = HRLSkeletal_ArrayToGLM(src);
            }
        }
        return;
    }

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        std::vector<glm::mat4> world(boneCount, glm::mat4(1.f));
        std::vector<unsigned char> built(boneCount, 0);

        std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
        {
            if (built[boneIndex])
                return world[boneIndex];

            const HRL_SkeletalBoneInternal& bone = mesh->bones_[boneIndex];
            const size_t offset = frame * boneCount + boneIndex;
            glm::mat4 local = HRLSkeletal_TransformToMatrix(animation.frames_[offset]);
            glm::mat4 parentWorld(1.f);
            if (bone.public_.parentIndex < boneCount && bone.public_.parentIndex != boneIndex)
                parentWorld = buildBone((size_t)bone.public_.parentIndex);

            world[boneIndex] = parentWorld * local;
            built[boneIndex] = 1;
            return world[boneIndex];
        };

        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex)
        {
            const glm::mat4 boneWorld = buildBone(boneIndex);
            animation.pose_frames_[frame * boneCount + boneIndex] =
                boneWorld * mesh->bones_[boneIndex].inverse_bind_;
        }
    }
}

static bool HRLSkeletal_UpdatePose(HRL_SkeletalMesh* mesh)
{
	if (!mesh || mesh->bones_.empty())
		return false;

	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	const HRL_SkeletalAnimationInternal* animation = nullptr;
	if (mesh->current_animation_ >= 0 && (size_t)mesh->current_animation_ < mesh->animations_.size())
		animation = &mesh->animations_[(size_t)mesh->current_animation_];

	if (animation && !animation->pose_frames_.empty() && animation->public_.frameCount > 0)
	{
		const float duration = std::max(animation->public_.duration, 0.f);
		float time = std::clamp(mesh->animation_time_, 0.f, duration);
		const float framePosition = duration > 0.f
			? time * animation->public_.frameRate
			: 0.f;
		const size_t frame0 = std::min((size_t)std::floor(framePosition), animation->public_.frameCount - 1);
		const size_t frame1 = std::min(frame0 + 1, animation->public_.frameCount - 1);
		const float alpha = frame1 == frame0 ? 0.f : framePosition - (float)frame0;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
		{
			const size_t offset0 = frame0 * mesh->bones_.size() + boneIndex;
			const size_t offset1 = frame1 * mesh->bones_.size() + boneIndex;
			mesh->bone_matrices_[boneIndex] = HRLSkeletal_LerpMatrix(animation->pose_frames_[offset0], animation->pose_frames_[offset1], alpha);
		}
	}
	else if (!mesh->animations_.empty() && !mesh->animations_.front().pose_frames_.empty())
	{
		// Imported skeletal meshes keep their bind/first-frame skin matrices in
		// world-space form, matching ufbx's geometry_to_world skinning convention.
		const auto& firstPose = mesh->animations_.front().pose_frames_;
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = firstPose[boneIndex];
	}
	else
	{
		for (size_t boneIndex = 0; boneIndex < mesh->bones_.size(); ++boneIndex)
			mesh->bone_matrices_[boneIndex] = mesh->bones_[boneIndex].bind_world_ * mesh->bones_[boneIndex].inverse_bind_;
	}

	++mesh->pose_serial_;
	if (mesh->pose_serial_ == 0)
		mesh->pose_serial_ = 1;
	return true;
}

static char* HRLSkeletal_CopyCString(const ufbx_string& string)
{
	char* result = new char[string.length + 1];
	if (string.length > 0)
		std::memcpy(result, string.data, string.length);
	result[string.length] = '\0';
	return result;
}

static void HRLSkeletal_DestroyMeshData(HRL_SkeletalMeshData* data)
{
	if (!data) return;
	if (data->bones)
	{
		for (size_t i = 0; i < data->boneCount; ++i)
			delete[] const_cast<char*>(data->bones[i].name);
		delete[] data->bones;
	}
	if (data->animations)
	{
		for (size_t i = 0; i < data->animationCount; ++i)
		{
			delete[] const_cast<char*>(data->animations[i].name);
			delete[] data->animations[i].frames;
			delete[] data->animations[i].poseMatrices;
		}
		delete[] data->animations;
	}
	delete[] data->vertices;
	delete[] data->indices;
	delete data;
}

static bool HRLSkeletal_FillVertex(
	HRL_SkeletalVertex& output,
	const ufbx_mesh* mesh,
	const ufbx_skin_deformer* skin,
	const std::vector<HRL_uint>& clusterToBone,
	uint32_t meshIndex)
{
	std::memset(&output, 0, sizeof(output));
	const uint32_t vertexIndex = mesh->vertex_indices.data[meshIndex];
	if ((size_t)vertexIndex >= mesh->vertices.count)
		return false;

	ufbx_vec3 position = mesh->vertices.data[vertexIndex];
	if (mesh->vertex_position.exists)
		position = ufbx_get_vertex_vec3(&mesh->vertex_position, meshIndex);
	output.vertex.position[0] = (float)position.x;
	output.vertex.position[1] = (float)position.y;
	output.vertex.position[2] = (float)position.z;

	if (mesh->vertex_normal.exists)
	{
		const ufbx_vec3 normal = ufbx_get_vertex_vec3(&mesh->vertex_normal, meshIndex);
		output.vertex.normal[0] = (float)normal.x;
		output.vertex.normal[1] = (float)normal.y;
		output.vertex.normal[2] = (float)normal.z;
	}
	else
	{
		output.vertex.normal[1] = 1.f;
	}

	if (mesh->vertex_uv.exists)
	{
		const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, meshIndex);
		output.vertex.uv[0] = (float)uv.x;
		output.vertex.uv[1] = (float)uv.y;
	}

	if (mesh->vertex_tangent.exists)
	{
		const ufbx_vec3 tangent = ufbx_get_vertex_vec3(&mesh->vertex_tangent, meshIndex);
		output.vertex.tangent[0] = (float)tangent.x;
		output.vertex.tangent[1] = (float)tangent.y;
		output.vertex.tangent[2] = (float)tangent.z;
	}
	if (mesh->vertex_bitangent.exists)
	{
		const ufbx_vec3 bitangent = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, meshIndex);
		output.vertex.bitangent[0] = (float)bitangent.x;
		output.vertex.bitangent[1] = (float)bitangent.y;
		output.vertex.bitangent[2] = (float)bitangent.z;
	}

	if (!mesh->vertex_tangent.exists || !mesh->vertex_bitangent.exists)
	{
		HRL_Vertex3D tri[3]{};
		// Tangent fallback is completed at the triangle level by the caller. The
		// zeroed tangent space here makes the absence explicit without inventing UVs.
		(void)tri;
	}

	if (skin && (size_t)vertexIndex < skin->vertices.count)
	{
		const ufbx_skin_vertex& skinVertex = skin->vertices.data[vertexIndex];
		const size_t weightEnd = std::min(
			(size_t)skinVertex.weight_begin + (size_t)skinVertex.num_weights,
			skin->weights.count);

		struct Influence {
			HRL_uint bone;
			float weight;
		};
		std::vector<Influence> influences;
		influences.reserve((size_t)skinVertex.num_weights);
		for (size_t weightIndex = skinVertex.weight_begin; weightIndex < weightEnd; ++weightIndex)
		{
			const ufbx_skin_weight& weight = skin->weights.data[weightIndex];
			if ((size_t)weight.cluster_index >= clusterToBone.size())
				continue;
			const HRL_uint boneIndex = clusterToBone[weight.cluster_index];
			if (boneIndex == HRL_INVALID_ID)
				continue;
			const float w = (float)weight.weight;
			if (!std::isfinite(w) || w <= 0.f)
				continue;
			influences.push_back({boneIndex, w});
		}

		std::sort(influences.begin(), influences.end(), [](const Influence& a, const Influence& b) {
			return a.weight > b.weight;
		});

		const size_t influenceCount = std::min(influences.size(), (size_t)HRL_SKELETAL_MAX_INFLUENCES);
		float weightSum = 0.f;
		for (size_t i = 0; i < influenceCount; ++i)
		{
			output.boneIndices[i] = influences[i].bone;
			output.boneWeights[i] = influences[i].weight;
			weightSum += influences[i].weight;
		}
		if (weightSum > 0.f)
		{
			for (size_t i = 0; i < influenceCount; ++i)
				output.boneWeights[i] /= weightSum;
		}
		else if (skin->clusters.count > 0 && !clusterToBone.empty())
		{
			output.boneIndices[0] = clusterToBone[0] == HRL_INVALID_ID ? 0u : clusterToBone[0];
			output.boneWeights[0] = 1.f;
		}
	}
	return true;
}

static void HRLSkeletal_ComputeMissingTangentSpace(
	HRL_SkeletalVertex& a, HRL_SkeletalVertex& b, HRL_SkeletalVertex& c)
{
	const glm::vec3 p0(a.vertex.position[0], a.vertex.position[1], a.vertex.position[2]);
	const glm::vec3 p1(b.vertex.position[0], b.vertex.position[1], b.vertex.position[2]);
	const glm::vec3 p2(c.vertex.position[0], c.vertex.position[1], c.vertex.position[2]);
	const glm::vec2 uv0(a.vertex.uv[0], a.vertex.uv[1]);
	const glm::vec2 uv1(b.vertex.uv[0], b.vertex.uv[1]);
	const glm::vec2 uv2(c.vertex.uv[0], c.vertex.uv[1]);
	glm::vec3 normalA(a.vertex.normal[0], a.vertex.normal[1], a.vertex.normal[2]);
	const glm::vec3 faceNormal = HRLFBX_NormalizeOrFallback(glm::cross(p1-p0, p2-p0), glm::vec3(0,1,0));
	for (glm::vec3* n : {&normalA})
		*n = HRLFBX_NormalizeOrFallback(*n, faceNormal);

	glm::vec3 tangent, bitangent;
	HRLFBX_BuildFallbackTangentSpace(p0,p1,p2,uv0,uv1,uv2,normalA,tangent,bitangent);
	for (HRL_SkeletalVertex* v : {&a,&b,&c})
	{
		glm::vec3 n(v->vertex.normal[0], v->vertex.normal[1], v->vertex.normal[2]);
		n = HRLFBX_NormalizeOrFallback(n, faceNormal);
		v->vertex.normal[0]=n.x; v->vertex.normal[1]=n.y; v->vertex.normal[2]=n.z;
		glm::vec3 t(v->vertex.tangent[0], v->vertex.tangent[1], v->vertex.tangent[2]);
		if (glm::dot(t,t) < 1e-8f) t=tangent;
		t = HRLFBX_NormalizeOrFallback(t - n*glm::dot(n,t), tangent);
		glm::vec3 bt(v->vertex.bitangent[0], v->vertex.bitangent[1], v->vertex.bitangent[2]);
		const glm::vec3 expected = glm::normalize(glm::cross(n,t));
		if (glm::dot(bt,bt) < 1e-8f) bt=expected;
		bt = HRLFBX_NormalizeOrFallback(bt, expected);
		if (glm::dot(bt, expected) < 0.f) bt=-expected;
		v->vertex.tangent[0]=t.x; v->vertex.tangent[1]=t.y; v->vertex.tangent[2]=t.z;
		v->vertex.bitangent[0]=bt.x; v->vertex.bitangent[1]=bt.y; v->vertex.bitangent[2]=bt.z;
	}
}

} // anonymous namespace

HRL_SkeletalMeshData* HRL_GetSkeletalMeshFromFBX(const char* _data, size_t _bufferSize)
{
	if (!_data || _bufferSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: data and buffer size must be valid");
		return nullptr;
	}

	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.generate_missing_normals = true;
	options.normalize_normals = true;
	options.load_external_files = true;
	options.ignore_missing_external_files = true;
	options.evaluate_skinning = true;
	options.evaluate_caches = false;
	options.ignore_geometry = false;
	options.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
	options.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
	options.target_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
	options.target_unit_meters = 1.0;

	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_GetSkeletalMeshFromFBX: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	// A FBX file may contain several meshes attached to the same skeleton.
	// This API represents a single HRL skeletal mesh, so pick the largest skinned
	// geometry instead of depending on FBX object ordering (which often puts
	// small accessory meshes before the main character). The diagnostic below
	// makes the limitation explicit instead of silently hiding it.
	const ufbx_mesh* mesh = nullptr;
	size_t skinnedMeshCount = 0;
	size_t largestTriangles = 0;
	for (size_t i = 0; i < scene->meshes.count; ++i)
	{
		const ufbx_mesh* candidate = scene->meshes.data[i];
		if (!candidate || candidate->skin_deformers.count == 0 || candidate->num_triangles == 0)
			continue;
		++skinnedMeshCount;
		if (!mesh || candidate->num_triangles > largestTriangles)
		{
			mesh = candidate;
			largestTriangles = candidate->num_triangles;
		}
	}
	if (skinnedMeshCount > 1)
	{
		std::printf(
			"HRL_GetSkeletalMeshFromFBX: FBX contains %zu skinned meshes; importing the largest (%zu triangles) with the single-mesh API.\n",
			skinnedMeshCount, largestTriangles);
	}
	if (!mesh)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: FBX scene contains no skinned mesh geometry");
		return nullptr;
	}

	const ufbx_node* meshNode = nullptr;
	for (size_t nodeIndex = 0; nodeIndex < scene->nodes.count; ++nodeIndex)
	{
		const ufbx_node* candidate = scene->nodes.data[nodeIndex];
		if (candidate && candidate->mesh == mesh)
		{
			meshNode = candidate;
			break;
		}
	}
	if (!meshNode && mesh->instances.count > 0)
		meshNode = mesh->instances.data[0];

	const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
	if (!skin || skin->clusters.count == 0)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: mesh has no skin clusters");
		return nullptr;
	}

	try
	{
		std::unordered_map<const ufbx_node*, bool> nodeSet;
		std::unordered_map<const ufbx_node*, const ufbx_skin_cluster*> clusterByNode;
		std::vector<const ufbx_node*> boneNodes;
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node)
				continue;
			clusterByNode.emplace(cluster->bone_node, cluster);
			for (ufbx_node* node = cluster->bone_node; node; node = node->parent)
			{
				if (!nodeSet.emplace(node, true).second)
					break;
				boneNodes.push_back(node);
			}
		}
		if (boneNodes.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skin has no valid bone nodes");
			return nullptr;
		}
		std::stable_sort(boneNodes.begin(), boneNodes.end(), [](const ufbx_node* a, const ufbx_node* b){
			if (a->node_depth != b->node_depth) return a->node_depth < b->node_depth;
			if (a->name.length != b->name.length) return a->name.length < b->name.length;
			return std::strncmp(a->name.data, b->name.data, a->name.length) < 0;
		});
		if (boneNodes.size() > HRL_MAX_SKELETAL_BONES)
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skeleton exceeds HRL_MAX_SKELETAL_BONES");
			return nullptr;
		}

		std::unordered_map<const ufbx_node*, HRL_uint> nodeToBone;
		for (size_t i = 0; i < boneNodes.size(); ++i)
			nodeToBone[boneNodes[i]] = (HRL_uint)i;

		std::vector<HRL_uint> clusterToBone(skin->clusters.count, HRL_INVALID_ID);
		for (size_t i = 0; i < skin->clusters.count; ++i)
		{
			const ufbx_skin_cluster* cluster = skin->clusters.data[i];
			if (!cluster || !cluster->bone_node) continue;
			auto it = nodeToBone.find(cluster->bone_node);
			if (it != nodeToBone.end()) clusterToBone[i] = it->second;
		}

		// Keep mesh vertices in ufbx's native geometry space. IMPORTANT: the
		// skinning matrices themselves are world-space matrices. This is the same
		// convention used by the official ufbx viewer: for each influence,
		// currentBoneWorld * geometry_to_bone is applied directly to the geometry
		// vertex. Therefore the HRL object/model transform must NOT apply the FBX
		// geometry_to_world a second time.

		std::vector<HRL_SkeletalVertex> vertices;
		std::vector<HRL_uint> indices;
		vertices.reserve(mesh->num_triangles * 3u);
		indices.reserve(mesh->num_triangles * 3u);
		const size_t triangleCapacity = (size_t)std::max<uint32_t>(1u, mesh->max_face_triangles) * 3u;
		std::vector<uint32_t> triangles(triangleCapacity);
		for (size_t faceIndex = 0; faceIndex < mesh->faces.count; ++faceIndex)
		{
			const ufbx_face& face = mesh->faces.data[faceIndex];
			if (face.num_indices < 3) continue;
			const uint32_t triangleCount = ufbx_triangulate_face(triangles.data(), triangles.size(), mesh, face);
			for (uint32_t tri = 0; tri < triangleCount; ++tri)
			{
				HRL_SkeletalVertex triVertices[3]{};
				bool valid = true;
				for (size_t corner = 0; corner < 3; ++corner)
				{
					const uint32_t meshIndex = triangles[(size_t)tri * 3u + corner];
					if ((size_t)meshIndex >= mesh->num_indices ||
						!HRLSkeletal_FillVertex(triVertices[corner], mesh, skin, clusterToBone, meshIndex))
					{
						valid = false;
						break;
					}
				}
				if (!valid) continue;
				HRLSkeletal_ComputeMissingTangentSpace(triVertices[0], triVertices[1], triVertices[2]);
				for (int corner = 0; corner < 3; ++corner)
				{
					const HRL_uint index = (HRL_uint)vertices.size();
					vertices.push_back(triVertices[corner]);
					indices.push_back(index);
				}
			}
		}

		if (vertices.empty())
		{
			ufbx_free_scene(scene);
			SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: skinned mesh contains no convertible triangles");
			return nullptr;
		}

		HRL_SkeletalMeshData* result = new HRL_SkeletalMeshData{};
		// Skeletal vertices are deformed all the way to world space by the skin
		// matrices. Keep the generic geometry transform field neutral so the
		// renderer cannot apply the FBX node transform a second time.
		for (int i = 0; i < 16; ++i)
			result->geometryToWorldMatrix[i] = 0.f;
		result->geometryToWorldMatrix[0] = 1.f;
		result->geometryToWorldMatrix[5] = 1.f;
		result->geometryToWorldMatrix[10] = 1.f;
		result->geometryToWorldMatrix[15] = 1.f;
		result->vertexCount = vertices.size();
		result->indexCount = indices.size();
		result->boneCount = boneNodes.size();
		result->animationCount = scene->anim_stacks.count > 0 ? scene->anim_stacks.count : 1;
		result->vertices = new HRL_SkeletalVertex[result->vertexCount];
		result->indices = new HRL_uint[result->indexCount];
		result->bones = new HRL_SkeletalBone[result->boneCount]{};
		result->animations = new HRL_SkeletalAnimation[result->animationCount]{};
		std::copy(vertices.begin(), vertices.end(), result->vertices);
		std::copy(indices.begin(), indices.end(), result->indices);

		for (size_t i = 0; i < boneNodes.size(); ++i)
		{
			const ufbx_node* node = boneNodes[i];
			HRL_SkeletalBone& bone = result->bones[i];
			bone.name = HRLSkeletal_CopyCString(node->name);
			bone.parentIndex = HRL_INVALID_ID;
			for (ufbx_node* parent = node->parent; parent; parent = parent->parent)
			{
				auto pit = nodeToBone.find(parent);
				if (pit != nodeToBone.end()) { bone.parentIndex = pit->second; break; }
			}
			const auto clusterIt = clusterByNode.find(node);
			const glm::mat4 inverseBind = clusterIt != clusterByNode.end()
				? HRLSkeletal_UFBXMatrixToGLM(clusterIt->second->geometry_to_bone)
				: glm::mat4(1.f);
			const HRL_SkeletalBoneTransform bindTransform = HRLSkeletal_TransformFromUFBX(node->local_transform);
			bone.bindTransform = bindTransform;
			const glm::mat4 bindWorld = HRLSkeletal_UFBXMatrixToGLM(node->node_to_world);
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
				{
					bone.bindWorldMatrix[c * 4 + r] = bindWorld[c][r];
					bone.inverseBindMatrix[c * 4 + r] = inverseBind[c][r];
				}
		}

		for (size_t animationIndex = 0; animationIndex < result->animationCount; ++animationIndex)
		{
			HRL_SkeletalAnimation& animation = result->animations[animationIndex];
			const bool bindPose = scene->anim_stacks.count == 0;
			const ufbx_anim_stack* stack = bindPose ? nullptr : scene->anim_stacks.data[animationIndex];
			if (bindPose)
			{
				const char* name = "BindPose";
				animation.name = new char[std::strlen(name) + 1];
				std::strcpy(const_cast<char*>(animation.name), name);
				animation.duration = 0.f;
				animation.frameRate = 30.f;
				animation.frameCount = 1;
			}
			else
			{
				animation.name = HRLSkeletal_CopyCString(stack->name);
				animation.duration = (float)std::max(0.0, stack->time_end - stack->time_begin);
				animation.frameRate = 30.f;
				const size_t maxFrames = 4096;
				animation.frameCount = animation.duration > 0.f
					? std::min(maxFrames, (size_t)std::ceil(animation.duration * animation.frameRate) + 1u)
					: 1u;
			}
			if (result->boneCount != 0 && animation.frameCount > std::numeric_limits<size_t>::max() / result->boneCount)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: animation frame data is too large");
				return nullptr;
			}
			const size_t frameValueCount = animation.frameCount * result->boneCount;
			if (frameValueCount > std::numeric_limits<size_t>::max() / 16u)
			{
				ufbx_free_scene(scene);
				HRLSkeletal_DestroyMeshData(result);
				SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated pose data is too large");
				return nullptr;
			}
			animation.frames = new HRL_SkeletalBoneTransform[frameValueCount]{};
			animation.poseMatrices = new float[frameValueCount * 16u]{};
			for (size_t frame = 0; frame < animation.frameCount; ++frame)
			{
				const double time = bindPose || !stack
					? 0.0
					: std::min(stack->time_end, stack->time_begin + (double)frame / (double)animation.frameRate);
				ufbx_scene* evaluatedScene = nullptr;
				if (!bindPose && stack && stack->anim)
				{
					ufbx_error evalError{};
					evaluatedScene = ufbx_evaluate_scene(scene, stack->anim, time, nullptr, &evalError);
					if (!evaluatedScene)
					{
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: failed to evaluate animation scene");
						return nullptr;
					}
				}

				// ufbx directly provides the current skinning transform as
				// cluster->geometry_to_world = bone_node->node_to_world * geometry_to_bone.
				// Use that instead of rebuilding the FBX bone hierarchy manually.
				std::unordered_map<uint32_t, const ufbx_skin_cluster*> evaluatedClusterByNode;
				if (evaluatedScene)
				{
					const uint32_t meshTypedId = mesh->typed_id;
					if ((size_t)meshTypedId >= evaluatedScene->meshes.count || !evaluatedScene->meshes.data[meshTypedId])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh lookup failed");
						return nullptr;
					}
					const ufbx_mesh* evaluatedMesh = evaluatedScene->meshes.data[meshTypedId];
					if (!evaluatedMesh || evaluatedMesh->skin_deformers.count == 0 || !evaluatedMesh->skin_deformers.data[0])
					{
						ufbx_free_scene(evaluatedScene);
						ufbx_free_scene(scene);
						HRLSkeletal_DestroyMeshData(result);
						SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated mesh has no skin deformer");
						return nullptr;
					}
					const ufbx_skin_deformer* evaluatedSkin = evaluatedMesh->skin_deformers.data[0];
					for (size_t clusterIndex = 0; clusterIndex < evaluatedSkin->clusters.count; ++clusterIndex)
					{
						const ufbx_skin_cluster* cluster = evaluatedSkin->clusters.data[clusterIndex];
						if (cluster && cluster->bone_node)
							evaluatedClusterByNode[cluster->bone_node->typed_id] = cluster;
					}
				}


				for (size_t boneIndex = 0; boneIndex < boneNodes.size(); ++boneIndex)
				{
					const size_t offset = frame * boneNodes.size() + boneIndex;
					const ufbx_skin_cluster* cluster = nullptr;
					const auto clusterIt = clusterByNode.find(boneNodes[boneIndex]);
					if (clusterIt != clusterByNode.end()) cluster = clusterIt->second;
					const ufbx_skin_cluster* poseCluster = cluster;
					if (evaluatedScene)
					{
						auto evaluatedIt = evaluatedClusterByNode.find(boneNodes[boneIndex]->typed_id);
						if (evaluatedIt != evaluatedClusterByNode.end()) poseCluster = evaluatedIt->second;
					}

					HRL_SkeletalBoneTransform transform = HRLSkeletal_TransformFromUFBX(boneNodes[boneIndex]->local_transform);
					if (evaluatedScene)
					{
						const uint32_t typedId = boneNodes[boneIndex]->typed_id;
						if ((size_t)typedId < evaluatedScene->nodes.count && evaluatedScene->nodes.data[typedId])
							transform = HRLSkeletal_TransformFromUFBX(evaluatedScene->nodes.data[typedId]->local_transform);
						else
						{
							ufbx_free_scene(evaluatedScene);
							ufbx_free_scene(scene);
							HRLSkeletal_DestroyMeshData(result);
							SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: evaluated bone node lookup failed");
							return nullptr;
						}
					}
					animation.frames[offset] = transform;
					// ufbx's cluster->geometry_to_world is already the complete current
					// skin matrix:
					//     bone_node->node_to_world * geometry_to_bone
					// Keep it in world space exactly as provided by ufbx. Do not pre/post-
					// multiply it by the mesh node transform: doing so is the source of
					// the disappearing/partial-mesh regression between the previous fixes.
					const glm::mat4 poseMatrix = poseCluster
						? HRLSkeletal_UFBXMatrixToGLM(poseCluster->geometry_to_world)
						: glm::mat4(1.f);
					for (int col = 0; col < 4; ++col)
						for (int row = 0; row < 4; ++row)
							animation.poseMatrices[offset * 16u + (size_t)col * 4u + (size_t)row] = poseMatrix[col][row];
				}
				if (evaluatedScene)
					ufbx_free_scene(evaluatedScene);
			}
		}

		ufbx_free_scene(scene);
		return result;
	}
	catch (const std::bad_alloc&)
	{
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_GetSkeletalMeshFromFBX: out of memory while converting skeleton");
		return nullptr;
	}
}

void HRL_FreeSkeletalMeshData(HRL_SkeletalMeshData* _data)
{
	HRLSkeletal_DestroyMeshData(_data);
}

// ============================================================================
// FBX resource/material helpers
// ============================================================================

namespace {

static ufbx_load_opts HRLFBX_MakeResourceLoadOptions()
{
	ufbx_load_opts options{};
	options.file_format = UFBX_FILE_FORMAT_FBX;
	options.load_external_files = false;
	options.ignore_missing_external_files = true;
	options.ignore_geometry = true;
	options.ignore_animation = true;
	options.ignore_embedded = false;
	options.evaluate_skinning = false;
	options.evaluate_caches = false;
	options.use_blender_pbr_material = true;
	return options;
}

static bool HRLFBX_HasString(const ufbx_string& value)
{
	return value.data && value.length > 0;
}

struct HRLFBX_TexturePayload
{
	const ufbx_texture* source = nullptr;
	ufbx_blob blob{};
};

static HRLFBX_TexturePayload HRLFBX_FindTexturePayload(
	const HRL_FBXResources* resources,
	const ufbx_texture* texture,
	int depth = 0)
{
	HRLFBX_TexturePayload result{};
	if (!resources || !resources->scene || !texture || depth > 16)
		return result;

	if (texture->content.data && texture->content.size > 0)
	{
		result.source = texture;
		result.blob = texture->content;
		return result;
	}

	if (texture->has_file && texture->file_index < resources->scene->texture_files.count)
	{
		const ufbx_texture_file& file = resources->scene->texture_files.data[texture->file_index];
		if (file.content.data && file.content.size > 0)
		{
			result.source = texture;
			result.blob = file.content;
			return result;
		}
	}

	for (size_t i = 0; i < texture->file_textures.count; ++i)
	{
		HRLFBX_TexturePayload nested = HRLFBX_FindTexturePayload(
			resources, texture->file_textures.data[i], depth + 1);
		if (nested.blob.data && nested.blob.size > 0)
			return nested;
	}

	return result;
}

static const char* HRLFBX_TextureFilename(const ufbx_texture* texture)
{
	if (!texture)
		return "";
	if (HRLFBX_HasString(texture->filename))
		return texture->filename.data;
	if (HRLFBX_HasString(texture->relative_filename))
		return texture->relative_filename.data;
	return "";
}

static const ufbx_material_map* HRLFBX_SelectMap(
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	if (primary.texture && primary.texture_enabled)
		return &primary;
	if (primary.has_value)
		return &primary;
	if (fallback.texture && fallback.texture_enabled)
		return &fallback;
	return &fallback;
}

static const ufbx_material_map* HRLFBX_SelectTextureMap(
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	if (primary.texture && primary.texture_enabled)
		return &primary;
	if (fallback.texture && fallback.texture_enabled)
		return &fallback;
	return nullptr;
}

static glm::vec4 HRLFBX_ReadColor(const ufbx_material_map& map, const glm::vec4& fallback)
{
	if (!map.has_value && map.value_components == 0)
		return fallback;

	switch (map.value_components)
	{
	case 1:
		return glm::vec4((float)map.value_real, (float)map.value_real, (float)map.value_real, 1.f);
	case 2:
		return glm::vec4((float)map.value_vec2.x, (float)map.value_vec2.y, 0.f, 1.f);
	case 3:
		return glm::vec4(
			(float)map.value_vec3.x,
			(float)map.value_vec3.y,
			(float)map.value_vec3.z,
			1.f);
	case 4:
		return glm::vec4(
			(float)map.value_vec4.x,
			(float)map.value_vec4.y,
			(float)map.value_vec4.z,
			(float)map.value_vec4.w);
	default:
		return fallback;
	}
}

static float HRLFBX_ReadScalar(const ufbx_material_map& map, float fallback)
{
	if (!map.has_value && map.value_components == 0)
		return fallback;
	if (map.value_components == 1)
		return (float)map.value_real;
	if (map.value_components == 2)
		return (float)map.value_vec2.x;
	if (map.value_components == 3)
		return (float)map.value_vec3.x;
	if (map.value_components == 4)
		return (float)map.value_vec4.x;
	return fallback;
}

static HRL_uint HRLFBX_TextureIndex(HRL_FBXResources* resources, const ufbx_texture* texture)
{
	if (!resources || !texture)
		return (HRL_uint)HRL_INVALID_ID;

	auto found = resources->texture_indices.find(texture);
	if (found != resources->texture_indices.end())
		return found->second;

	HRL_FBXTextureInfo info{};
	info.name = HRLFBX_HasString(texture->name) ? texture->name.data : "";
	info.filename = HRLFBX_TextureFilename(texture);
	switch (texture->type)
	{
	case UFBX_TEXTURE_FILE:       info.type = HRL_FBX_TEXTURE_FILE; break;
	case UFBX_TEXTURE_LAYERED:    info.type = HRL_FBX_TEXTURE_LAYERED; break;
	case UFBX_TEXTURE_PROCEDURAL: info.type = HRL_FBX_TEXTURE_PROCEDURAL; break;
	case UFBX_TEXTURE_SHADER:     info.type = HRL_FBX_TEXTURE_SHADER; break;
	default:                      info.type = HRL_FBX_TEXTURE_FILE; break;
	}

	const HRLFBX_TexturePayload payload = HRLFBX_FindTexturePayload(resources, texture);
	if (payload.blob.data && payload.blob.size > 0)
	{
		info.data = (const unsigned char*)payload.blob.data;
		info.size = payload.blob.size;
		info.embedded = 1;
	}

	if (resources->textures.size() >= (size_t)HRL_INVALID_ID)
		return (HRL_uint)HRL_INVALID_ID;

	const HRL_uint index = (HRL_uint)resources->textures.size();
	resources->textures.push_back(info);
	resources->texture_indices.emplace(texture, index);
	return index;
}

static void HRLFBX_AssignTextureIndex(
	HRL_FBXResources* resources,
	HRL_uint& destination,
	const ufbx_material_map& primary,
	const ufbx_material_map& fallback)
{
	const ufbx_material_map* map = HRLFBX_SelectTextureMap(primary, fallback);
	if (!map || !map->texture)
		return;
	destination = HRLFBX_TextureIndex(resources, map->texture);
}

static void HRLFBX_BuildTextureInfo(
	HRL_FBXResources* resources,
	const ufbx_texture* texture,
	HRL_FBXTextureInfo& info)
{
	info = {};
	info.name = texture && HRLFBX_HasString(texture->name) ? texture->name.data : "";
	info.filename = HRLFBX_TextureFilename(texture);
	info.type = HRL_FBX_TEXTURE_FILE;
	if (texture)
	{
		switch (texture->type)
		{
		case UFBX_TEXTURE_FILE:       info.type = HRL_FBX_TEXTURE_FILE; break;
		case UFBX_TEXTURE_LAYERED:    info.type = HRL_FBX_TEXTURE_LAYERED; break;
		case UFBX_TEXTURE_PROCEDURAL: info.type = HRL_FBX_TEXTURE_PROCEDURAL; break;
		case UFBX_TEXTURE_SHADER:     info.type = HRL_FBX_TEXTURE_SHADER; break;
		default:                      info.type = HRL_FBX_TEXTURE_FILE; break;
		}
	}
	const HRLFBX_TexturePayload payload = HRLFBX_FindTexturePayload(resources, texture);
	if (payload.blob.data && payload.blob.size > 0)
	{
		info.data = (const unsigned char*)payload.blob.data;
		info.size = payload.blob.size;
		info.embedded = 1;
	}
}

static void HRLFBX_BuildMaterialInfo(
	HRL_FBXResources* resources,
	const ufbx_material* material,
	HRL_FBXMaterialInfo& info)
{
	info = {};
	for (HRL_uint& index : info.textureIndices)
		index = (HRL_uint)HRL_INVALID_ID;

	info.name = material && HRLFBX_HasString(material->name) ? material->name.data : "";
	info.baseColor[0] = 1.f;
	info.baseColor[1] = 1.f;
	info.baseColor[2] = 1.f;
	info.baseColor[3] = 1.f;
	info.roughness = 0.5f;
	info.metallic = 0.f;
	info.specular = 0.5f;
	info.opacity = 1.f;

	if (!material)
		return;

	const bool hasPbrBase =
		(material->pbr.base_color.texture && material->pbr.base_color.texture_enabled) ||
		material->pbr.base_color.has_value;
	const ufbx_material_map* baseColor = hasPbrBase
		? &material->pbr.base_color
		: &material->fbx.diffuse_color;
	glm::vec4 color = HRLFBX_ReadColor(*baseColor, glm::vec4(1.f));
	if (hasPbrBase)
	{
		if (material->pbr.base_factor.has_value)
			color *= HRLFBX_ReadColor(material->pbr.base_factor, glm::vec4(1.f));
	}
	else if (material->fbx.diffuse_factor.has_value)
	{
		color *= HRLFBX_ReadColor(material->fbx.diffuse_factor, glm::vec4(1.f));
	}

	info.baseColor[0] = std::clamp(color.r, 0.f, 1.f);
	info.baseColor[1] = std::clamp(color.g, 0.f, 1.f);
	info.baseColor[2] = std::clamp(color.b, 0.f, 1.f);
	info.baseColor[3] = std::clamp(color.a, 0.f, 1.f);

	if (material->pbr.roughness.has_value || material->pbr.roughness.texture)
		info.roughness = std::clamp(HRLFBX_ReadScalar(material->pbr.roughness, 0.5f), 0.f, 1.f);
	else if (material->pbr.glossiness.has_value || material->pbr.glossiness.texture)
		info.roughness = 1.f - std::clamp(HRLFBX_ReadScalar(material->pbr.glossiness, 0.5f), 0.f, 1.f);

	if (material->pbr.metalness.has_value || material->pbr.metalness.texture)
		info.metallic = std::clamp(HRLFBX_ReadScalar(material->pbr.metalness, 0.f), 0.f, 1.f);

	if (material->pbr.specular_factor.has_value || material->pbr.specular_factor.texture)
		info.specular = std::clamp(HRLFBX_ReadScalar(material->pbr.specular_factor, 0.5f), 0.f, 1.f);
	else if (material->fbx.specular_factor.has_value || material->fbx.specular_factor.texture)
		info.specular = std::clamp(HRLFBX_ReadScalar(material->fbx.specular_factor, 0.5f), 0.f, 1.f);

	// ufbx exposes PBR maps for all shading models, and for legacy Phong/
	// Lambert materials some of those maps can contain synthesized default
	// values even when the material does not actually use opacity. Do not treat
	// such a scalar as authoritative: a synthesized opacity of 0 would make the
	// entire material disappear in the fragment shader (which discards alpha).
	const bool pbrOpacityTexture =
		material->pbr.opacity.texture && material->pbr.opacity.texture_enabled;
	const bool fbxTransparencyTexture =
		material->fbx.transparency_factor.texture && material->fbx.transparency_factor.texture_enabled;
	const bool opacityFeatureEnabled = material->features.opacity.enabled;
	if (pbrOpacityTexture)
		info.opacity = std::clamp(HRLFBX_ReadScalar(material->pbr.opacity, 1.f), 0.f, 1.f);
	else if (fbxTransparencyTexture)
		info.opacity = 1.f - std::clamp(HRLFBX_ReadScalar(material->fbx.transparency_factor, 0.f), 0.f, 1.f);
	else if (opacityFeatureEnabled && material->pbr.opacity.has_value)
		info.opacity = std::clamp(HRLFBX_ReadScalar(material->pbr.opacity, 1.f), 0.f, 1.f);
	else if (opacityFeatureEnabled && material->fbx.transparency_factor.has_value)
		info.opacity = 1.f - std::clamp(HRLFBX_ReadScalar(material->fbx.transparency_factor, 0.f), 0.f, 1.f);
	else
		info.opacity = 1.f;

	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALBEDO],
		material->pbr.base_color, material->fbx.diffuse_color);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_NORMAL],
		material->pbr.normal_map, material->fbx.normal_map);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_SPECULAR],
		material->pbr.specular_color, material->fbx.specular_color);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS],
		material->pbr.roughness, material->pbr.roughness);
	if (info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS] == (HRL_uint)HRL_INVALID_ID)
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS],
			material->pbr.glossiness, material->pbr.glossiness);
	HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_METALLIC],
		material->pbr.metalness, material->pbr.metalness);
	if (material->pbr.opacity.texture && material->pbr.opacity.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->pbr.opacity, material->pbr.opacity);
	}
	if (info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID &&
		material->fbx.transparency_factor.texture && material->fbx.transparency_factor.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->fbx.transparency_factor, material->fbx.transparency_factor);
	}
	if (info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID &&
		material->fbx.transparency_color.texture && material->fbx.transparency_color.texture_enabled)
	{
		HRLFBX_AssignTextureIndex(resources, info.textureIndices[HRL_FBX_MATERIAL_ALPHA],
			material->fbx.transparency_color, material->fbx.transparency_color);
	}
}

static HRL_id HRLFBX_CreateMaterialFromResource(
	HRL_FBXResources* resources,
	size_t materialIndex,
	HRL_id shaderid)
{
	if (!resources || materialIndex >= resources->materials.size())
		return HRL_INVALID_ID;
	if (!HRL_IsValidShader(shaderid))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: invalid shader ID");
		return HRL_INVALID_ID;
	}

	HRL_id materialId = HRL_INVALID_ID;
	try
	{
		const HRL_FBXMaterialInfo& info = resources->materials[materialIndex];
		materialId = HRL_CreateMaterial(shaderid);
		if (materialId == HRL_INVALID_ID)
			return HRL_INVALID_ID;

		HRL_MaterialSetVec3(materialId, "TintColor", info.baseColor[0], info.baseColor[1], info.baseColor[2]);
		HRL_MaterialSetFloat(materialId, "BaseColorAlpha", info.baseColor[3]);
		HRL_MaterialSetFloat(materialId, "RoughnessValue", info.roughness);
		HRL_MaterialSetFloat(materialId, "MetallicValue", info.metallic);
		HRL_MaterialSetFloat(materialId, "SpecularValue", info.specular);
		HRL_MaterialSetFloat(materialId, "OpacityValue", info.opacity);
		HRL_MaterialSetInt(materialId, "RoughnessUseValue", info.textureIndices[HRL_FBX_MATERIAL_ROUGHNESS] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "MetallicUseValue", info.textureIndices[HRL_FBX_MATERIAL_METALLIC] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "SpecularUseValue", info.textureIndices[HRL_FBX_MATERIAL_SPECULAR] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);
		HRL_MaterialSetInt(materialId, "OpacityUseValue", info.textureIndices[HRL_FBX_MATERIAL_ALPHA] == (HRL_uint)HRL_INVALID_ID ? 1 : 0);

		if (materialIndex < resources->material_sources.size())
		{
			const ufbx_material* fbxMaterial = resources->material_sources[materialIndex];
			if (fbxMaterial)
			{
				if (!fbxMaterial->pbr.roughness.texture && fbxMaterial->pbr.glossiness.texture)
					HRL_MaterialSetInt(materialId, "RoughnessInvert", 1);
				if (!fbxMaterial->pbr.opacity.texture &&
					((fbxMaterial->fbx.transparency_factor.texture && fbxMaterial->fbx.transparency_factor.texture_enabled) ||
					 (fbxMaterial->fbx.transparency_color.texture && fbxMaterial->fbx.transparency_color.texture_enabled)))
					HRL_MaterialSetInt(materialId, "AlphaInvert", 1);
			}
		}

		HRL_Material* materialObject = nullptr;
		auto materialIt = ctx_.materials.find(materialId);
		if (materialIt != ctx_.materials.end())
			materialObject = materialIt->second;
		if (!materialObject)
		{
			SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
				"HRL_CreateMaterialFromFBX: material was not registered");
			return HRL_INVALID_ID;
		}

		materialObject->owned_textures_.reserve(
			materialObject->owned_textures_.size() + HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT);

		HRL_uint createdTextureIndices[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT]{};
		HRL_id createdTextureIds[HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT]{};
		size_t createdTextureCount = 0;
		static const char* const slots[] = {
			HRL_T_ALBEDO,
			HRL_T_NORMAL,
			HRL_T_SPECULAR,
			HRL_T_ROUGHNESS,
			HRL_T_METALLIC,
			HRL_T_ALPHA
		};

		for (int slot = 0; slot < (int)HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT; ++slot)
		{
			const HRL_uint textureIndex = info.textureIndices[slot];
			if (textureIndex == (HRL_uint)HRL_INVALID_ID)
				continue;

			const HRL_FBXTextureInfo* textureInfo = HRL_GetFBXTexture(resources, textureIndex);
			if (!textureInfo || !textureInfo->embedded || !textureInfo->data || textureInfo->size == 0)
				continue;

			HRL_id textureId = HRL_INVALID_ID;
			for (size_t i = 0; i < createdTextureCount; ++i)
			{
				if (createdTextureIndices[i] == textureIndex)
				{
					textureId = createdTextureIds[i];
					break;
				}
			}

			if (textureId == HRL_INVALID_ID)
			{
				textureId = HRL_CreateTextureFromFBX(resources, textureIndex);
				if (textureId == HRL_INVALID_ID)
					continue;
				createdTextureIndices[createdTextureCount] = textureIndex;
				createdTextureIds[createdTextureCount] = textureId;
				++createdTextureCount;
				materialObject->owned_textures_.push_back(textureId);
			}

			HRL_MaterialSetTexture(materialId, slots[slot], textureId);
		}

		return materialId;
	}
	catch (const std::bad_alloc&)
	{
		if (materialId != HRL_INVALID_ID)
			HRL_DeleteMaterial(materialId);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: out of memory while creating material resources");
		return HRL_INVALID_ID;
	}
}
} // anonymous namespace


HRL_FBXResources* HRL_LoadFBXResources(const char* _data, size_t _bufferSize)
{
	if (!_data || _bufferSize == 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_LoadFBXResources: data and buffer size must be valid");
		return nullptr;
	}

	ufbx_load_opts options = HRLFBX_MakeResourceLoadOptions();
	ufbx_error error{};
	ufbx_scene* scene = ufbx_load_memory(_data, _bufferSize, &options, &error);
	if (!scene)
	{
		std::string detail = "HRL_LoadFBXResources: failed to decode FBX file";
		if (error.description.data && error.description.data[0] != '\0')
		{
			detail += " (";
			detail += error.description.data;
			detail += ")";
		}
		SetErrorCode(HRL_INVALID_FILE_FORMAT, HRL_SEVERITY_ERROR, detail.c_str());
		return nullptr;
	}

	HRL_FBXResources* resources = nullptr;
	try
	{
		resources = new HRL_FBXResources();
		resources->scene = scene;
		resources->textures.reserve(scene->textures.count);
		resources->materials.reserve(scene->materials.count);
		resources->material_sources.reserve(scene->materials.count);

		for (size_t i = 0; i < scene->textures.count; ++i)
		{
			const ufbx_texture* texture = scene->textures.data[i];
			if (!texture)
				continue;
			HRL_FBXTextureInfo info{};
			HRLFBX_BuildTextureInfo(resources, texture, info);
			const HRL_uint index = (HRL_uint)resources->textures.size();
			resources->textures.push_back(info);
			resources->texture_indices.emplace(texture, index);
		}

		for (size_t i = 0; i < scene->materials.count; ++i)
		{
			const ufbx_material* material = scene->materials.data[i];
			if (!material)
				continue;
			HRL_FBXMaterialInfo info{};
			HRLFBX_BuildMaterialInfo(resources, material, info);
			resources->materials.push_back(info);
			resources->material_sources.push_back(material);
		}
	}
	catch (const std::bad_alloc&)
	{
		delete resources;
		ufbx_free_scene(scene);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_LoadFBXResources: out of memory while building resource tables");
		return nullptr;
	}

	return resources;
}

void HRL_FreeFBXResources(HRL_FBXResources* _resources)
{
	if (!_resources)
		return;
	if (_resources->scene)
		ufbx_free_scene(_resources->scene);
	delete _resources;
}

size_t HRL_GetFBXMaterialCount(const HRL_FBXResources* _resources)
{
	return _resources ? _resources->materials.size() : 0;
}

const HRL_FBXMaterialInfo* HRL_GetFBXMaterial(const HRL_FBXResources* _resources, size_t _index)
{
	if (!_resources || _index >= _resources->materials.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXMaterial: invalid material index");
		return nullptr;
	}
	return &_resources->materials[_index];
}

HRL_id HRL_FindFBXMaterial(const HRL_FBXResources* _resources, const char* _name)
{
	if (!_resources || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindFBXMaterial: invalid resources or name");
		return HRL_INVALID_ID;
	}
	for (size_t i = 0; i < _resources->materials.size(); ++i)
	{
		const char* name = _resources->materials[i].name ? _resources->materials[i].name : "";
		if (std::strcmp(name, _name) == 0)
			return (HRL_id)i;
	}
	return HRL_INVALID_ID;
}

size_t HRL_GetFBXTextureCount(const HRL_FBXResources* _resources)
{
	return _resources ? _resources->textures.size() : 0;
}

const HRL_FBXTextureInfo* HRL_GetFBXTexture(const HRL_FBXResources* _resources, size_t _index)
{
	if (!_resources || _index >= _resources->textures.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXTexture: invalid texture index");
		return nullptr;
	}
	return &_resources->textures[_index];
}

HRL_id HRL_FindFBXTexture(const HRL_FBXResources* _resources, const char* _name)
{
	if (!_resources || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindFBXTexture: invalid resources or name");
		return HRL_INVALID_ID;
	}
	for (size_t i = 0; i < _resources->textures.size(); ++i)
	{
		const char* name = _resources->textures[i].name ? _resources->textures[i].name : "";
		const char* filename = _resources->textures[i].filename ? _resources->textures[i].filename : "";
		if (std::strcmp(name, _name) == 0 || std::strcmp(filename, _name) == 0)
			return (HRL_id)i;
	}
	return HRL_INVALID_ID;
}

const HRL_FBXTextureInfo* HRL_GetFBXMaterialTexture(
	const HRL_FBXResources* _resources,
	size_t _materialIndex,
	HRL_EFBXMaterialTextureSlot _slot)
{
	if (!_resources || _materialIndex >= _resources->materials.size() ||
		_slot < HRL_FBX_MATERIAL_ALBEDO || _slot >= HRL_FBX_MATERIAL_TEXTURE_SLOT_COUNT)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetFBXMaterialTexture: invalid material or texture slot");
		return nullptr;
	}
	const HRL_uint index = _resources->materials[_materialIndex].textureIndices[(size_t)_slot];
	if (index == (HRL_uint)HRL_INVALID_ID)
		return nullptr;
	return HRL_GetFBXTexture(_resources, index);
}

HRL_id HRL_CreateTextureFromFBX(const HRL_FBXResources* _resources, size_t _textureIndex)
{
	const HRL_FBXTextureInfo* texture = HRL_GetFBXTexture(_resources, _textureIndex);
	if (!texture)
		return HRL_INVALID_ID;
	if (!texture->embedded || !texture->data || texture->size == 0)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_CreateTextureFromFBX: texture has no embedded image data");
		return HRL_INVALID_ID;
	}
	return HRL_CreateTexture(reinterpret_cast<const char*>(texture->data), texture->size);
}

static HRL_id HRLFBX_SelectAutomaticMaterialShader(const HRL_FBXResources* resources)
{
	if (resources && resources->scene)
	{
		for (size_t i = 0; i < resources->scene->meshes.count; ++i)
		{
			const ufbx_mesh* mesh = resources->scene->meshes.data[i];
			if (mesh && mesh->skin_deformers.count > 0)
				return HRL_SKINNED_3D_MESH_SHADER;
		}
	}
	return HRL_MESH_3D_SHADER;
}

static HRL_id HRLFBX_CreateAutomaticMaterial(
	const char* _data, size_t _bufferSize, size_t _materialIndex)
{
	HRL_FBXResources* resources = HRL_LoadFBXResources(_data, _bufferSize);
	if (!resources)
		return HRL_INVALID_ID;

	if (_materialIndex >= HRL_GetFBXMaterialCount(resources))
	{
		HRL_FreeFBXResources(resources);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: FBX contains no material at the requested index");
		return HRL_INVALID_ID;
	}

	const HRL_id shader = HRLFBX_SelectAutomaticMaterialShader(resources);
	const HRL_id material = HRLFBX_CreateMaterialFromResource(resources, _materialIndex, shader);
	HRL_FreeFBXResources(resources);
	return material;
}

HRL_id HRL_CreateMaterialFromFBX(const char* _data, size_t _bufferSize)
{
	return HRLFBX_CreateAutomaticMaterial(_data, _bufferSize, 0);
}

HRL_id HRL_CreateMaterialFromFBXWithShader(const char* _data, size_t _bufferSize, HRL_id _shaderid)
{
	return HRL_CreateMaterialFromFBXIndexedWithShader(
		_data, _bufferSize, 0, _shaderid);
}

HRL_id HRL_CreateMaterialFromFBXIndexed(
	const char* _data, size_t _bufferSize, size_t _materialIndex)
{
	return HRLFBX_CreateAutomaticMaterial(_data, _bufferSize, _materialIndex);
}

HRL_id HRL_CreateMaterialFromFBXIndexedWithShader(
	const char* _data, size_t _bufferSize, size_t _materialIndex, HRL_id _shaderid)
{
	HRL_FBXResources* resources = HRL_LoadFBXResources(_data, _bufferSize);
	if (!resources)
		return HRL_INVALID_ID;

	if (_materialIndex >= HRL_GetFBXMaterialCount(resources))
	{
		HRL_FreeFBXResources(resources);
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_CreateMaterialFromFBX: FBX contains no material at the requested index");
		return HRL_INVALID_ID;
	}

	const HRL_id material = HRLFBX_CreateMaterialFromResource(resources, _materialIndex, _shaderid);
	HRL_FreeFBXResources(resources);
	return material;
}

HRL_id HRL_CreateSkeletalMesh(HRL_id _sceneid, const HRL_SkeletalMeshData* _data)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!_data || !_data->vertices || _data->vertexCount < 3 || !_data->bones || _data->boneCount == 0 || _data->boneCount > HRL_MAX_SKELETAL_BONES)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: invalid skeletal mesh data");
		return HRL_INVALID_ID;
	}
	if (_data->indexCount > 0 && !_data->indices)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count is non-zero but indices are null");
		return HRL_INVALID_ID;
	}
	if (_data->vertexCount > (size_t)std::numeric_limits<HRL_uint>::max())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex count exceeds HRL_uint index range");
		return HRL_INVALID_ID;
	}
	for (size_t vertexIndex = 0; vertexIndex < _data->vertexCount; ++vertexIndex)
	{
		const HRL_Vertex3D& vertex = _data->vertices[vertexIndex].vertex;
		for (float value : vertex.position)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex position contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.normal)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex normal contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.uv)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex UV contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.tangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex tangent contains NaN or infinity"); return HRL_INVALID_ID; }
		for (float value : vertex.bitangent)
			if (!std::isfinite(value)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: vertex bitangent contains NaN or infinity"); return HRL_INVALID_ID; }
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = _data->vertices[vertexIndex].boneWeights[influence];
			if (!std::isfinite(weight) || weight < 0.f)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone weights must be finite and non-negative");
				return HRL_INVALID_ID;
			}
			if (weight > 0.f && _data->vertices[vertexIndex].boneIndices[influence] >= _data->boneCount)
			{
				SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: bone index is outside the skeleton");
				return HRL_INVALID_ID;
			}
			weightSum += weight;
		}
		if (!(weightSum > 0.f) || !std::isfinite(weightSum))
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: every vertex must have at least one positive bone weight");
			return HRL_INVALID_ID;
		}
	}
	for (size_t i = 0; i < _data->indexCount; ++i)
	{
		if ((size_t)_data->indices[i] >= _data->vertexCount)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index references a vertex outside the vertex array");
			return HRL_INVALID_ID;
		}
	}
	if (_data->indexCount > 0 && (_data->indexCount % 3u) != 0)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: index count must be a multiple of 3");
		return HRL_INVALID_ID;
	}

	std::vector<HRL_uint> generatedIndices;
	const HRL_uint* indices = _data->indices;
	if (_data->indexCount == 0)
	{
		if ((_data->vertexCount % 3u) != 0)
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: non-indexed vertex count must be a multiple of 3");
			return HRL_INVALID_ID;
		}
		generatedIndices.resize(_data->vertexCount);
		for (size_t i = 0; i < generatedIndices.size(); ++i)
			generatedIndices[i] = (HRL_uint)i;
		indices = generatedIndices.data();
	}

	HRL_id newId = GenerateHRL_ID();
	if (!g_Backend.RHI_CreateSkeletalMesh ||
		g_Backend.RHI_CreateSkeletalMesh(newId, _data->vertices, _data->vertexCount, indices,
			_data->indexCount > 0 ? _data->indexCount : generatedIndices.size(), (HRL_uint)_data->boneCount) != HRL_TRUE)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: backend failed to create skeletal mesh");
		return HRL_INVALID_ID;
	}

	std::unique_ptr<HRL_SkeletalMesh> mesh(new (std::nothrow) HRL_SkeletalMesh());
	if (!mesh)
	{
		g_Backend.RHI_DeleteMesh(newId);
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: out of memory");
		return HRL_INVALID_ID;
	}
	mesh->scene_ = _sceneid;
	mesh->type_ = HRL_3D_SKELETAL_MESH;
	bool hasGeometryToWorld = false;
	for (int i = 0; i < 16; ++i)
		if (std::isfinite(_data->geometryToWorldMatrix[i]) && std::fabs(_data->geometryToWorldMatrix[i]) > 0.f) { hasGeometryToWorld = true; break; }
	mesh->fbx_geometry_to_world_ = hasGeometryToWorld
		? HRLSkeletal_ArrayToGLM(_data->geometryToWorldMatrix)
		: glm::mat4(1.f);
	mesh->triangle_count_ = (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()) / 3u;
	mesh->vertices_.assign(_data->vertices, _data->vertices + _data->vertexCount);
	mesh->indices_.assign(indices, indices + (_data->indexCount > 0 ? _data->indexCount : generatedIndices.size()));
	mesh->bones_.resize(_data->boneCount);
	for (size_t i = 0; i < _data->boneCount; ++i)
	{
		mesh->bones_[i].public_ = _data->bones[i];
		mesh->bones_[i].name_storage_ = _data->bones[i].name ? _data->bones[i].name : "Bone";
		mesh->bones_[i].public_.parentIndex = _data->bones[i].parentIndex < _data->boneCount ? _data->bones[i].parentIndex : HRL_INVALID_ID;
		for (int j = 0; j < 16; ++j)
			mesh->bones_[i].public_.inverseBindMatrix[j] = _data->bones[i].inverseBindMatrix[j];
		mesh->bones_[i].bind_local_ = _data->bones[i].bindTransform;
		glm::mat4 inverseBind(1.0f);
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				inverseBind[col][row] = mesh->bones_[i].public_.inverseBindMatrix[col * 4 + row];
		mesh->bones_[i].inverse_bind_ = inverseBind;
		mesh->bones_[i].bind_world_ = HRLSkeletal_ArrayToGLM(_data->bones[i].bindWorldMatrix);
	}
	// Bind world matrices from the importer are exact; reconstruct only for legacy manually-created data.
	{
		const size_t boneCount = mesh->bones_.size();
		std::vector<unsigned char> built(boneCount, 0);
		std::function<glm::mat4(size_t)> buildBone = [&](size_t boneIndex) -> glm::mat4
		{
			if (built[boneIndex])
				return mesh->bones_[boneIndex].bind_world_;
			glm::mat4 parentWorld(1.f);
			const size_t parent = mesh->bones_[boneIndex].public_.parentIndex;
			if (parent < boneCount && parent != boneIndex)
				parentWorld = buildBone(parent);
			mesh->bones_[boneIndex].bind_world_ = parentWorld * HRLSkeletal_TransformToMatrix(mesh->bones_[boneIndex].bind_local_);
			built[boneIndex] = 1;
			return mesh->bones_[boneIndex].bind_world_;
		};
		for (size_t i = 0; i < boneCount; ++i)
		{
			bool hasBindWorld = false;
			for (int e = 0; e < 16; ++e)
				if (mesh->bones_[i].public_.bindWorldMatrix[e] != 0.f) { hasBindWorld = true; break; }
			if (!hasBindWorld)
				buildBone(i);
		}
	}

	// The baked animations are copied before public pointers are refreshed.
	mesh->animations_.resize(_data->animationCount);
	for (size_t i = 0; i < _data->animationCount; ++i)
	{
		mesh->animations_[i].public_ = _data->animations[i];
		mesh->animations_[i].name_storage_ = _data->animations[i].name ? _data->animations[i].name : "Animation";
		if (_data->boneCount != 0 && _data->animations[i].frameCount > std::numeric_limits<size_t>::max() / _data->boneCount)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation frame data is too large");
			return HRL_INVALID_ID;
		}
		const size_t valueCount = _data->animations[i].frameCount * _data->boneCount;
		if (valueCount > 0 && !_data->animations[i].frames)
		{
			g_Backend.RHI_DeleteMesh(newId);
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateSkeletalMesh: animation has no frame data");
			return HRL_INVALID_ID;
		}
		if (valueCount > 0)
			mesh->animations_[i].frames_.assign(_data->animations[i].frames, _data->animations[i].frames + valueCount);
		else
			mesh->animations_[i].frames_.clear();
		if (valueCount > 0 && _data->animations[i].poseMatrices)
			mesh->animations_[i].pose_matrices_storage_.assign(_data->animations[i].poseMatrices, _data->animations[i].poseMatrices + valueCount * 16u);
		else
			mesh->animations_[i].pose_matrices_storage_.clear();
	}

	for (auto& animation : mesh->animations_)
		HRLSkeletal_BuildPoseFramesForAnimation(mesh.get(), animation);

	mesh->RefreshPublicPointers();
	mesh->bone_matrices_.resize(mesh->bones_.size(), glm::mat4(1.f));
	HRLSkeletal_UpdatePose(mesh.get());

	glm::vec3 minPoint(std::numeric_limits<float>::max());
	glm::vec3 maxPoint(std::numeric_limits<float>::lowest());
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f))
			skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f)
			skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
		{
			minPoint = glm::min(minPoint, p);
			maxPoint = glm::max(maxPoint, p);
		}
	}
	if (!std::isfinite(minPoint.x) || !std::isfinite(maxPoint.x))
	{
		minPoint = glm::vec3(-1.f);
		maxPoint = glm::vec3(1.f);
	}
	mesh->bounds_center_ = (minPoint + maxPoint) * 0.5f;
	float radius2 = 0.f;
	for (const auto& vertex : mesh->vertices_)
	{
		const glm::vec4 localPosition(vertex.vertex.position[0], vertex.vertex.position[1], vertex.vertex.position[2], 1.f);
		glm::vec4 skinnedPosition(0.f);
		float weightSum = 0.f;
		for (size_t influence = 0; influence < HRL_SKELETAL_MAX_INFLUENCES; ++influence)
		{
			const float weight = vertex.boneWeights[influence];
			if (!(weight > 0.f)) continue;
			const size_t boneIndex = (size_t)vertex.boneIndices[influence];
			if (boneIndex >= mesh->bone_matrices_.size()) continue;
			skinnedPosition += mesh->bone_matrices_[boneIndex] * localPosition * weight;
			weightSum += weight;
		}
		if (!(weightSum > 0.f)) skinnedPosition = localPosition;
		else if (std::abs(weightSum - 1.f) > 1e-5f) skinnedPosition /= weightSum;
		const glm::vec3 p = glm::vec3(skinnedPosition);
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
			radius2 = std::max(radius2, glm::dot(p - mesh->bounds_center_, p - mesh->bounds_center_));
	}
	mesh->bounds_radius_ = std::sqrt(radius2);

	HRL_SkeletalMesh* rawMesh = mesh.release();
	sceneIt->second->meshes.emplace(newId, rawMesh);
	sceneIt->second->shadows_dirty = true;
	ctx_.meshes.emplace(newId, rawMesh);
	return newId;
}

HRL_uint HRL_GetSkeletalBoneCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBoneCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->bones_.size();
}

const HRL_SkeletalBone* HRL_GetSkeletalBone(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->bones_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalBone: invalid bone index");
		return nullptr;
	}
	return &mesh->bones_[_index].public_;
}

HRL_uint HRL_FindSkeletalBone(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalBone: invalid mesh or bone name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->bones_.size(); ++i)
		if (mesh->bones_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

HRL_uint HRL_GetSkeletalAnimationCount(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationCount: invalid skeletal mesh ID");
		return 0;
	}
	return (HRL_uint)static_cast<HRL_SkeletalMesh*>(it->second)->animations_.size();
}

const HRL_SkeletalAnimation* HRL_GetSkeletalAnimation(HRL_id _meshid, HRL_uint _index)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid skeletal mesh ID");
		return nullptr;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_index >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimation: invalid animation index");
		return nullptr;
	}
	return &mesh->animations_[_index].public_;
}

HRL_uint HRL_FindSkeletalAnimation(HRL_id _meshid, const char* _name)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH || !_name)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_FindSkeletalAnimation: invalid mesh or animation name");
		return HRL_INVALID_ID;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	for (size_t i = 0; i < mesh->animations_.size(); ++i)
		if (mesh->animations_[i].name_storage_ == _name)
			return (HRL_uint)i;
	return HRL_INVALID_ID;
}

void HRL_PlaySkeletalAnimation(HRL_id _meshid, HRL_uint _animation)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (_animation >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_PlaySkeletalAnimation: invalid animation index");
		return;
	}
	mesh->current_animation_ = (int)_animation;
	mesh->animation_time_ = 0.f;
	mesh->animation_playing_ = true;
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_StopSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_StopSkeletalAnimation: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ = false;
}

void HRL_SetSkeletalAnimationTime(HRL_id _meshid, float _time)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_time))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: time must be finite");
		return;
	}
	auto* mesh = static_cast<HRL_SkeletalMesh*>(it->second);
	if (mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationTime: no animation is selected");
		return;
	}
	const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
	if (mesh->animation_loop_ && duration > 0.f)
	{
		float wrapped = std::fmod(_time, duration);
		if (wrapped < 0.f) wrapped += duration;
		mesh->animation_time_ = wrapped;
	}
	else
		mesh->animation_time_ = std::clamp(_time, 0.f, duration);
	HRLSkeletal_UpdatePose(mesh);
}

void HRL_SetSkeletalAnimationSpeed(HRL_id _meshid, float _speed)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: invalid skeletal mesh ID");
		return;
	}
	if (!std::isfinite(_speed))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationSpeed: speed must be finite");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_speed_ = _speed;
}

void HRL_SetSkeletalAnimationLoop(HRL_id _meshid, int _loop)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSkeletalAnimationLoop: invalid skeletal mesh ID");
		return;
	}
	static_cast<HRL_SkeletalMesh*>(it->second)->animation_loop_ = _loop != HRL_FALSE;
}

int HRL_GetCurrentSkeletalAnimation(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetCurrentSkeletalAnimation: invalid skeletal mesh ID");
		return -1;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->current_animation_;
}

float HRL_GetSkeletalAnimationTime(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetSkeletalAnimationTime: invalid skeletal mesh ID");
		return 0.f;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_time_;
}

int HRL_IsSkeletalAnimationPlaying(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end() || !it->second || it->second->type_ != HRL_3D_SKELETAL_MESH)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_IsSkeletalAnimationPlaying: invalid skeletal mesh ID");
		return HRL_FALSE;
	}
	return static_cast<HRL_SkeletalMesh*>(it->second)->animation_playing_ ? HRL_TRUE : HRL_FALSE;
}

void HRL_UpdateSkeletalAnimations(float _deltaSeconds)
{
	if (!std::isfinite(_deltaSeconds) || _deltaSeconds < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_UpdateSkeletalAnimations: delta seconds must be finite and non-negative");
		return;
	}
	for (const auto& [id, baseMesh] : ctx_.meshes)
	{
		(void)id;
		if (!baseMesh || baseMesh->type_ != HRL_3D_SKELETAL_MESH)
			continue;
		auto* mesh = static_cast<HRL_SkeletalMesh*>(baseMesh);
		if (!mesh->animation_playing_ || mesh->current_animation_ < 0 || (size_t)mesh->current_animation_ >= mesh->animations_.size())
			continue;
		const float duration = std::max(mesh->animations_[(size_t)mesh->current_animation_].public_.duration, 0.f);
		if (duration <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			mesh->animation_playing_ = false;
			HRLSkeletal_UpdatePose(mesh);
			continue;
		}
		mesh->animation_time_ += _deltaSeconds * mesh->animation_speed_;
		if (mesh->animation_loop_)
		{
			mesh->animation_time_ = std::fmod(mesh->animation_time_, duration);
			if (mesh->animation_time_ < 0.f) mesh->animation_time_ += duration;
		}
		else if (mesh->animation_time_ >= duration)
		{
			mesh->animation_time_ = duration;
			mesh->animation_playing_ = false;
		}
		else if (mesh->animation_time_ <= 0.f)
		{
			mesh->animation_time_ = 0.f;
			if (mesh->animation_speed_ < 0.f)
				mesh->animation_playing_ = false;
		}
		const bool poseChanged = HRLSkeletal_UpdatePose(mesh);
		if (poseChanged)
		{
			auto sceneIt = ctx_.scenes.find(mesh->scene_);
			if (sceneIt != ctx_.scenes.end() && sceneIt->second)
				sceneIt->second->shadows_dirty = true;
		}
	}
}

HRL_id HRL_CreateMesh(HRL_id _sceneid, HRL_EMeshType _type, const float *_vertices)
{
	(void)_sceneid;
	(void)_type;
	(void)_vertices;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateMesh: reserved legacy entry point; use HRL_CreateMesh3D");
	return HRL_INVALID_ID;
}

HRL_id HRL_CreateMeshFromFile(HRL_id _sceneid, HRL_EMeshType _type, const char *_data, size_t _bufferSize)
{
	(void)_sceneid;
	(void)_type;
	(void)_data;
	(void)_bufferSize;
	SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateMeshFromFile: generic file-based mesh creation is reserved; use HRL_GetVertex3DFromFBX for FBX conversion");
	return HRL_INVALID_ID;
}

void HRL_DrawSceneAsDebugMode(HRL_id _sceneid, HRL_EDebugView mode)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid scene ID");
		return;
	}

	switch (mode)
	{
	case HRL_DEBUG_VIEW_NONE:
	case HRL_DEBUG_VIEW_UNLIT:
	case HRL_DEBUG_VIEW_NORMAL:
	case HRL_DEBUG_VIEW_LIGHTS:
	case HRL_DEBUG_VIEW_WIREFRAME:
	case HRL_DEBUG_VIEW_LOD:
		it->second->debug_view = mode;
		break;
	default:
		SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_DrawSceneAsDebugMode: invalid debug view mode");
		break;
	}
}



void HRL_MouseMovedCallback(float x, float y)
{
	ctx_.mouseX = x;
	ctx_.mouseY = y;
}

HRL_id HRL_CreateWidget(HRL_id viewport, HRL_EWidgetType type)
{
	auto it = ctx_.viewports.find(viewport);
	if (it == ctx_.viewports.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateWidget: invalid viewport ID");
		return HRL_INVALID_ID;
	}
	HRL_id newId = GenerateHRL_ID();
	HRL_Widget* widget = nullptr;
	switch (type)
	{
		case HRL_WIDGET_BUTTON:
		{
			widget = new HRL_WidgetButton{};
			break;
		}
		default:
		{
			SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateWidget: unsupported widget type");
			return HRL_INVALID_ID;
		}
	}

	ctx_.widgets.emplace(newId, widget);
	it->second->widgets.emplace(newId, widget);
	return newId;
}

void HRL_DeleteWidget(HRL_id widget)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteWidget: invalid widget ID");
		return;
	}

	for (auto& [viewportId, viewport] : ctx_.viewports)
	{
		(void)viewportId;
		if (viewport) viewport->widgets.erase(widget);
	}

	delete it->second;
	ctx_.widgets.erase(it);
}

void HRL_SetWidgetPosition(HRL_id widget, float x, float y)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetPosition: invalid widget ID");
		return;
	}
	it->second->SetPosition(x, y);
}

void HRL_SetWidgetSize(HRL_id widget, float width, float height)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetWidgetSize: invalid widget ID");
		return;
	}
	it->second->SetScale(width, height);
}


//BUTTON WIDGET
void HRL_SetButtonBackgroundTexture(HRL_id widget, HRL_EWidgetState state, HRL_id texture)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid widget ID");
		return;
	}
	auto* widgetptr = dynamic_cast<HRL_WidgetButton*>(it->second);
	if (!widgetptr)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid widget class");
		return;
	}
	if (!HRL_IsValidTexture(texture))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTexture: invalid texture ID");
		return;
	}
	widgetptr->background_texture_ = texture;
}

void HRL_SetButtonBackgroundTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget ID");
		return;
	}
	auto* widgetptr = dynamic_cast<HRL_WidgetButton*>(it->second);
	if (!widgetptr)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget class");
		return;
	}
	widgetptr->background_tint_color_ = {r,g,b,a};
}

void HRL_SetButtonText(HRL_id widget, const char *text)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget ID");
		return;
	}
	auto* widgetptr = dynamic_cast<HRL_WidgetButton*>(it->second);
	if (!widgetptr)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetButtonBackgroundTintColor: invalid widget class");
		return;
	}

	widgetptr->text_text_ = text;
	widgetptr->GenerateTextTexture();
}

void HRL_SetButtonTextFont(HRL_id widget, HRL_id font)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid widget ID");
		return;
	}
	auto* widgetptr = dynamic_cast<HRL_WidgetButton*>(it->second);
	if (!widgetptr)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid widget class");
		return;
	}

	auto it_font = ctx_.fonts.find(font);
	if (it_font == ctx_.fonts.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid font ID");
		return;
	}
	widgetptr->font_ = font;
	widgetptr->GenerateTextTexture();
}

void HRL_SetButtonTextTintColor(HRL_id widget, HRL_EWidgetState state, float r, float g, float b, float a)
{
	auto it = ctx_.widgets.find(widget);
	if (it == ctx_.widgets.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid widget ID");
		return;
	}
	auto* widgetptr = dynamic_cast<HRL_WidgetButton*>(it->second);
	if (!widgetptr)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetButtonTextFont: invalid widget class");
		return;
	}

	widgetptr->text_tint_color_ = {r, g, b, a};
	widgetptr->GenerateTextTexture();
}
