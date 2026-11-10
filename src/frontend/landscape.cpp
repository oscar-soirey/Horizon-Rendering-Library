// HRL frontend - landscapes.
#include "frontend_internal.h"

static HRL_Landscape* GetLandscapeForEdit(HRL_id id, const char* fn)
{
	auto it = ctx_.landscapes.find(id);
	if (it == ctx_.landscapes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid landscape ID");
		return nullptr;
	}
	return it->second;
}

HRL_id HRL_CreateLandscape(HRL_id _sceneid, HRL_id _heightmap)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (_heightmap == HRL_INVALID_ID || !g_Backend.RHI_IsValidTexture || !g_Backend.RHI_IsValidTexture(_heightmap))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: invalid heightmap texture ID");
		return HRL_INVALID_ID;
	}
	auto* landscape = new (std::nothrow) HRL_Landscape();
	if (!landscape)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: allocation failed");
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	landscape->id_ = id;
	landscape->scene_ = _sceneid;
	landscape->heightmap_ = _heightmap;
	sceneIt->second->landscapes.emplace(id, landscape);
	ctx_.landscapes.emplace(id, landscape);
	sceneIt->second->shadows_dirty = true;
	++sceneIt->second->gi_geometry_revision;
	return id;
}

void HRL_DeleteLandscape(HRL_id _landscape)
{
	auto it = ctx_.landscapes.find(_landscape);
	if (it == ctx_.landscapes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteLandscape: invalid landscape ID");
		return;
	}
	HRL_Landscape* landscape = it->second;
	if (auto sceneIt = ctx_.scenes.find(landscape->scene_); sceneIt != ctx_.scenes.end())
	{
		sceneIt->second->landscapes.erase(_landscape);
		sceneIt->second->shadows_dirty = true;
		++sceneIt->second->gi_geometry_revision;
	}
	ctx_.landscapes.erase(it);
	delete landscape;
}

int HRL_IsValidLandscape(HRL_id _landscape)
{
	return ctx_.landscapes.find(_landscape) != ctx_.landscapes.end() ? HRL_TRUE : HRL_FALSE;
}

void HRL_SetLandscapeHeightmap(HRL_id _landscape, HRL_id _heightmap)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeHeightmap");
	if (!landscape) return;
	if (_heightmap == HRL_INVALID_ID || !g_Backend.RHI_IsValidTexture || !g_Backend.RHI_IsValidTexture(_heightmap))
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLandscapeHeightmap: invalid heightmap texture ID");
		return;
	}
	landscape->heightmap_ = _heightmap;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapePosition(HRL_id _landscape, float x, float y, float z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapePosition");
	if (!landscape) return;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapePosition: values must be finite");
		return;
	}
	landscape->position_ = glm::vec3(x, y, z);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeRotation(HRL_id _landscape, float pitch, float yaw, float roll)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeRotation");
	if (!landscape) return;
	if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeRotation: values must be finite");
		return;
	}
	landscape->rotation_ = glm::vec3(pitch, yaw, roll);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeScale(HRL_id _landscape, float x, float y, float z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeScale");
	if (!landscape) return;
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x <= 0.f || y <= 0.f || z <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeScale: scale components must be finite and > 0");
		return;
	}
	landscape->scale_ = glm::vec3(x, y, z);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeSize(HRL_id _landscape, float width, float depth)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeSize");
	if (!landscape) return;
	if (!std::isfinite(width) || !std::isfinite(depth) || width <= 0.f || depth <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeSize: width/depth must be finite and > 0");
		return;
	}
	landscape->size_ = glm::vec2(width, depth);
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end()) MarkSceneGIGeometryDirty(it->second);
}

void HRL_SetLandscapeHeight(HRL_id _landscape, float height)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeHeight");
	if (!landscape) return;
	if (!std::isfinite(height) || height <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeHeight: height must be finite and > 0");
		return;
	}
	landscape->height_scale_ = height;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapeResolution(HRL_id _landscape, HRL_uint x, HRL_uint z)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeResolution");
	if (!landscape) return;
	x = std::clamp<HRL_uint>(x, 2u, 512u);
	z = std::clamp<HRL_uint>(z, 2u, 512u);
	if (landscape->resolution_x_ == x && landscape->resolution_z_ == z) return;
	landscape->resolution_x_ = x;
	landscape->resolution_z_ = z;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

void HRL_SetLandscapeUVScale(HRL_id _landscape, float u, float v)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeUVScale");
	if (!landscape) return;
	if (!std::isfinite(u) || !std::isfinite(v) || u <= 0.f || v <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeUVScale: values must be finite and > 0");
		return;
	}
	landscape->uv_scale_ = glm::vec2(u, v);
}

void HRL_SetLandscapeMaterial(HRL_id _landscape, HRL_id _material)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeMaterial");
	if (!landscape) return;
	if (_material != HRL_INVALID_ID && ctx_.materials.find(_material) == ctx_.materials.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetLandscapeMaterial: invalid material ID");
		return;
	}
	landscape->material_ = _material;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
		it->second->shadows_dirty = true;
	MarkAllScenesGILightingDirty();
}
