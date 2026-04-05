/**
 * Contient l'implémentation des fichiers :
 * hrl.h, hrl_gl.h, hrl_vulkan.h, hrl_d3d.h
 */

#include "hrl.h"
#include "hrl_gl.h"

#include "core/backend_vtable.h"
#include "core/object_types.h"
//on inclus utils functions car hrl.cpp contient la définition de std::string lastError;
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


//vtable utilisée pour appeller les fonctions, ne doit jamais etre modifié apres Init()
static HRL_vtable g_Backend;


//erreurs//
std::string lastErrorCode;


//variables window//
unsigned int window_width_;
unsigned int window_height_;


//variables textures//
HRL_uint textureMinFilter = HRL_Filter_Linear;
HRL_uint textureMagFilter = HRL_Filter_Linear;


//objects//
typedef struct {
	std::unordered_map<HRL_id, HRL_Mesh*> meshes;
	std::unordered_map<HRL_id, HRL_Light*> lights;
	std::unordered_map<HRL_id, HRL_Viewport*> viewports;
	std::unordered_map<HRL_id, HRL_Camera*> cameras;
	std::unordered_map<HRL_id, HRL_PostProcess*> post_processes;
}hrl_scene_t;
static std::unordered_map<HRL_id, hrl_scene_t*> scenes_;

//ressources copié des scenes (pour favoriser l'acces)
static std::unordered_map<HRL_id, HRL_Mesh*> meshes_;
static std::unordered_map<HRL_id, HRL_Light*> lights_;
static std::unordered_map<HRL_id, HRL_Viewport*> viewports_;
static std::unordered_map<HRL_id, HRL_Camera*> cameras_;
static std::unordered_map<HRL_id, HRL_PostProcess*> post_processes_;

//ressources globales
static std::unordered_map<HRL_id, HRL_Material*> materials_;


//debugs triés par scenes
static std::unordered_map<HRL_id, DebugRenderer> debug_renderers{};
static float debug_line_thickness = 1.f;


//texte
typedef struct {
	stbtt_fontinfo             info;
	std::vector<unsigned char> ttf_buffer;
}HRL_Font;
static std::unordered_map<HRL_id, HRL_Font*> fonts_;


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
	auto it = scenes_.find(_scene);
	if (it == scenes_.end())
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
	lvector.reserve(lights_.size());

	for (const auto& [id, light] : lights_)
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
		g_Backend = GetDirect3D11Backend();
		g_Backend.RHI_Init();
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
	window_width_ = _width;
	window_height_ = _height;
	g_Backend.RHI_InitContext(_width, _height, _loader);
}

void HRL_Shutdown()
{
	//supprimer tous les objets de toutes les scenes
	for (const auto& [scene_id, scene] : scenes_)
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
	scenes_.clear();


	for (const auto& [id, material] : materials_)
	{
		delete material;
	}
	materials_.clear();

	g_Backend.RHI_Shutdown();
}

void HRL_BeginFrame()
{
	//g_Backend.RHI_BeginFrame();
}

void HRL_EndFrame()
{
	//appels à RHI_DrawMesh, HRI_BindMaterial, etc...
	for (const auto& [scene_id, scene] : scenes_)
	{
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

			// --- Draw Sprites --- //
			for (const auto& sprite : GetSortedSprites(scene))
			{
				auto mat_it = materials_.find(sprite->material_);
				if (mat_it == materials_.end())
				{
					//si on trouve pas le material, on passe l'iteration de la boucle
					lastErrorCode = "Tried to draw mesh : material not found";
					continue;
				}
				g_Backend.RHI_BindMaterial(mat_it->second);

				g_Backend.RHI_DrawMesh(sprite);
			}

			auto it_debug = debug_renderers.find(scene_id);
			if (it_debug != debug_renderers.end())
			{
				g_Backend.RHI_DrawDebug(it_debug->second, debug_line_thickness);
			}

			//clear le debug a chaque frame
			debug_renderers.clear();

			// --- Mettre ici le draw des mesh 3D --- //
		}
	}

	g_Backend.RHI_ResetFramebuffer();
}

void HRL_WindowResizeCallback(int _width, int _height)
{
	window_width_ = _width;
	window_height_ = _height;
}


