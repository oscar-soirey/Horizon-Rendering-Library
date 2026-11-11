// HRL frontend - landscapes.
#include "frontend_internal.h"
#include "../core/render_common.h"

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

// ---------------------------------------------------------------------------
// Heights on the CPU
// ---------------------------------------------------------------------------
static void MarkHeightsChanged(HRL_Landscape* landscape, HRL_uint x0, HRL_uint z0, HRL_uint x1, HRL_uint z1)
{
	HRL_uint* d = landscape->dirty_;
	if (d[0] >= d[2] || d[1] >= d[3]) { d[0] = x0; d[1] = z0; d[2] = x1; d[3] = z1; }
	else
	{
		d[0] = std::min(d[0], x0); d[1] = std::min(d[1], z0);
		d[2] = std::max(d[2], x1); d[3] = std::max(d[3], z1);
	}
	++landscape->heights_revision_;
	++landscape->revision_;
	if (auto it = ctx_.scenes.find(landscape->scene_); it != ctx_.scenes.end())
	{
		it->second->shadows_dirty = true;
		++it->second->gi_geometry_revision;
	}
}

// Heights from the red channel of the heightmap texture (0..1).
static bool LoadHeightsFromTexture(HRL_Landscape* landscape, HRL_id texture)
{
	std::vector<unsigned char> rgba;
	int width = 0, height = 0;
	if (!g_Backend.RHI_ReadTexturePixels || !g_Backend.RHI_ReadTexturePixels(texture, &rgba, &width, &height) ||
		width < 2 || height < 2 || rgba.size() < size_t(width) * size_t(height) * 4u)
		return false;
	landscape->heights_.resize(size_t(width) * size_t(height));
	for (size_t i = 0; i < landscape->heights_.size(); ++i) landscape->heights_[i] = (float)rgba[i * 4] / 255.f;
	landscape->heights_w_ = (HRL_uint)width;
	landscape->heights_h_ = (HRL_uint)height;
	MarkHeightsChanged(landscape, 0, 0, (HRL_uint)width, (HRL_uint)height);
	return true;
}

static bool ValidHeights(const float* heights, size_t count, const char* fn)
{
	if (!heights)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, std::string(fn) + ": heights must not be null");
		return false;
	}
	for (size_t i = 0; i < count; ++i)
		if (!std::isfinite(heights[i]))
		{
			SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, std::string(fn) + ": heights must be finite");
			return false;
		}
	return true;
}

bool SampleLandscape(const HRL_Landscape* landscape, float worldX, float worldZ, glm::vec3* position, glm::vec3* normal)
{
	if (!landscape || landscape->heights_w_ < 2 || landscape->heights_h_ < 2) return false;
	const glm::mat4 model = CalculateLandscapeModelMatrix(landscape);
	const glm::vec3 local = glm::vec3(glm::inverse(model) * glm::vec4(worldX, landscape->position_.y, worldZ, 1.f));
	const float u = local.x / landscape->size_.x + 0.5f, v = local.z / landscape->size_.y + 0.5f;
	if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return false;
	const HRL_uint w = landscape->heights_w_, h = landscape->heights_h_;
	auto at = [&](int x, int z)
	{
		x = std::clamp(x, 0, (int)w - 1);
		z = std::clamp(z, 0, (int)h - 1);
		return landscape->heights_[size_t(z) * w + size_t(x)];
	};
	const float fx = u * float(w - 1), fz = v * float(h - 1);
	const int x0 = std::min((int)fx, (int)w - 2), z0 = std::min((int)fz, (int)h - 2);
	const float tx = fx - (float)x0, tz = fz - (float)z0;
	const float y = glm::mix(glm::mix(at(x0, z0), at(x0 + 1, z0), tx), glm::mix(at(x0, z0 + 1), at(x0 + 1, z0 + 1), tx), tz);
	if (position) *position = glm::vec3(model * glm::vec4(local.x, y * landscape->height_scale_, local.z, 1.f));
	if (normal)
	{
		// Central differences of the bilinear surface, in local units.
		const float cellX = landscape->size_.x / float(w - 1), cellZ = landscape->size_.y / float(h - 1);
		const int xi = (int)std::round(fx), zi = (int)std::round(fz);
		const float dx = (at(xi + 1, zi) - at(xi - 1, zi)) * landscape->height_scale_ / (2.f * cellX);
		const float dz = (at(xi, zi + 1) - at(xi, zi - 1)) * landscape->height_scale_ / (2.f * cellZ);
		const glm::vec3 n = glm::normalize(glm::vec3(-dx, 1.f, -dz));
		*normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(model))) * n);
	}
	return true;
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
	if (!LoadHeightsFromTexture(landscape, _heightmap) && g_Backend.RHI_RenderSceneFrame)
	{
		// Renderers without the legacy path read the heights on the CPU.
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "HRL_CreateLandscape: the heightmap pixels cannot be read");
		delete landscape;
		return HRL_INVALID_ID;
	}
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
	// Grass grows on its landscape: it goes with it.
	std::vector<HRL_id> grass;
	for (const auto& [gid, g] : ctx_.grass) if (g && g->landscape_ == _landscape) grass.push_back(gid);
	for (HRL_id gid : grass) HRL_DeleteGrass(gid);
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
	LoadHeightsFromTexture(landscape, _heightmap);
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

