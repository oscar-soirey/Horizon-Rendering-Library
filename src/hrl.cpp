/**
 * Contient l'implémentation des fichiers :
 * hrl.h, hrl_gl.h, hrl_vulkan.h, hrl_d3d.h
 */

#include "hrl.h"
#include "hrl_gl.h"

#include "core/backend_vtable.h"
#include "core/object_types.h"
#include "core/utils_functions.h"

#include "backend/opengl33/gl33_backend.h"

#include <unordered_map>
#include <string>
#include <algorithm>
#include <vector>

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
		if (mesh->type_ == HRL_Sprite)
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

std::vector<HRL_Light*> GetLightsVector(/*HRL_id _scene*/)
{/**
	auto it = ctx_.scenes.find(_scene);
	if (it == ctx_.scenes.end())
	{
		return {};
	}

	std::vector<HRL_Light*> lvector;

	//on réserve la taille pour eviter l'alocation a chaque boucle
	lvector.reserve(it->second->lights.size());

	for (const auto& [id, light] : it->second->lights)
	{
		lvector.push_back(light);
	}
	return lvector;
	*/

	std::vector<HRL_Light*> lvector;

	//on réserve la taille pour eviter l'alocation a chaque boucle
	lvector.reserve(ctx_.lights.size());

	for (const auto& [id, light] : ctx_.lights)
	{
		lvector.push_back(light);
	}
	return lvector;
}



/// API Implementation ///

void HRL_Init(HRL_uint _api)
{
	switch (_api)
	{
	case HRL_OpenGL33 :
	{
		g_Backend = GetOpenGL33Backend();
		g_Backend.RHI_Init();
		break;
	}
	case HRL_OpenGL45 :
	{
		break;
	}
	case HRL_Vulkan :
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
	case HRL_Metal :
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
		assert("HRL : Backend not supported");
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
			delete mesh;
		}
		scene->meshes.clear();

		for (const auto& [id, light] : scene->lights)
		{
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
		g_Backend.RHI_RenderScene(scene, scene_id);
	}
	//g_Backend.RHI_ResetFramebuffer();
}

void HRL_WindowResizeCallback(int _width, int _height)
{
	ctx_.window_width  = _width;
	ctx_.window_height = _height;
}


HRL_Error HRL_GetLastError(const char** _detail, HRL_Severity* _severity)
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

const char* HRL_ErrorEnumToString(HRL_Error err)
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

const char* HRL_SeverityEnumToString(HRL_Severity sev)
{
	int index = static_cast<int>(sev) - HRL_SEVERITY_BASE;

	if (index < 0 || index >= HRL_SEVERITY_COUNT)
		return "UNKNOWN_SEVERITY";

	return severity_str[index];
}

void HRL_RegisterErrorCallback(HRL_ErrorCallback _callback)
{
	ctx_.error_callback = _callback;
}


//Meshes//
HRL_id HRL_CreateMeshSprite(HRL_id _sceneid)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateMeshSprite: invalid scene ID");
		return HRL_InvalidID;
	}
	auto* m = new HRL_MeshSprite();
	m->scene_ = _sceneid;
	m->type_  = HRL_Sprite;

	HRL_id newId = GenerateHRL_ID();
	it_scene->second->meshes.emplace(newId, m);
	ctx_.meshes.emplace(newId, m);

	return newId;
}

void HRL_SetMeshPivotPoint(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshPivotPoint: invalid ID");
		return;
	}
	it->second->pivot_point_ = {x, y, z};
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: invalid ID");
		return;
	}
	auto* mesh = dynamic_cast<HRL_MeshSprite*>(it->second);
	if (!mesh)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_SetSpriteRegion: trying to set region on a non-sprite mesh");
		return;
	}
	mesh->region_[0] = min_u;
	mesh->region_[1] = min_v;
	mesh->region_[2] = max_u;
	mesh->region_[3] = max_v;
}

void HRL_DeleteMesh(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMesh: invalid ID");
		return;
	}

	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end())
	{
		scene_it->second->meshes.erase(_meshid);
	}

	delete it->second;
	ctx_.meshes.erase(it);
}

