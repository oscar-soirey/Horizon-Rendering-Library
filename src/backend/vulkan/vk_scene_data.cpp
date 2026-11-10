// HRL native Vulkan backend - scene data: what the shaders read for a scene
// (instances, materials, lights) and the cameras / shadow cascades.
#include "vk_renderer_internal.h"

namespace hrlvk
{
float MaterialFloat(const HRL_Material* m, const char* name, float fallback)
{
	auto it = m->floatParams_.find(name);
	return it == m->floatParams_.end() ? fallback : it->second;
}
namespace
{
bool IntParam(const HRL_Material* m, const char* name)
{
	auto it = m->intParams_.find(name);
	return it != m->intParams_.end() && it->second != 0;
}

MaterialData PackMaterial(const HRL_Material* m, Bucket& bucket)
{
	MaterialData d{};
	glm::vec3 tint(1.f);
	if (auto it = m->vec3Params_.find("TintColor"); it != m->vec3Params_.end()) tint = it->second;
	d.baseColor = glm::vec4(glm::max(tint, glm::vec3(0.f)), MaterialFloat(m, "BaseColorAlpha", 1.f));
	// Emissive = colour * intensity * emissive texture. A texture alone emits at full strength.
	if (auto it = m->vec4Params_.find("EmissiveColor"); it != m->vec4Params_.end())
		d.emissive = glm::vec4(glm::vec3(it->second) * it->second.a, 0.f);
	else if (m->textureParams_.count(HRL_T_EMISSIVE))
		d.emissive = glm::vec4(1.f, 1.f, 1.f, 0.f);
	d.params = glm::vec4(MaterialFloat(m, "RoughnessValue", 1.f), MaterialFloat(m, "MetallicValue", 0.f),
		MaterialFloat(m, "SpecularValue", 1.f), MaterialFloat(m, "OpacityValue", 1.f));
	const uint32_t white = R.white.slot, black = R.black.slot, normal = R.flatNormal.slot;
	// Fallbacks reproduce the built-in OpenGL defaults: rough dielectric, no AO.
	d.tex0 = glm::uvec4(TextureSlot(m, HRL_T_ALBEDO, white), TextureSlot(m, HRL_T_NORMAL, normal),
		TextureSlot(m, HRL_T_ROUGHNESS, white), TextureSlot(m, HRL_T_METALLIC, black));
	d.tex1 = glm::uvec4(TextureSlot(m, HRL_T_AMBIENT_OCCLUSION, white), TextureSlot(m, HRL_T_ALPHA, white),
		TextureSlot(m, HRL_T_SPECULAR, white), TextureSlot(m, HRL_T_EMISSIVE, white));
	uint32_t flags = 0;
	if (IntParam(m, "RoughnessUseValue")) flags |= MatRoughnessValue;
	if (IntParam(m, "MetallicUseValue")) flags |= MatMetallicValue;
	if (IntParam(m, "SpecularUseValue")) flags |= MatSpecularValue;
	if (IntParam(m, "OpacityUseValue")) flags |= MatOpacityValue;
	if (IntParam(m, "RoughnessInvert")) flags |= MatRoughnessInvert;
	if (IntParam(m, "AlphaInvert")) flags |= MatAlphaInvert;
	d.flags = glm::uvec4(flags, 0, 0, 0);
	const bool masked = (flags & MatOpacityValue) || m->textureParams_.count(HRL_T_ALPHA) != 0;
	bucket = m->two_sided_ ? (masked ? BucketMaskedTwoSided : BucketOpaqueTwoSided) : (masked ? BucketMasked : BucketOpaque);
	return d;
}

// ---------------------------------------------------------------------------
// Cached instances and materials
//
// Rebuilding everything each frame costs a model matrix, a 3x3 inverse and
// about twenty string-keyed lookups per material. Instead the data is kept
// and compared with its sources:
//   * the list of meshes (ids, materials) : a mismatch rebuilds everything,
//   * the transform of each mesh          : compared by value, so a transform
//                                           written by any path is seen,
//   * HRL_Material::revision_             : bumped by the material setters,
//   * the renderer mesh / texture tables  : revision counters.
// ---------------------------------------------------------------------------
void WriteInstance(SceneData& data, uint32_t index)
{
	DrawItem& item = data.items[index];
	item.model = CalculateModelMatrix(item.mesh);
	InstanceData& inst = data.instances[index];
	inst.model = item.model;
	const glm::mat3 m3(item.model);
	const float det = glm::determinant(m3);
	inst.normalMatrix = (std::isfinite(det) && std::abs(det) > 1e-12f)
		? glm::mat4(glm::transpose(glm::inverse(m3))) : glm::mat4(1.f);
}

HRL_id LastId(const HRL_IdMap<HRL_Material*>& materials)
{
	return materials.empty() ? HRL_INVALID_ID : (materials.end() - 1)->first;
}

void RebuildSceneData(SceneData& data, const HRL_SceneFrame& frame)
{
	data.keys.clear();
	data.items.clear();
	data.instances.clear();
	data.materials.clear();
	data.materialBuckets.clear();
	data.materialKeys.clear();
	std::unordered_map<HRL_id, int> materialIndex; // -1 : unknown material
	auto resolveMaterial = [&](HRL_id id) -> int
	{
		auto found = materialIndex.find(id);
		if (found != materialIndex.end()) return found->second;
		int index = -1;
		auto it = frame.materials->find(id);
		if (it != frame.materials->end() && it->second)
		{
			Bucket bucket = BucketOpaque;
			data.materials.push_back(PackMaterial(it->second, bucket));
			data.materialBuckets.push_back(bucket);
			data.materialKeys.push_back({it->second, it->second->revision_});
			index = (int)data.materials.size() - 1;
		}
		materialIndex.emplace(id, index);
		return index;
	};

	for (const auto& [id, mesh] : frame.scene->meshes)
	{
		if (!mesh || mesh->type_ != HRL_3D_MESH) continue;
		SceneData::MeshKey key;
		key.id = id;
		key.mesh = mesh;
		key.material = mesh->material_;
		key.radius = mesh->bounds_radius_;
		key.position = mesh->position_;
		key.rotation = mesh->rotation_;
		key.scale = mesh->scale_;
		key.pivot = mesh->pivot_point_;
		auto gpuMesh = R.meshes.find(id);
		const bool drawable = IsFiniteBounds(mesh) && gpuMesh != R.meshes.end() &&
			!gpuMesh->second.lods.empty() && gpuMesh->second.lods[0].valid;
		const int material = drawable ? resolveMaterial(mesh->material_) : -1;
		if (material >= 0)
		{
			key.item = (int32_t)data.items.size();
			DrawItem item;
			item.mesh = mesh;
			item.gpu = &gpuMesh->second;
			item.instance = (uint32_t)data.items.size();
			item.material = (uint32_t)material;
			data.items.push_back(item);
			InstanceData inst{};
			inst.ids = glm::uvec4((uint32_t)material, id, 0, 0);
			data.instances.push_back(inst);
			WriteInstance(data, item.instance);
		}
		data.keys.push_back(key);
	}
	data.built = true;
	data.meshTableRevision = R.meshTableRevision;
	data.textureRevision = R.textureRevision;
	data.materialCount = frame.materials->size();
	data.lastMaterialId = LastId(*frame.materials);
	data.rebuilds = 1;
}
} // namespace

void UpdateSceneData(SceneData& data, const HRL_SceneFrame& frame)
{
	data.rebuilds = data.transformUpdates = data.materialUpdates = 0;
	// Creating or deleting a material changes (count, last id): ids only grow.
	bool valid = data.built && data.meshTableRevision == R.meshTableRevision &&
		data.materialCount == frame.materials->size() && data.lastMaterialId == LastId(*frame.materials);
	if (valid)
	{
		size_t k = 0;
		for (const auto& [id, mesh] : frame.scene->meshes)
		{
			if (!mesh || mesh->type_ != HRL_3D_MESH) continue;
			if (k >= data.keys.size()) { valid = false; break; }
			SceneData::MeshKey& key = data.keys[k++];
			if (key.id != id || key.mesh != mesh || key.material != mesh->material_ || key.radius != mesh->bounds_radius_)
			{
				valid = false;
				break;
			}
			if (key.item >= 0 && (key.position != mesh->position_ || key.rotation != mesh->rotation_ ||
				key.scale != mesh->scale_ || key.pivot != mesh->pivot_point_))
			{
				key.position = mesh->position_;
				key.rotation = mesh->rotation_;
				key.scale = mesh->scale_;
				key.pivot = mesh->pivot_point_;
				WriteInstance(data, (uint32_t)key.item);
				++data.transformUpdates;
			}
		}
		if (k != data.keys.size()) valid = false;
	}
	if (!valid)
	{
		RebuildSceneData(data, frame);
		return;
	}

	const bool texturesChanged = data.textureRevision != R.textureRevision;
	for (size_t i = 0; i < data.materialKeys.size(); ++i)
	{
		SceneData::MaterialKey& key = data.materialKeys[i];
		if (!texturesChanged && key.revision == key.material->revision_) continue;
		data.materials[i] = PackMaterial(key.material, data.materialBuckets[i]);
		key.revision = key.material->revision_;
		++data.materialUpdates;
	}
	data.textureRevision = R.textureRevision;
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------
LightSetup PackLights(const hrl_scene_t* scene, bool rayTracedShadows)
{
	LightSetup out;
	for (const auto& [id, light] : scene->lights)
	{
		if (!light || light->intensity_ <= 0.f) continue;
		LightData d{};
		int type = 0;
		switch (light->type_)
		{
		case HRL_DIRECTIONAL_LIGHT: type = 1; break;
		case HRL_SPOT_LIGHT: type = 2; break;
		case HRL_SKY_LIGHT: type = 3; break;
		default: type = 0; break;
		}
		d.position = glm::vec4(light->position_, (float)type);
		d.direction = glm::vec4(GetLightDirection(light), light->attenuation_);
		// Shadow slot: -1 none, 0 cascades, 1.. spot shadow maps, 9 ray traced.
		float slot = -1.f;
		if (light->cast_shadows_ && type != 3)
		{
			if (rayTracedShadows) slot = 9.f;
			else if (type == 1 && !out.cascadeLight) { out.cascadeLight = light; slot = 0.f; }
			else if (type == 2 && out.spotShadowLights.size() < kMaxSpotShadows)
			{
				out.spotShadowLights.push_back(light);
				slot = (float)out.spotShadowLights.size();
			}
			if (slot >= 0.f && slot < 8.f)
				out.shadowResolution = std::max(out.shadowResolution, (uint32_t)glm::clamp(light->shadow_resolution_, 256, 8192));
		}
		d.color = glm::vec4(light->color_ * light->intensity_, slot);
		d.spot = glm::vec4(std::cos(glm::radians(light->innerCutoff)), std::cos(glm::radians(light->outerCutoff)),
			light->shadow_strength_, light->shadow_bias_ * 0.25f);
		out.lights.push_back(d);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Cameras and shadow cascades
// ---------------------------------------------------------------------------
Camera MakeCamera(const HRL_Camera* c, float width, float height)
{
	Camera cam;
	cam.position = c->position_;
	cam.view = glm::lookAt(c->position_, c->position_ + GetForwardVector(c->rotation_), GetUpVector(c->rotation_));
	cam.nearPlane = std::max(c->near_plane_, 1e-4f);
	cam.farPlane = std::max(c->far_plane_, cam.nearPlane + 1e-3f);
	cam.aspect = width / std::max(height, 1.f);
	cam.perspective = c->type_ == HRL_PERSPECTIVE;
	if (cam.perspective)
	{
		cam.fovY = glm::radians(glm::clamp(c->value_, 1.f, 179.f));
		cam.proj = glm::perspectiveRH_ZO(cam.fovY, cam.aspect, cam.farPlane, cam.nearPlane);
		cam.projGL = glm::perspective(cam.fovY, cam.aspect, cam.nearPlane, cam.farPlane);
	}
	else
	{
		cam.orthoHalfHeight = c->value_ * 0.5f;
		const float hw = cam.orthoHalfHeight * cam.aspect;
		cam.proj = glm::orthoRH_ZO(-hw, hw, -cam.orthoHalfHeight, cam.orthoHalfHeight, cam.farPlane, cam.nearPlane);
		cam.projGL = glm::ortho(-hw, hw, -cam.orthoHalfHeight, cam.orthoHalfHeight, cam.nearPlane, cam.farPlane);
	}
	return cam;
}

// Stable cascades: each cascade is the bounding sphere of its frustum slice,
// snapped to shadow map texels so the shadows do not shimmer when the camera moves.
void ComputeCascades(const Camera& cam, const glm::vec3& lightDir, float resolution,
	ViewData& view, std::vector<ShadowView>& out)
{
	const float n = cam.nearPlane;
	const float f = std::min(cam.farPlane, std::max(R.shadowDistance, n + 1.f));
	const glm::mat4 invView = glm::inverse(cam.view);
	const glm::vec3 up = std::abs(lightDir.y) > 0.98f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
	const float extrude = 250.f; // casters between the light and the slice
	float previous = n;
	for (uint32_t i = 0; i < kCascades; ++i)
	{
		const float p = float(i + 1) / float(kCascades);
		const float split = glm::mix(n + (f - n) * p, n * std::pow(f / n, p), 0.8f);
		glm::vec3 corners[8];
		int k = 0;
		for (float d : {previous, split})
		{
			const float hh = cam.perspective ? std::tan(cam.fovY * 0.5f) * d : cam.orthoHalfHeight;
			const float hw = hh * cam.aspect;
			for (float sx : {-1.f, 1.f})
				for (float sy : {-1.f, 1.f})
					corners[k++] = glm::vec3(invView * glm::vec4(sx * hw, sy * hh, -d, 1.f));
		}
		glm::vec3 center(0.f);
		for (const auto& c : corners) center += c;
		center /= 8.f;
		float radius = 0.f;
		for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
		radius = std::ceil(radius * 16.f) / 16.f;

		const glm::mat4 lightView = glm::lookAt(center - lightDir * (radius + extrude), center, up);
		glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.f, 2.f * radius + extrude);
		// Texel snapping.
		glm::vec4 origin = proj * lightView * glm::vec4(0.f, 0.f, 0.f, 1.f);
		origin *= resolution * 0.5f;
		const glm::vec2 offset = (glm::round(glm::vec2(origin)) - glm::vec2(origin)) * (2.f / resolution);
		proj[3][0] += offset.x;
		proj[3][1] += offset.y;

		view.cascadeViewProj[i] = proj * lightView;
		view.cascadeSplits[i] = split;
		ShadowView sv;
		sv.viewProj = view.cascadeViewProj[i];
		sv.cullClip = glm::ortho(-radius, radius, -radius, radius, 0.f, 2.f * radius + extrude) * lightView;
		sv.layer = i;
		out.push_back(sv);
		previous = split;
	}
}
} // namespace hrlvk
