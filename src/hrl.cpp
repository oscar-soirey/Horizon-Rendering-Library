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

// Internal FBX decoding dependency. Add ufbx to the build; HRL only consumes
// its public header here and converts the decoded data to HRL_Vertex3D.
#include <ufbx/ufbx.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

//pour le texte
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>


static HRL_Context ctx_;

//vtable utilisée pour appeller les fonctions, ne doit jamais etre modifiée apres Init()
static HRL_vtable g_Backend;


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



/// API Implementation ///

void HRL_Init(HRL_E_APIs _api)
{
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

	//nettoyer les caches flat
	ctx_.meshes.clear();
	ctx_.lights.clear();
	ctx_.viewports.clear();
	ctx_.cameras.clear();
	ctx_.post_processes.clear();

	for (const auto& [id, material] : ctx_.materials)
	{
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
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE) scene_it->second->shadows_dirty = true;
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
			scene_it->second->shadows_dirty = true;
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
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE) scene_it->second->shadows_dirty = true;
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
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE) scene_it->second->shadows_dirty = true;
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
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE) scene_it->second->shadows_dirty = true;
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
	if (it->second->type_ == HRL_SPRITE)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"LOD is only supported for 3D meshes");
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
	if (scene_it != ctx_.scenes.end()) scene_it->second->shadows_dirty = true;

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
	if (scene_it != ctx_.scenes.end()) scene_it->second->shadows_dirty = true;

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
	if (scene_it != ctx_.scenes.end()) scene_it->second->shadows_dirty = true;
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
	if (scene_it != ctx_.scenes.end()) scene_it->second->shadows_dirty = true;

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
	if (scene_it != ctx_.scenes.end()) scene_it->second->shadows_dirty = true;

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

	g_Backend.RHI_DeleteScene(_sceneid);

	ctx_.scenes.erase(it);
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
	pp->material_ = _matid;

	HRL_id newId = GenerateHRL_ID();

	ctx_.post_processes.emplace(newId, pp);
	it_viewport->second->post_processes.emplace(priority, pp);

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

	//retire de la scene propriétaire
	for (auto& [scene_id, viewports] : ctx_.viewports)
	{
		auto sit = viewports->post_processes.find(_postid);
		if (sit != viewports->post_processes.end())
		{
			viewports->post_processes.erase(sit);
			break;
		}
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
	delete it->second;
	ctx_.materials.erase(it);
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
}



HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid scene ID");
		return HRL_INVALID_ID;
	}

	HRL_Camera* cam = nullptr;

	auto it = ctx_.cameras.find(_cameraid);
	if (it != ctx_.cameras.end())
	{
		cam = it->second;
	}

	//camera valide
	auto* v = new HRL_Viewport(cam, x, y, _width, _height);

	HRL_id newId = GenerateHRL_ID();
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

	HRL_Camera* cam = nullptr;

	auto cam_it = ctx_.cameras.find(_camid);
	if (cam_it != ctx_.cameras.end())
	{
		cam = cam_it->second;
	}

	viewport_it->second->camera_ = cam;
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
	it_scene->second->shadows_dirty = true;
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
		default: { break; }
	}

	ctx_.widgets.emplace(newId, widget);
	it->second->widgets.emplace(newId, widget);
	return newId;
}

void HRL_DeleteWidget(HRL_id widget)
{

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
