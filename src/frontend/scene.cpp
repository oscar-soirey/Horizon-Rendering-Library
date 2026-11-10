// HRL frontend - scenes, global illumination settings, sky, post-process.
#include "frontend_internal.h"

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
	std::vector<HRL_id> mesh_ids, landscape_ids, light_ids, fog_ids, gizmo_ids, vfx_system_ids, viewport_ids, camera_ids;
	for (const auto& [id, mesh]     : it->second->meshes)          mesh_ids.push_back(id);
	for (const auto& [id, landscape] : it->second->landscapes)   { (void)landscape; landscape_ids.push_back(id); }
	for (const auto& [id, light]    : it->second->lights)         light_ids.push_back(id);
	for (const auto& [id, fog]      : it->second->volumetric_fogs) fog_ids.push_back(id);
	for (const auto& [id, gizmo]   : it->second->gizmos)          gizmo_ids.push_back(id);
	for (const auto& [id, system] : it->second->vfx_systems)        { (void)system; vfx_system_ids.push_back(id); }
	for (const auto& [id, viewport] : it->second->viewports)      viewport_ids.push_back(id);
	for (const auto& [id, camera]   : it->second->cameras)        camera_ids.push_back(id);

	//delete every objects that owns the scene
	for (auto id : mesh_ids)      HRL_DeleteMesh(id);
	for (auto id : landscape_ids) HRL_DeleteLandscape(id);
	for (auto id : light_ids)     HRL_DeleteLight(id);
	for (auto id : fog_ids)       HRL_DeleteVolumetricFog(id);
	for (auto id : gizmo_ids)     HRL_DeleteGizmo(id);
	for (auto id : vfx_system_ids) HRL_DeleteVFXSystem(id);
	for (auto id : viewport_ids)  HRL_DeleteViewport(id);
	for (auto id : camera_ids)    HRL_DeleteCamera(id);

	delete it->second->voxel_world;
	it->second->voxel_world = nullptr;
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

void HRL_SetGlobalIlluminationIntensity(HRL_id _sceneid, float _intensity)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationIntensity: invalid scene ID");
		return;
	}
	if (!std::isfinite(_intensity) || _intensity < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetGlobalIlluminationIntensity: intensity must be >= 0");
		return;
	}
	it->second->global_illumination_intensity = _intensity;
}

float HRL_GetGlobalIlluminationIntensity(HRL_id _sceneid)
{
	auto it = ctx_.scenes.find(_sceneid);
	if (it == ctx_.scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetGlobalIlluminationIntensity: invalid scene ID");
		return 0.f;
	}
	return it->second->global_illumination_intensity;
}

void HRL_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	g_Backend.RHI_ResizeSceneTexture(_sceneid, _width, _height);
}

HRL_id HRL_GetHoveredObject(HRL_id _scene, int mouseX, int mouseY, HRL_EMeshType* mesh_type)
{
	if (ctx_.scenes.find(_scene) == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetHoveredObject: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (!g_Backend.RHI_GetHoveredObject)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "HRL_GetHoveredObject: not supported by the active backend");
		return HRL_INVALID_ID;
	}
	return g_Backend.RHI_GetHoveredObject(_scene, mouseX, mouseY, mesh_type);
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