const char* HRL_GetLastError()
{
	if (lastErrorCode.empty())
	{
		return "";
	}
	//on stocke dans une var statique pour eviter un use after free
	static std::string err;
	err = "HRL Error : " + lastErrorCode;
	return err.c_str();
}


//Meshes//
HRL_id HRL_CreateMeshSprite(HRL_id _sceneid)
{
	auto it_scene = scenes_.find(_sceneid);
	if (it_scene == scenes_.end())
	{
		return HRL_InvalidID;
	}
	auto* m = new HRL_Mesh();
	m->type_ = HRL_Sprite;
	//default values
	m->material_ = HRL_InvalidID;
	m->position_ = glm::vec3(0.f);
	m->rotation_ = glm::vec3(0.f);
	m->scale_ = glm::vec3(1.f);

	HRL_id newId = GenerateHRL_ID();
	it_scene->second->meshes.emplace(newId, m);
	meshes_.emplace(newId, m);

	return newId;
}

void HRL_SetSpritePivotPoint(HRL_id _meshid, float x, float y)
{

}

void HRL_SetSpriteUV(HRL_id _meshid, float min_u, float min_v, float max_u, float max_v)
{

}

void HRL_DeleteMesh(HRL_id _meshid)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_DeleteMesh, invalid ID";
		return;
	}
	else
	{
		delete it->second;
		meshes_.erase(_meshid);
	}
}

void HRL_SetMeshMaterial(HRL_id _meshid, HRL_id _matid)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_SetMeshMaterial, invalid ID";
	}
	else
	{
		it->second->material_ = _matid;
	}
}

void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_SetMeshLocation, invalid ID";
	}
	else
	{
		it->second->position_ = glm::vec3(x, y, z);
	}
}

void HRL_SetMeshRotation(HRL_id _meshid, float pitch, float yaw, float roll)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_SetMeshRotation, invalid ID";
	}
	else
	{
		//glm attend : X-pitch, Y-yaw, Z-roll.
		it->second->rotation_ = glm::vec3(pitch, yaw, roll);
	}
}

void HRL_SetMeshScale(HRL_id _meshid, float x, float y, float z)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_SetMeshScale, invalid ID";
	}
	else
	{
		it->second->scale_ = glm::vec3(x, y, z);
	}
}

void HRL_SetSpriteDrawOrder(HRL_id _meshid, float _draworder)
{
	auto it = meshes_.find(_meshid);
	if (it == meshes_.end())
	{
		lastErrorCode = "HRL_SetSpriteDrawOrder, invalid ID";
	}
	else
	{
		it->second->draw_order_ = _draworder;
	}
}



//Lights//
HRL_id HRL_CreateLight(HRL_id _sceneid, HRL_uint _type)
{
	auto it_scene = scenes_.find(_sceneid);
	if (it_scene == scenes_.end())
	{
		return HRL_InvalidID;
	}

	if (_type == HRL_PointLight || _type == HRL_DirectionalLight || _type == HRL_SpotLight)
	{
		auto* l = new HRL_Light();
		l->type_ = _type;

		HRL_id newId = GenerateHRL_ID();
		it_scene->second->lights.emplace(newId, l);
		lights_.emplace(newId, l);

		g_Backend.RHI_UpdateLights(GetLightsVector());

		return newId;
	}
	else
	{
		lastErrorCode = "HRL_CreateLight error : invalid type";
		return HRL_InvalidID;
	}
}

void HRL_DeleteLight(HRL_id _lightid)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_DeleteLight error : invalid id";
	}
	else
	{
		delete it->second;
		lights_.erase(it);

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
}

void HRL_SetLightColor(HRL_id _lightid, float x, float y, float z)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_SetLightColor error : invalid id";
	}
	else
	{
		//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
		it->second->color_ = glm::vec4(x, y, z, 0.f);

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
}

void HRL_SetLightIntensity(HRL_id _lightid, float i)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_SetLightIntensity error : invalid id";
	}
	else
	{
		it->second->intensity_ = i;

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
}

void HRL_SetLightAttenuation(HRL_id _lightid, float a)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_SetLightAttenuation error : invalid id";
	}
	else
	{
		it->second->attenuation_ = a;

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
}

