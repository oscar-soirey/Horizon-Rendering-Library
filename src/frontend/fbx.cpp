// HRL frontend - FBX import.
#include "frontend_internal.h"

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

glm::vec3 HRLFBX_NormalizeOrFallback(const glm::vec3& value, const glm::vec3& fallback)
{
	const float length2 = glm::dot(value, value);
	if (!std::isfinite(length2) || length2 <= 0.0000001f)
		return fallback;
	return glm::normalize(value);
}

void HRLFBX_BuildFallbackTangentSpace(
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


// ============================================================================
// FBX resource/material helpers
// ============================================================================


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