// ---------------------------------------------------------------------------
// Heights given as floats, height queries, LOD
// ---------------------------------------------------------------------------
HRL_id HRL_CreateLandscapeFromHeights(HRL_id _sceneid, const float* _heights, HRL_uint _width, HRL_uint _depth)
{
	auto sceneIt = ctx_.scenes.find(_sceneid);
	if (sceneIt == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateLandscapeFromHeights: invalid scene ID");
		return HRL_INVALID_ID;
	}
	if (_width < 2 || _depth < 2 || _width > 16384 || _depth > 16384)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_CreateLandscapeFromHeights: width and depth must be in 2..16384");
		return HRL_INVALID_ID;
	}
	if (!ValidHeights(_heights, size_t(_width) * _depth, "HRL_CreateLandscapeFromHeights")) return HRL_INVALID_ID;
	auto* landscape = new (std::nothrow) HRL_Landscape();
	if (!landscape)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateLandscapeFromHeights: allocation failed");
		return HRL_INVALID_ID;
	}
	if (!g_Backend.RHI_RenderSceneFrame)
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING,
			"HRL_CreateLandscapeFromHeights: drawn by the Vulkan backend only (OpenGL needs a heightmap texture)");
	const HRL_id id = GenerateHRL_ID();
	landscape->id_ = id;
	landscape->scene_ = _sceneid;
	landscape->height_scale_ = 1.f; // the heights are local units
	landscape->heights_.assign(_heights, _heights + size_t(_width) * _depth);
	landscape->heights_w_ = _width;
	landscape->heights_h_ = _depth;
	sceneIt->second->landscapes.emplace(id, landscape);
	ctx_.landscapes.emplace(id, landscape);
	MarkHeightsChanged(landscape, 0, 0, _width, _depth);
	return id;
}

void HRL_SetLandscapeHeights(HRL_id _landscape, const float* _heights, HRL_uint _width, HRL_uint _depth)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeHeights");
	if (!landscape) return;
	if (_width < 2 || _depth < 2 || _width > 16384 || _depth > 16384)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeHeights: width and depth must be in 2..16384");
		return;
	}
	if (!ValidHeights(_heights, size_t(_width) * _depth, "HRL_SetLandscapeHeights")) return;
	landscape->heights_.assign(_heights, _heights + size_t(_width) * _depth);
	landscape->heights_w_ = _width;
	landscape->heights_h_ = _depth;
	MarkHeightsChanged(landscape, 0, 0, _width, _depth);
}

void HRL_UpdateLandscapeHeights(HRL_id _landscape, HRL_uint _x, HRL_uint _z, HRL_uint _width, HRL_uint _depth, const float* _heights)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_UpdateLandscapeHeights");
	if (!landscape) return;
	if (_width == 0 || _depth == 0 || _x + _width > landscape->heights_w_ || _z + _depth > landscape->heights_h_)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_UpdateLandscapeHeights: the area is outside the heights");
		return;
	}
	if (!ValidHeights(_heights, size_t(_width) * _depth, "HRL_UpdateLandscapeHeights")) return;
	for (HRL_uint z = 0; z < _depth; ++z)
		std::copy(_heights + size_t(z) * _width, _heights + size_t(z + 1) * _width,
			landscape->heights_.begin() + (std::ptrdiff_t)(size_t(_z + z) * landscape->heights_w_ + _x));
	MarkHeightsChanged(landscape, _x, _z, _x + _width, _z + _depth);
}

void HRL_GetLandscapeHeightsSize(HRL_id _landscape, HRL_uint* _width, HRL_uint* _depth)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_GetLandscapeHeightsSize");
	if (_width) *_width = landscape ? landscape->heights_w_ : 0;
	if (_depth) *_depth = landscape ? landscape->heights_h_ : 0;
}

int HRL_GetLandscapeHeightAt(HRL_id _landscape, float _x, float _z, float* _y)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_GetLandscapeHeightAt");
	glm::vec3 p(0.f);
	if (!landscape || !SampleLandscape(landscape, _x, _z, &p, nullptr)) return HRL_FALSE;
	if (_y) *_y = p.y;
	return HRL_TRUE;
}

int HRL_GetLandscapeNormalAt(HRL_id _landscape, float _x, float _z, float* _nx, float* _ny, float* _nz)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_GetLandscapeNormalAt");
	glm::vec3 n(0.f, 1.f, 0.f);
	if (!landscape || !SampleLandscape(landscape, _x, _z, nullptr, &n)) return HRL_FALSE;
	if (_nx) *_nx = n.x;
	if (_ny) *_ny = n.y;
	if (_nz) *_nz = n.z;
	return HRL_TRUE;
}

void HRL_SetLandscapeLODDistance(HRL_id _landscape, float _distance)
{
	HRL_Landscape* landscape = GetLandscapeForEdit(_landscape, "HRL_SetLandscapeLODDistance");
	if (!landscape) return;
	if (!std::isfinite(_distance) || _distance < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetLandscapeLODDistance: distance must be finite and >= 0");
		return;
	}
	landscape->lod_distance_ = _distance;
}