void HRL_SetLightLocation(HRL_id _lightid, float x, float y, float z)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_SetLightLocation error : invalid id";
	}
	else
	{
		//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
		it->second->position_ = glm::vec4(x, y, z, 0.f);

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
}

void HRL_SetLightRotation(HRL_id _lightid, float pitch, float yaw, float roll)
{
	auto it = lights_.find(_lightid);
	if (it == lights_.end())
	{
		lastErrorCode = "HRL_SetLightRotation error : invalid id";
	}
	else
	{
		//rappel : la derniere valeur ne compte pas, elle est juste la pour des raisons techniques
		it->second->rotation_ = glm::vec4(pitch, yaw, roll, 0.f);

		g_Backend.RHI_UpdateLights(GetLightsVector());
	}
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
		lastErrorCode = "HRL_GetTextureSize: width or height are not a valid pointer";
	}
}

void HRL_SetTextureMinFilter(HRL_uint _filter)
{
	textureMinFilter = _filter;
}
void HRL_SetTextureMagFilter(HRL_uint _filter)
{
	textureMagFilter = _filter;
}

HRL_API HRL_id HRL_CreateTextureFromText(const char* _text, HRL_id _fontid,
	float _font_size, float _wrap_width,
	float r, float g, float b,
	float bg_r, float bg_g, float bg_b, float bg_a
)
{
	auto it = fonts_.find(_fontid);
	if (it == fonts_.end())
	{
		lastErrorCode = "HRL_CreateTextureFromText: invalid font ID";
		return HRL_InvalidID;
	}

	BitmapResult bmp = GenerateBitmap(_text, &it->second->info, it->second->ttf_buffer,
			_font_size, _wrap_width,
			r, g, b,
			bg_r, bg_g, bg_b, bg_a
	);
	if (bmp.pixels.empty())
	{
		lastErrorCode = "HRL_CreateTextureFromText: bitmap generation failed, pixels is empty";
		return HRL_InvalidID;
	}

	return g_Backend.RHI_CreateTextureFromBitmap(bmp);
}


//Scenes//
HRL_id HRL_CreateScene(int _renderOnScreen)
{
	auto* scene = new hrl_scene_t();
	HRL_id newId = GenerateHRL_ID();
	scenes_.emplace(newId, scene);
	g_Backend.RHI_CreateScene(newId, _renderOnScreen);
	return newId;
}

void HRL_DeleteScene(HRL_id _sceneid)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end())
	{
		lastErrorCode = "HRL_DeleteScene error : invalid scene ID";
		return;
	}
	g_Backend.RHI_DeleteScene(_sceneid);
	delete it->second;
	scenes_.erase(it);
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
HRL_id HRL_CreatePostProcess(HRL_id _sceneid, HRL_id _matid)
{
	auto it_scene = scenes_.find(_sceneid);
	if (it_scene == scenes_.end())
	{
		lastErrorCode = "HRL_CreatePostProcess error : invalid scene ID";
		return HRL_InvalidID;
	}

	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_CreatePostProcess error : invalid material ID";
		return HRL_InvalidID;
	}

	auto p = new HRL_PostProcess();
	p->material_ = _matid;

	HRL_id newId = GenerateHRL_ID();

	it_scene->second->post_processes.emplace(newId, p);
	post_processes_.emplace(newId, p);

	return newId;
}
void HRL_DeletePostProcess(HRL_id _postid)
{
	auto it = post_processes_.find(_postid);
	if (it == post_processes_.end())
	{
		lastErrorCode = "HRL_DeletePostProcess error : invalid ID";
	}
	else
	{
		delete it->second;
		post_processes_.erase(it);
	}
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
	materials_.emplace(newId, m);

	return newId;
}

void HRL_DeleteMaterial(HRL_id _matid)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_DeleteMaterial, invalid ID";
	}
	else
	{
		delete it->second;
		materials_.erase(it);
	}
}

void HRL_MaterialSetInt(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetInt, invalid ID";
	}
	else
	{
		//si la clée n'existe pas, elle est créée
		it->second->intParams_[_uniformName] = a;
	}
}