void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshMaterial: invalid ID");
		return;
	}
	it->second->material_ = _matid;
}

void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshLocation: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
}

void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshRotation: invalid ID");
		return;
	}
	//glm attend : X-pitch, Y-yaw, Z-roll.
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
}

void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshScale: invalid ID");
		return;
	}
	it->second->scale_ = glm::vec3(x, y, z);
}

void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetSpriteDrawOrder: invalid ID");
		return;
	}
	it->second->draw_order_ = _draworder;
}



//Lights//
HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_uint _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLight: invalid scene ID");
		return HRL_InvalidID;
	}

	if (_type == HRL_PointLight || _type == HRL_DirectionalLight || _type == HRL_SpotLight)
	{
		auto* l = new HRL_Light();
		l->type_ = _type;

		HRL_id newId = GenerateHRL_ID();
		it_scene->second->lights.emplace(newId, l);
		ctx_.lights.emplace(newId, l);

		g_Backend.RHI_UpdateLights(GetLightsVector());

		return newId;
	}
	SetErrorCode(HRL_INVALID_ENUM, HRL_SEVERITY_ERROR, "HRL_CreateLight: invalid light type");
	return HRL_InvalidID;
}

void HRL_DeleteLight(HRL_id _lightid)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLight: invalid ID");
		return;
	}

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->lights.find(_lightid);
		if (sit != scene->lights.end())
		{
			scene->lights.erase(sit);
			break;
		}
	}

	delete it->second;
	ctx_.lights.erase(it);

	g_Backend.RHI_UpdateLights(GetLightsVector());
}

void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightColor: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->color_ = glm::vec4(x, y, z, 0.f);

	g_Backend.RHI_UpdateLights(GetLightsVector());
}

void HRL_SetLightIntensity(HRL_id _lightid, float i)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightIntensity: invalid ID");
		return;
	}
	it->second->intensity_ = i;

	g_Backend.RHI_UpdateLights(GetLightsVector());
}

void HRL_SetLightAttenuation(HRL_id _lightid, float a)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightAttenuation: invalid ID");
		return;
	}
	it->second->attenuation_ = a;

	g_Backend.RHI_UpdateLights(GetLightsVector());
}

void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightLocation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->position_ = glm::vec4(x, y, z, 0.f);

	g_Backend.RHI_UpdateLights(GetLightsVector());
}

void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll)
{
	auto it = ctx_.lights.find(_lightid);
	if (it == ctx_.lights.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLightRotation: invalid ID");
		return;
	}
	//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
	it->second->rotation_ = glm::vec4(pitch, yaw, roll, 0.f);

	g_Backend.RHI_UpdateLights(GetLightsVector());
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

void HRL_SetTextureMinFilter(HRL_id _textureid, HRL_uint _filter)
{
	g_Backend.RHI_SetTextureMinFilter(_textureid, _filter);
}
void HRL_SetTextureMagFilter(HRL_id _textureid, HRL_uint _filter)
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: invalid font ID");
		return HRL_InvalidID;
	}

	BitmapResult bmp = GenerateBitmap(_text, &it->second->info, it->second->ttf_buffer,
			_font_size, _wrap_width,
			r, g, b,
			bg_r, bg_g, bg_b, bg_a
	);
	if (bmp.pixels.empty())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateTextureFromText: bitmap generation failed, pixels is empty");
		return HRL_InvalidID;
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteScene: invalid scene ID");
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



//Post Process//
HRL_id HRL_CreatePostProcess(HRL_id _sceneid, HRL_id _matid, int priority)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid scene ID");
		return HRL_InvalidID;
	}

	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreatePostProcess: invalid material ID");
		return HRL_InvalidID;
	}

	auto p = new HRL_PostProcess();
	p->material_ = _matid;
	p->priority  = priority;

	HRL_id newId = GenerateHRL_ID();

	g_Backend.RHI_CreatePostProcess(_matid, priority);

	it_scene->second->post_processes.emplace(newId, p);
	ctx_.post_processes.emplace(newId, p);

	return newId;
}
void HRL_DeletePostProcess(HRL_id _postid)
{
	auto it = ctx_.post_processes.find(_postid);
	if (it == ctx_.post_processes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeletePostProcess: invalid ID");
		return;
	}

	//retire de la scene propriétaire
	for (auto& [scene_id, scene] : ctx_.scenes)
	{
		auto sit = scene->post_processes.find(_postid);
		if (sit != scene->post_processes.end())
		{
			scene->post_processes.erase(sit);
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteMaterial: invalid ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetInt: invalid ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetTexture: invalid material ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetBool: invalid ID");
		return;
	}
	it->second->intParams_[_uniformName] = a;
}

void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetFloat: invalid ID");
		return;
	}
	it->second->floatParams_[_uniformName] = a;
}

