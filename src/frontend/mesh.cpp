// HRL frontend - meshes, sprites, LODs.
#include "frontend_internal.h"

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
	++it_scene->second->mesh_revision;
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
	const glm::vec3 pivot(x, y, z);
	// Valeur identique : ne pas invalider les ombres ni la GI (reconstruction CPU couteuse).
	if (it->second->pivot_point_ == pivot)
		return;
	it->second->pivot_point_ = pivot;
	MarkMeshMoved(_meshid, it->second);
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
		++scene_it->second->mesh_revision;
		auto& moved = scene_it->second->moved_meshes;
		moved.erase(std::remove(moved.begin(), moved.end(), _meshid), moved.end());
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
	if (it->second->material_ == _matid)
		return;
	it->second->material_ = _matid;
	MarkSceneMeshesChanged(it->second->scene_);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ == HRL_3D_MESH)
		MarkSceneGILightingDirty(scene_it->second);
}

void HRL_SetMeshUserHandle(HRL_id _meshid, void* _handle)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshUserHandle: invalid ID");
		return;
	}

	it->second->user_handle_ = _handle;
}

void* HRL_GetMeshUserHandle(HRL_id _meshid)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshUserHandle: invalid ID");
		return nullptr;
	}

	return it->second->user_handle_;
}

void HRL_SetMeshLocation(HRL_id _meshid, float x, float y, float z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetMeshLocation: invalid ID");
		return;
	}
	const glm::vec3 position(x, y, z);
	// Beaucoup d'applications reecrivent la position a chaque frame : sans ce
	// test, chaque appel relancait toutes les shadow maps et la reconstruction
	// CPU du BVH de la GI, meme pour un objet immobile.
	if (it->second->position_ == position)
		return;
	it->second->position_ = position;
	MarkMeshMoved(_meshid, it->second);
	auto scene_it = ctx_.scenes.find(it->second->scene_);
	if (scene_it != ctx_.scenes.end() && it->second->type_ != HRL_SPRITE)
		MarkSceneGIGeometryDirty(scene_it->second);
}

void HRL_GetMeshLocation(HRL_id _meshid, float* x, float* y, float* z)
{
	auto it = ctx_.meshes.find(_meshid);
	if (it == ctx_.meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GetMeshLocation: invalid ID");
		return;
	}

	if (x) *x = it->second->position_.x;
	if (y) *y = it->second->position_.y;
	if (z) *z = it->second->position_.z;
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
	const glm::vec3 rotation(pitch, yaw, roll);
	if (it->second->rotation_ == rotation)
		return;
	it->second->rotation_ = rotation;
	MarkMeshMoved(_meshid, it->second);
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
	const glm::vec3 scale(x, y, z);
	if (it->second->scale_ == scale)
		return;
	it->second->scale_ = scale;
	MarkMeshMoved(_meshid, it->second);
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


static bool RebuildLODs(HRL_Mesh* mesh);

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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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

	++mesh->lod_revision_;
	MarkSceneMeshesChanged(mesh->scene_);
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
	++it_scene->second->mesh_revision;
	MarkSceneGIGeometryDirty(it_scene->second);
	ctx_.meshes.emplace(newId, mesh);

	return newId;
}


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


bool GenerateMeshLODs(HRL_Mesh* mesh, HRL_uint levels)
{
	if (!mesh) return false;
	mesh->lod_levels_ = std::max<HRL_uint>(1u, levels);
	return RebuildLODs(mesh);
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