void HRL_MaterialSetTexture(HRL_id _matid, const char* _uniformName, HRL_id _textureid)
{
	//on v�rifie que le mat existe
	auto it_mat = materials_.find(_matid);
	if (it_mat == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetTexture, invalid material ID";
		return;
	}

	//on v�rifie que la texture existe au moment de RHI_BindMaterial, car ici on a pas acces aux textures
	it_mat->second->textureParams_[_uniformName] = _textureid;
}

void HRL_MaterialSetBool(HRL_id _matid, const char* _uniformName, int a)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetBool, invalid ID";
	}
	else
	{
		it->second->intParams_[_uniformName] = a;
	}
}

void HRL_MaterialSetFloat(HRL_id _matid, const char* _uniformName, float a)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetFloat, invalid ID";
	}
	else
	{
		it->second->floatParams_[_uniformName] = a;
	}
}

void HRL_MaterialSetVec2(HRL_id _matid, const char* _uniformName, float x, float y)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetVec2, invalid ID";
	}
	else
	{
		it->second->vec2Params_[_uniformName] = glm::vec2(x, y);
	}
}

void HRL_MaterialSetVec3(HRL_id _matid, const char* _uniformName, float x, float y, float z)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetVec3, invalid ID";
	}
	else
	{
		it->second->vec3Params_[_uniformName] = glm::vec3(x, y, z);
	}
}

void HRL_MaterialSetVec4(HRL_id _matid, const char* _uniformName, float x, float y, float z, float w)
{
	auto it = materials_.find(_matid);
	if (it == materials_.end())
	{
		lastErrorCode = "HRL_MaterialSetVec4, invalid ID";
	}
	else
	{
		it->second->vec4Params_[_uniformName] = glm::vec4(x, y, z, w);
	}
}

HRL_id HRL_CreateMaterialFromJson(const char *_jsonData, size_t _jsonSize)
{
	nlohmann::json jfile = nlohmann::json::parse(_jsonData);
	if (!jfile.contains("surface") || !jfile.contains("shader"))
	{
		lastErrorCode = "HRL_CreateMaterialFromJson : file doesn't contains at least 'surface' or 'shader' key";
		return HRL_InvalidID;
	}

	HRL_id matID = HRL_CreateMaterial(HRL_SpriteShader);
	for (const auto& [key, value] : jfile["surface"].items())
	{
		//HRL_MaterialSetTexture(matID, key, )
	}
	return matID;
}




HRL_id HRL_CreateViewport(HRL_id _sceneid, HRL_id _cameraid, float x, float y, float _width, float _height)
{
	auto it_scene = scenes_.find(_sceneid);
	if (it_scene == scenes_.end())
	{
		lastErrorCode = "HRL_CreateViewport error : invalid scene ID";
		return HRL_InvalidID;
	}

	HRL_Camera* cam=nullptr;

	auto it = cameras_.find(_cameraid);
	if (it != cameras_.end())
	{
		cam=it->second;
	}

	//camera valide
	auto* v = new HRL_Viewport(cam, x, y, _width, _height);

	HRL_id newId = GenerateHRL_ID();
	it_scene->second->viewports.emplace(newId, v);
	viewports_.emplace(newId, v);

	return newId;
}

void HRL_DeleteViewport(HRL_id _viewportid)
{
	auto it = viewports_.find(_viewportid);
	if (it == viewports_.end())
	{
		lastErrorCode = "HRL_DeleteViewport, invalid ID";
	}
	else
	{
		delete it->second;
		viewports_.erase(it);
	}
}

void HRL_SetViewportCamera(HRL_id _viewportid, HRL_id _camid)
{
	auto viewport_it = viewports_.find(_viewportid);
	if (viewport_it == viewports_.end())
	{
		lastErrorCode = "HRL_SetViewportCamera error : invalid Viewport ID";
		return;
	}

	HRL_Camera* cam = nullptr;

	auto cam_it = cameras_.find(_camid);
	if (cam_it != cameras_.end())
	{
		cam = cam_it->second;
	}

	viewport_it->second->camera_ = cam;
}

void HRL_SetViewportRect(HRL_id _viewportid, float x, float y, float _width, float _height)
{
	auto it = viewports_.find(_viewportid);
	if (it == viewports_.end())
	{
		lastErrorCode = "HRL_SetViewportRect, invalid ID";
	}
	else
	{
		it->second->x_ = x;
		it->second->y_ = y;
		it->second->width_= _width;
		it->second->height_= _height;
	}
}