void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec2: invalid ID");
		return;
	}
	it->second->vec2Params_[_uniformName] = glm::vec2(x, y);
}

void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec3: invalid ID");
		return;
	}
	it->second->vec3Params_[_uniformName] = glm::vec3(x, y, z);
}

void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w)
{
	auto it = ctx_.materials.find(_matid);
	if (it == ctx_.materials.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_MaterialSetVec4: invalid ID");
		return;
	}
	it->second->vec4Params_[_uniformName] = glm::vec4(x, y, z, w);
}



HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateViewport: invalid scene ID");
		return HRL_InvalidID;
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteViewport: invalid ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportCamera: invalid viewport ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetViewportRect: invalid ID");
		return;
	}
	it->second->x_      = x;
	it->second->y_      = y;
	it->second->width_  = _width;
	it->second->height_ = _height;
}



HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_uint _type)
{
	auto it_scene = ctx_.scenes.find(_sceneid);
	if (it_scene == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateCamera: invalid scene ID");
		return HRL_InvalidID;
	}

	if (_type == HRL_Ortho || _type == HRL_Perspective)
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
		return HRL_InvalidID;
	}
}

void HRL_DeleteCamera(HRL_id _camid)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteCamera: invalid ID");
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


void HRL_SetCameraType(HRL_id _camid, HRL_uint _type)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraType: invalid ID");
		return;
	}
	it->second->type_ = _type;
}

void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraOrthoVertical: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_Ortho)
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraPerspectiveFov: invalid ID");
		return;
	}
	if (it->second->type_ == HRL_Perspective)
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraNearPlane: invalid ID");
		return;
	}
	it->second->near_plane_ = _nearPlane;
}

void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraFarPlane: invalid ID");
		return;
	}
	it->second->far_plane_ = _farPlane;
}

void HRL_SetCameraPosition(HRL_id _camid, float x, float y, float z)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraPosition: invalid ID");
		return;
	}
	it->second->position_ = glm::vec3(x, y, z);
}

void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll)
{
	auto it = ctx_.cameras.find(_camid);
	if (it == ctx_.cameras.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetCameraRotation: invalid ID");
		return;
	}
	it->second->rotation_ = glm::vec3(pitch, yaw, roll);
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetModelMatrix: Invalid ID");
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
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugSegment: invalid scene ID");
		return;
	}

	ctx_.debug_renderers[_sceneid].lines.emplace_back(a_x, a_y, a_z, r, g, b);
	ctx_.debug_renderers[_sceneid].lines.emplace_back(b_x, b_y, b_z, r, g, b);
}

void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_uint _mode, const float *vertices_x, const float *vertices_y, const float *vertices_z, int vertices_count, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPolygon: invalid scene ID");
		return;
	}

	if (_mode == HRL_DebugSolid)
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
	else if (_mode == HRL_DebugHollow)
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

void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_uint _mode, float center_x, float center_y, float center_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCircle: invalid scene ID"); return; }

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

void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_uint _mode, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float radius, int segments, float r, float g, float b)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugCapsule: invalid scene ID"); return; }

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
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DrawDebugPoint: invalid scene ID"); return; }

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
		return HRL_InvalidID;
	}

	ctx_.fonts.emplace(newId, font);

	return newId;
}

void HRL_DeleteFont(HRL_id _fontid)
{
	auto it = ctx_.fonts.find(_fontid);
	if (it == ctx_.fonts.end())
	{
		SetErrorCode(HRL_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteFont: invalid ID");
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