HRL_id HRL_CreateCamera(HRL_id _sceneid, HRL_uint _type)
{
	auto it_scene = scenes_.find(_sceneid);
	if (it_scene == scenes_.end())
	{
		lastErrorCode = "HRL_CreateCamera error : invalid scene ID";
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
		cameras_.emplace(newId, cam);
		return newId;
	}
	else
	{
		return HRL_InvalidID;
	}
}

void HRL_DeleteCamera(HRL_id _camid)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_DeleteCamera error : invalid ID";
	}
	else
	{
		delete it->second;
		cameras_.erase(it);
	}
}


void HRL_SetCameraType(HRL_id _camid, HRL_uint _type)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraView, invalid ID";
	}
	else
	{
		it->second->type_ = _type;
	}
}

void HRL_SetCameraOrthoVertical(HRL_id _camid, float _height)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraOrthoVertical, invalid ID";
	}
	else
	{
		if (it->second->type_ == HRL_Ortho)
		{
			it->second->value_ = _height;
		}
		else
		{
			lastErrorCode = "(weak warning) : HRL_SetCameraOrthoVertical, camera is not of type Ortho";
		}
	}
}

void HRL_SetCameraPerspectiveFov(HRL_id _camid, float _fov)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraPerspectiveFov, invalid ID";
	}
	else
	{
		if (it->second->type_ == HRL_Perspective)
		{
			it->second->value_ = _fov;
		}
		else
		{
			lastErrorCode = "(weak warning) : HRL_SetCameraPerspectiveFov, camera is not of type Perspective";
		}
	}
}

void HRL_SetCameraNearPlane(HRL_id _camid, float _nearPlane)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraNearPlane, invalid ID";
	}
	else
	{
		it->second->near_plane_ = _nearPlane;
	}
}

void HRL_SetCameraFarPlane(HRL_id _camid, float _farPlane)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraFarPlane, invalid ID";
	}
	else
	{
		it->second->far_plane_ = _farPlane;
	}
}

void HRL_SetCameraPosition(HRL_id _camid, float x, float y, float z)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraPosition, invalid ID";
	}
	else
	{
		it->second->position_ = glm::vec3(x, y, z);
	}
}

void HRL_SetCameraRotation(HRL_id _camid, float pitch, float yaw, float roll)
{
	auto it = cameras_.find(_camid);
	if (it == cameras_.end())
	{
		lastErrorCode = "HRL_SetCameraRotation, invalid ID";
	}
	else
	{
		it->second->rotation_ = glm::vec3(pitch, yaw, roll);
	}
}


//Debug

void HRL_SetDebugLineThickness(float a)
{
	debug_line_thickness = a;
}

void HRL_DrawDebugSegment(HRL_id _sceneid, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float r, float g, float b)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end())
	{
		SetErrorCode("HRL_DebugDrawSegment, sceneid is not valid");
		return;
	}

	//scene id is valid

	debug_renderers[_sceneid].lines.emplace_back(a_x, a_y, a_z, r, g, b);
	debug_renderers[_sceneid].lines.emplace_back(b_x, b_y, b_z, r, g, b);
}

void HRL_DrawDebugPolygon(HRL_id _sceneid, HRL_uint _mode, const float *vertices_x, const float *vertices_y, const float *vertices_z, int vertices_count, float r, float g, float b)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end())
	{
		SetErrorCode("HRL_DebugDrawSegment, sceneid is not valid");
		return;
	}

	//scene id is valid

	if (_mode == HRL_DebugSolid)
	{
		for (int i=1; i < vertices_count-1; i++)
		{
			//pivot
			debug_renderers[_sceneid].triangles.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
			//i
			debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			//i+1
			debug_renderers[_sceneid].triangles.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}
	}
	else if (_mode == HRL_DebugHollow)
	{
		for (int i=0; i < vertices_count - 1; i++)
		{
			debug_renderers[_sceneid].lines.emplace_back(vertices_x[i], vertices_y[i], vertices_z[i], r, g, b);
			debug_renderers[_sceneid].lines.emplace_back(vertices_x[i+1], vertices_y[i+1], vertices_z[i+1], r, g, b);
		}

		//line entre le dernier et le 0 pour refermer
		debug_renderers[_sceneid].lines.emplace_back(vertices_x[vertices_count-1], vertices_y[vertices_count-1], vertices_z[vertices_count-1], r, g, b);
		debug_renderers[_sceneid].lines.emplace_back(vertices_x[0], vertices_y[0], vertices_z[0], r, g, b);
	}
}

void HRL_DrawDebugCircle(HRL_id _sceneid, HRL_uint _mode, float center_x, float center_y, float center_z, float radius, float segments, float r, float g, float b)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end()) { SetErrorCode("HRL_DrawDebugCircle, sceneid is not valid"); return; }

	int seg = (int)segments;
	auto* vx = (float*)alloca(seg * sizeof(float));
	auto* vy = (float*)alloca(seg * sizeof(float));
	auto* vz = (float*)alloca(seg * sizeof(float));

	for (int i = 0; i < seg; i++)
	{
		float angle = (float)(2.f * M_PI * i) / (float)seg;
		vx[i] = center_x + radius * cosf(angle);
		vy[i] = center_y + radius * sinf(angle);
		vz[i] = center_z;
	}

	HRL_DrawDebugPolygon(_sceneid, _mode, vx, vy, vz, seg, r, g, b);
}

void HRL_DrawDebugCapsule(HRL_id _sceneid, HRL_uint _mode, float a_x, float a_y, float a_z, float b_x, float b_y, float b_z, float radius, float segments, float r, float g, float b)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end()) { SetErrorCode("HRL_DrawDebugCapsule, sceneid is not valid"); return; }

	int half_seg = (int)segments / 2;

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

		debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a0), b_y + radius * sinf(a0), b_z, r, g, b);
		debug_renderers[_sceneid].lines.emplace_back(b_x + radius * cosf(a1), b_y + radius * sinf(a1), b_z, r, g, b);
	}

	// demi-cercle autour de A — face opposée à B
	// plage : [base_angle + PI/2 → base_angle + 3PI/2]
	for (int i = 0; i < half_seg; i++)
	{
		float a0 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)i       / (float)half_seg;
		float a1 = base_angle + (float)M_PI / 2.f + (float)M_PI * (float)(i + 1) / (float)half_seg;

		debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a0), a_y + radius * sinf(a0), a_z, r, g, b);
		debug_renderers[_sceneid].lines.emplace_back(a_x + radius * cosf(a1), a_y + radius * sinf(a1), a_z, r, g, b);
	}

	// deux segments latéraux reliant les demi-cercles
	debug_renderers[_sceneid].lines.emplace_back(a_x + nx * radius, a_y + ny * radius, a_z, r, g, b);
	debug_renderers[_sceneid].lines.emplace_back(b_x + nx * radius, b_y + ny * radius, b_z, r, g, b);

	debug_renderers[_sceneid].lines.emplace_back(a_x - nx * radius, a_y - ny * radius, a_z, r, g, b);
	debug_renderers[_sceneid].lines.emplace_back(b_x - nx * radius, b_y - ny * radius, b_z, r, g, b);

}

void HRL_DrawDebugPoint(HRL_id _sceneid, float a_x, float a_y, float a_z, float size, float r, float g, float b)
{
	auto it = scenes_.find(_sceneid);
	if (it == scenes_.end()) { SetErrorCode("HRL_DrawDebugPoint, sceneid is not valid"); return; }

	float h = size * 0.5f;

	debug_renderers[_sceneid].lines.emplace_back(a_x - h, a_y,     a_z, r, g, b);
	debug_renderers[_sceneid].lines.emplace_back(a_x + h, a_y,     a_z, r, g, b);

	debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y - h, a_z, r, g, b);
	debug_renderers[_sceneid].lines.emplace_back(a_x,     a_y + h, a_z, r, g, b);

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
		lastErrorCode = "HRL_CreateFont: failed to parse TTF data";
		return HRL_InvalidID;
	}

	fonts_.emplace(newId, font);

	return newId;
}

void HRL_DeleteFont(HRL_id _fontid)
{
	auto it = fonts_.find(_fontid);
	if (it == fonts_.end())
	{
		lastErrorCode = "HRL_DeleteFont: invalid ID";
		return;
	}
	delete it->second;
	fonts_.erase(it);
}
