// CPU-side rendering helpers shared by the OpenGL 3.3 and Vulkan backends.
// Everything here is pure math/CPU work so both backends make exactly the
// same decisions (culling, LOD, light matrices, overlay geometry, ...).
#include "render_common.h"
#include "utils_functions.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>




glm::vec4 NormalizePlane(const glm::vec4& p)
{
	const float len = glm::length(glm::vec3(p));
	return (len > 1e-6f) ? (p / len) : p;
}



FrustumPlaneSet BuildFrustum(const glm::mat4& clip)
{
	// GLM matrices are column-major. Extract the rows explicitly from the
	// combined clip matrix so the plane equations are independent of layout.
	const glm::vec4 r0(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
	const glm::vec4 r1(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
	const glm::vec4 r2(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
	const glm::vec4 r3(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);

	FrustumPlaneSet f{};
	f.planes[0] = NormalizePlane(r3 + r0); // left
	f.planes[1] = NormalizePlane(r3 - r0); // right
	f.planes[2] = NormalizePlane(r3 + r1); // bottom
	f.planes[3] = NormalizePlane(r3 - r1); // top
	f.planes[4] = NormalizePlane(r3 + r2); // near
	f.planes[5] = NormalizePlane(r3 - r2); // far
	return f;
}



bool SphereInsideFrustum(const FrustumPlaneSet& frustum, const glm::vec3& center, float radius)
{
	for (const glm::vec4& p : frustum.planes)
	{
		if (glm::dot(glm::vec3(p), center) + p.w < -radius)
			return false;
	}
	return true;
}



bool IsMeshVisible(const HRL_Mesh* mesh, const glm::mat4& model, const FrustumPlaneSet& frustum)
{
	if (!mesh || mesh->bounds_radius_ <= 0.f)
		return true;
	const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
	const float maxScale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
	const float radius = mesh->bounds_radius_ * std::max(maxScale, 1e-6f);
	return SphereInsideFrustum(frustum, center, radius);
}



bool IsFiniteBounds(const HRL_Mesh* mesh)
{
	return mesh && std::isfinite(mesh->bounds_center_.x) && std::isfinite(mesh->bounds_center_.y) &&
		std::isfinite(mesh->bounds_center_.z) && std::isfinite(mesh->bounds_radius_);
}



glm::mat4 CalculateModelMatrix(const HRL_Mesh* mesh)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, mesh->position_);
	model = glm::translate(model, mesh->pivot_point_);
	model = glm::rotate(model, glm::radians(mesh->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(mesh->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::translate(model, -mesh->pivot_point_);
	model = glm::scale(model, mesh->scale_);
	// Skeletal skinning matrices are already expressed in FBX world space.
	// Do not append the FBX geometry_to_world transform here: that would apply
	// the import transform a second time. The model matrix therefore contains
	// only HRL's user-controlled object transform.
	return model;
}



int SelectMeshLOD(const HRL_Mesh* mesh, const glm::mat4& model,
    const glm::mat4& view, const glm::mat4& projection, float viewportHeight, const HRL_Viewport* viewport)
{
	if (!mesh || !mesh->lod_automatic_ || mesh->lods_.size() <= 1)
		return 0;

	const int maxLevel = (int)mesh->lods_.size() - 1;
	if (mesh->lod_override_ >= 0)
		return std::clamp(mesh->lod_override_, 0, maxLevel);

	float metric = 0.0f;
	float transition0 = 0.0f;

	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
	{
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
		glm::vec3 cameraPos(0.0f);
		if (viewport && viewport->camera_)
			cameraPos = viewport->camera_->position_;
		const float distance = glm::length(center - cameraPos);
		metric = distance;
		transition0 = mesh->lod_base_distance_;
	}
	else
	{
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.0f));
		const glm::vec4 viewCenter = view * glm::vec4(center, 1.0f);
		const float z = std::max(std::abs(viewCenter.z), 1e-4f);
		const float objectScale = std::max({
			std::abs(mesh->scale_.x),
			std::abs(mesh->scale_.y),
			std::abs(mesh->scale_.z)
		});
		const float radius = mesh->bounds_radius_ * std::max(objectScale, 1e-6f);
		const bool perspective = viewport && viewport->camera_ &&
			viewport->camera_->type_ == HRL_PERSPECTIVE;
		metric = perspective
			? std::abs(projection[1][1]) * radius / z
			: std::abs(projection[1][1]) * radius;
		(void)viewportHeight;
		transition0 = mesh->lod_screen_threshold_;
	}

	int desired = maxLevel;
	float threshold = transition0;
	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
	{
		if (metric <= mesh->lod_min_distance_)
			desired = 0;
		else if (metric >= mesh->lod_max_distance_)
			desired = maxLevel;
		else
		{
			for (int level = 1; level <= maxLevel; ++level)
			{
				if (metric < threshold)
				{
					desired = level - 1;
					break;
				}
				threshold *= mesh->lod_distance_scale_;
			}
		}
	}
	else
	{
		for (int level = 1; level <= maxLevel; ++level)
		{
			if (metric >= threshold)
			{
				desired = level - 1;
				break;
			}
			threshold *= mesh->lod_screen_scale_;
		}
	}

	// Hysteresis keeps the selected LOD from oscillating when the camera hovers
	// around a transition boundary.
	const int current = std::clamp(mesh->last_lod_level_, 0, maxLevel);
	if (mesh->lod_hysteresis_ <= 0.0f || desired == current)
		return desired;

	if (desired > current)
	{
		// Moving toward a cheaper LOD.
		float boundary;
		if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
			boundary = mesh->lod_base_distance_ * std::pow(mesh->lod_distance_scale_, (float)current);
		else
			boundary = mesh->lod_screen_threshold_ * std::pow(mesh->lod_screen_scale_, (float)current);

		const float switchPoint = mesh->lod_mode_ == HRL_LOD_DISTANCE
			? boundary * (1.0f + mesh->lod_hysteresis_)
			: boundary * (1.0f - mesh->lod_hysteresis_);
		const bool crossed = mesh->lod_mode_ == HRL_LOD_DISTANCE
			? metric >= switchPoint
			: metric <= switchPoint;
		return crossed ? desired : current;
	}

	// Moving toward a more detailed LOD.
	float boundary;
	if (mesh->lod_mode_ == HRL_LOD_DISTANCE)
		boundary = mesh->lod_base_distance_ * std::pow(mesh->lod_distance_scale_, (float)(desired));
	else
		boundary = mesh->lod_screen_threshold_ * std::pow(mesh->lod_screen_scale_, (float)(desired));
	const float switchPoint = mesh->lod_mode_ == HRL_LOD_DISTANCE
		? boundary * (1.0f - mesh->lod_hysteresis_)
		: boundary * (1.0f + mesh->lod_hysteresis_);
	const bool crossed = mesh->lod_mode_ == HRL_LOD_DISTANCE
		? metric <= switchPoint
		: metric >= switchPoint;
	return crossed ? desired : current;
}



glm::mat4 MakeVFXSystemMatrix(const HRL_VFXSystem* system)
{
	glm::mat4 m(1.f);
	m = glm::translate(m, system->position_);
	m = glm::rotate(m, glm::radians(system->rotation_.x), glm::vec3(1.f,0.f,0.f));
	m = glm::rotate(m, glm::radians(system->rotation_.y), glm::vec3(0.f,1.f,0.f));
	m = glm::rotate(m, glm::radians(system->rotation_.z), glm::vec3(0.f,0.f,1.f));
	m = glm::scale(m, system->scale_);
	return m;
}



// systemMatrix == nullptr : espace monde (pas de transformation). Le test
// "matrice == identite" est fait une fois par emetteur et non par particule.
glm::mat4 MakeVFXParticleModel(const HRL_VFXParticle& p, const glm::mat4* systemMatrix,
	const glm::mat4& billboardBasis, bool stretched, float stretch)
{
	glm::vec3 worldPos = p.position;
	if (systemMatrix)
		worldPos = glm::vec3(*systemMatrix * glm::vec4(worldPos, 1.f));

	glm::mat4 model = glm::translate(glm::mat4(1.f), worldPos);
	glm::mat4 orient = billboardBasis;
	if (stretched)
	{
		const glm::vec3 viewRight = glm::normalize(glm::vec3(billboardBasis[0]));
		const glm::vec3 viewUp = glm::normalize(glm::vec3(billboardBasis[1]));
		const glm::vec3 v = p.velocity;
		const float projectedLength = glm::length(v - glm::vec3(billboardBasis[2]) * glm::dot(v, glm::vec3(billboardBasis[2])));
		if (projectedLength > 1e-4f)
		{
			const glm::vec3 pv = glm::normalize(v - glm::vec3(billboardBasis[2]) * glm::dot(v, glm::vec3(billboardBasis[2])));
			const float angle = std::atan2(glm::dot(pv, viewUp), glm::dot(pv, viewRight));
			orient = glm::rotate(orient, angle, glm::vec3(billboardBasis[2]));
		}
	}
	else
	{
		orient = glm::rotate(orient, glm::radians(p.rotation.z), glm::vec3(billboardBasis[2]));
	}
	model *= orient;
	float sx = std::max(0.f, p.size.x);
	float sy = std::max(0.f, p.size.y);
	if (stretched) sx += glm::length(p.velocity) * std::max(0.f, stretch);
	model = glm::scale(model, glm::vec3(sx, sy, 1.f));
	return model;
}



glm::vec3 GizmoAxisRenderer(const HRL_Gizmo* g, int axis)
{
    glm::vec3 v(0.f); v[axis] = 1.f;
    if (g->space_ == HRL_GIZMO_SPACE_WORLD) return v;
    glm::mat4 r(1.f);
    r = glm::rotate(r, glm::radians(g->rotation_.x), glm::vec3(1,0,0));
    r = glm::rotate(r, glm::radians(g->rotation_.y), glm::vec3(0,1,0));
    r = glm::rotate(r, glm::radians(g->rotation_.z), glm::vec3(0,0,1));
    return glm::normalize(glm::vec3(r * glm::vec4(v,0.f)));
}



float GizmoSizeRenderer(const HRL_Gizmo* g, const HRL_Viewport* viewport, const glm::mat4& view)
{
    if (!g || !viewport || !viewport->camera_) return 1.f;
    if (!g->use_screen_size_) return std::max(0.0001f, g->world_size_);
    const float winH = std::max(1.f, static_cast<float>(GetWindowHeight()) * viewport->height_);
    const float depth = std::max(0.001f, std::abs(glm::vec3(view * glm::vec4(g->position_,1.f)).z));
    if (viewport->camera_->type_ == HRL_PERSPECTIVE) {
        const float worldHeight = 2.f * depth * std::tan(glm::radians(viewport->camera_->value_) * 0.5f);
        return std::max(0.0001f, g->screen_size_pixels_ * worldHeight / winH);
    }
    return std::max(0.0001f, g->screen_size_pixels_ * viewport->camera_->value_ / winH);
}



void GizmoPushLine(DebugRenderer& d, const glm::vec3& a, const glm::vec3& b, const glm::vec4& c)
{
    d.lines.emplace_back(a.x,a.y,a.z,c.x,c.y,c.z);
    d.lines.emplace_back(b.x,b.y,b.z,c.x,c.y,c.z);
}



void GizmoPushArc(DebugRenderer& d, const glm::vec3& center, const glm::vec3& normal,
    const glm::vec3& u, float radius, float start, float sweep, int segments, const glm::vec4& color)
{
    glm::vec3 v = glm::normalize(glm::cross(normal, u));
    sweep = glm::clamp(sweep, 0.17453292519943295769f, 2.9670597283903604f);
    segments = std::max(6, segments);
    glm::vec3 prev = center + (std::cos(start)*u + std::sin(start)*v) * radius;
    for (int i=1; i<=segments; ++i) {
        const float a = start + sweep * float(i) / float(segments);
        glm::vec3 cur = center + (std::cos(a)*u + std::sin(a)*v) * radius;
        GizmoPushLine(d, prev, cur, color);
        prev = cur;
    }
}



const char* MeshDebugTypeName(const HRL_Mesh* mesh)
{
	if (!mesh) return "Unknown";
	switch (mesh->type_)
	{
	case HRL_3D_MESH: return "Mesh3D";
	case HRL_3D_SKELETAL_MESH: return "SkeletalMesh";
	case HRL_2D_MESH: return "Mesh2D";
	case HRL_SPRITE: return "Sprite";
	default: return "Unknown";
	}
}



glm::vec3 ComputeMeshAverageNormal(const HRL_Mesh* mesh, int lodLevel)
{
	glm::vec3 sum(0.0f);
	size_t count = 0;
	if (!mesh) return glm::vec3(0.0f, 1.0f, 0.0f);
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		for (const auto& v : skinned->vertices_)
		{
			const glm::vec3 n(v.vertex.normal[0], v.vertex.normal[1], v.vertex.normal[2]);
			if (glm::dot(n, n) > 1e-10f && std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z))
			{
				sum += glm::normalize(n);
				++count;
			}
		}
	}
	else if (!mesh->lods_.empty())
	{
		const int level = std::clamp(lodLevel, 0, static_cast<int>(mesh->lods_.size()) - 1);
		for (const auto& v : mesh->lods_[(size_t)level].vertices)
		{
			const glm::vec3 n(v.normal[0], v.normal[1], v.normal[2]);
			if (glm::dot(n, n) > 1e-10f && std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z))
			{
				sum += glm::normalize(n);
				++count;
			}
		}
	}
	if (count == 0 || glm::dot(sum, sum) <= 1e-12f)
		return glm::vec3(0.0f, 1.0f, 0.0f);
	return glm::normalize(sum / static_cast<float>(count));
}



void BuildMeshDebugInfoText(const HRL_Mesh* mesh, std::string& out)
{
	out.clear();
	if (!mesh) return;
	const int lodLevel = std::max(0, mesh->last_lod_level_);
	size_t vertexCount = 0;
	size_t triangleCount = mesh->triangle_count_;
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		vertexCount = skinned->vertices_.size();
		triangleCount = !skinned->indices_.empty() ? skinned->indices_.size() / 3u : skinned->vertices_.size() / 3u;
	}
	else if (!mesh->lods_.empty())
	{
		const size_t level = std::min<size_t>((size_t)lodLevel, mesh->lods_.size() - 1);
		const auto& lod = mesh->lods_[level];
		vertexCount = lod.vertices.size();
		triangleCount = !lod.indices.empty() ? lod.indices.size() / 3u : lod.vertices.size() / 3u;
	}
	const glm::vec3 n = ComputeMeshAverageNormal(mesh, lodLevel);
	std::ostringstream ss;
	ss << MeshDebugTypeName(mesh) << "\n"
	   << "Triangles: " << triangleCount << "\n"
	   << "Vertices: " << vertexCount << "\n"
	   << "LOD: " << lodLevel << "\n"
	   << std::fixed << std::setprecision(2)
	   << "Normal: (" << n.x << ", " << n.y << ", " << n.z << ")\n"
	   << "Position: (" << mesh->position_.x << ", " << mesh->position_.y << ", " << mesh->position_.z << ")\n"
	   << "Scale: (" << mesh->scale_.x << ", " << mesh->scale_.y << ", " << mesh->scale_.z << ")\n"
	   << "Material: " << static_cast<unsigned long long>(mesh->material_);
	if (mesh->type_ == HRL_3D_SKELETAL_MESH)
	{
		const auto* skinned = static_cast<const HRL_SkeletalMesh*>(mesh);
		ss << "\nBones: " << skinned->bones_.size()
		   << "\nAnimation: " << skinned->current_animation_;
	}
	out = ss.str();
}



static uint32_t PNG_CRC32(const unsigned char* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}



static uint32_t PNG_Adler32(const std::vector<unsigned char>& data)
{
    uint32_t a = 1u, b = 0u;
    for (unsigned char c : data)
    {
        a += c;
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}



static void PNG_AppendU32BE(std::vector<unsigned char>& out, uint32_t value)
{
    out.push_back((unsigned char)((value >> 24) & 0xFFu));
    out.push_back((unsigned char)((value >> 16) & 0xFFu));
    out.push_back((unsigned char)((value >> 8) & 0xFFu));
    out.push_back((unsigned char)(value & 0xFFu));
}



static void PNG_AppendChunk(std::vector<unsigned char>& png, const char type[4], const std::vector<unsigned char>& payload)
{
    PNG_AppendU32BE(png, (uint32_t)payload.size());
    size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    uint32_t crc = PNG_CRC32(png.data() + typeOffset, 4 + payload.size());
    PNG_AppendU32BE(png, crc);
}



bool WritePNG_RGBA8(const char* path, int width, int height, const std::vector<unsigned char>& rgbaTopDown)
{
    if (!path || width <= 0 || height <= 0 || rgbaTopDown.size() != (size_t)width * (size_t)height * 4u)
        return false;

    std::vector<unsigned char> raw;
    raw.reserve((size_t)height * ((size_t)width * 4u + 1u));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0);
        const unsigned char* row = rgbaTopDown.data() + (size_t)y * (size_t)width * 4u;
        raw.insert(raw.end(), row, row + (size_t)width * 4u);
    }

    std::vector<unsigned char> zlib;
    zlib.push_back(0x78); zlib.push_back(0x01);
    size_t offset = 0;
    while (offset < raw.size())
    {
        const size_t remaining = raw.size() - offset;
        const uint16_t blockLen = (uint16_t)std::min<size_t>(remaining, 65535u);
        const bool finalBlock = (offset + blockLen == raw.size());
        zlib.push_back(finalBlock ? 0x01 : 0x00);
        zlib.push_back((unsigned char)(blockLen & 0xFFu));
        zlib.push_back((unsigned char)((blockLen >> 8) & 0xFFu));
        const uint16_t nlen = (uint16_t)~blockLen;
        zlib.push_back((unsigned char)(nlen & 0xFFu));
        zlib.push_back((unsigned char)((nlen >> 8) & 0xFFu));
        zlib.insert(zlib.end(), raw.begin() + (ptrdiff_t)offset, raw.begin() + (ptrdiff_t)(offset + blockLen));
        offset += blockLen;
    }
    PNG_AppendU32BE(zlib, PNG_Adler32(raw));

    std::vector<unsigned char> png = {0x89, 'P','N','G',0x0D,0x0A,0x1A,0x0A};
    std::vector<unsigned char> ihdr;
    PNG_AppendU32BE(ihdr, (uint32_t)width);
    PNG_AppendU32BE(ihdr, (uint32_t)height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PNG_AppendChunk(png, "IHDR", ihdr);
    PNG_AppendChunk(png, "IDAT", zlib);
    PNG_AppendChunk(png, "IEND", {});

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write((const char*)png.data(), (std::streamsize)png.size());
    return (bool)file;
}



//SHADOWS / ANTI-ALIASING
glm::vec3 GetLightDirection(const HRL_Light* light)
{
	const float pitch = glm::radians(light->rotation_.x);
	const float yaw = glm::radians(light->rotation_.y);
	glm::vec3 dir(
		std::cos(yaw) * std::cos(pitch),
		std::sin(pitch),
		std::sin(yaw) * std::cos(pitch));
	if (glm::length(dir) < 1e-5f)
		return glm::vec3(0.f, -1.f, 0.f);
	return glm::normalize(dir);
}



glm::mat4 CalculateLandscapeModelMatrix(const HRL_Landscape* landscape)
{
	glm::mat4 model(1.f);
	model = glm::translate(model, landscape->position_);
	model = glm::rotate(model, glm::radians(landscape->rotation_.x), glm::vec3(1.f, 0.f, 0.f));
	model = glm::rotate(model, glm::radians(landscape->rotation_.y), glm::vec3(0.f, 1.f, 0.f));
	model = glm::rotate(model, glm::radians(landscape->rotation_.z), glm::vec3(0.f, 0.f, 1.f));
	model = glm::scale(model, landscape->scale_);
	return model;
}



float SampleLandscapeHeight(const HRL_LandscapeHeightmap* texture, float u, float v)
{
	if (!texture || !texture->rgba || texture->width == 0 || texture->height == 0)
		return 0.f;
	const auto& pixels = *texture->rgba;
	const int width = texture->width;
	const int height = texture->height;
	if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
		return 0.f;

	u = glm::clamp(u, 0.f, 1.f);
	v = glm::clamp(v, 0.f, 1.f);
	const float fx = u * static_cast<float>(std::max(1, width - 1));
	const float fy = v * static_cast<float>(std::max(1, height - 1));
	const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, width - 1);
	const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, height - 1);
	const int x1 = std::min(x0 + 1, width - 1);
	const int y1 = std::min(y0 + 1, height - 1);
	const float tx = fx - static_cast<float>(x0);
	const float ty = fy - static_cast<float>(y0);

	auto read = [&](int x, int y) -> float {
		const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
		return static_cast<float>(pixels[index]) / 255.f;
	};
	const float a = glm::mix(read(x0, y0), read(x1, y0), tx);
	const float b = glm::mix(read(x0, y1), read(x1, y1), tx);
	return glm::mix(a, b, ty);
}



bool BuildLandscapeGeometry(const HRL_Landscape* landscape, const HRL_LandscapeHeightmap* heightmap,
	std::vector<HRL_Vertex3D>& vertices, std::vector<HRL_uint>& indices,
	glm::vec3& boundsCenter, float& boundsRadius)
{
	if (!landscape || !heightmap)
		return false;

	const HRL_uint rx = std::clamp<HRL_uint>(landscape->resolution_x_, 2u, 512u);
	const HRL_uint rz = std::clamp<HRL_uint>(landscape->resolution_z_, 2u, 512u);
	const size_t vertexCount = static_cast<size_t>(rx + 1u) * static_cast<size_t>(rz + 1u);
	const size_t indexCount = static_cast<size_t>(rx) * static_cast<size_t>(rz) * 6u;
	if (vertexCount > static_cast<size_t>(std::numeric_limits<int>::max()) ||
		indexCount > static_cast<size_t>(std::numeric_limits<int>::max()))
		return false;

	vertices.resize(vertexCount);
	indices.resize(indexCount);

	const float cellX = landscape->size_.x / static_cast<float>(rx);
	const float cellZ = landscape->size_.y / static_cast<float>(rz);
	const float du = 1.f / static_cast<float>(rx);
	const float dv = 1.f / static_cast<float>(rz);

	float minHeight = std::numeric_limits<float>::max();
	float maxHeight = -std::numeric_limits<float>::max();

	for (HRL_uint z = 0; z <= rz; ++z)
	{
		const float v = static_cast<float>(z) / static_cast<float>(rz);
		for (HRL_uint x = 0; x <= rx; ++x)
		{
			const float u = static_cast<float>(x) / static_cast<float>(rx);
			const float h = SampleLandscapeHeight(heightmap, u, v) * landscape->height_scale_;
			minHeight = std::min(minHeight, h);
			maxHeight = std::max(maxHeight, h);

			HRL_Vertex3D& vertex = vertices[static_cast<size_t>(z) * static_cast<size_t>(rx + 1u) + x];
			const float hL = SampleLandscapeHeight(heightmap, std::max(0.f, u - du), v) * landscape->height_scale_;
			const float hR = SampleLandscapeHeight(heightmap, std::min(1.f, u + du), v) * landscape->height_scale_;
			const float hD = SampleLandscapeHeight(heightmap, u, std::max(0.f, v - dv)) * landscape->height_scale_;
			const float hU = SampleLandscapeHeight(heightmap, u, std::min(1.f, v + dv)) * landscape->height_scale_;

			const float dhx = (hR - hL) / std::max(2.f * cellX, 1e-6f);
			const float dhz = (hU - hD) / std::max(2.f * cellZ, 1e-6f);
			glm::vec3 normal = glm::normalize(glm::vec3(-dhx, 1.f, -dhz));
			glm::vec3 tangent = glm::normalize(glm::vec3(1.f, dhx, 0.f));
			tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
			if (glm::dot(tangent, tangent) < 1e-8f)
				tangent = glm::vec3(1.f, 0.f, 0.f);
			glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));
			if (glm::dot(bitangent, bitangent) < 1e-8f)
				bitangent = glm::vec3(0.f, 0.f, 1.f);

			const float localX = (u - 0.5f) * landscape->size_.x;
			const float localZ = (v - 0.5f) * landscape->size_.y;
			vertex.position[0] = localX;
			vertex.position[1] = h;
			vertex.position[2] = localZ;
			vertex.normal[0] = normal.x;
			vertex.normal[1] = normal.y;
			vertex.normal[2] = normal.z;
			vertex.uv[0] = u * landscape->uv_scale_.x;
			vertex.uv[1] = v * landscape->uv_scale_.y;
			vertex.tangent[0] = tangent.x;
			vertex.tangent[1] = tangent.y;
			vertex.tangent[2] = tangent.z;
			vertex.bitangent[0] = bitangent.x;
			vertex.bitangent[1] = bitangent.y;
			vertex.bitangent[2] = bitangent.z;
		}
	}

	size_t writeIndex = 0;
	for (HRL_uint z = 0; z < rz; ++z)
	{
		for (HRL_uint x = 0; x < rx; ++x)
		{
			const HRL_uint a = z * (rx + 1u) + x;
			const HRL_uint b = a + 1u;
			const HRL_uint c = a + (rx + 1u);
			const HRL_uint d = c + 1u;
			// a-c-b gives an upward-facing (+Y) normal in the HRL coordinate system.
			indices[writeIndex++] = a;
			indices[writeIndex++] = c;
			indices[writeIndex++] = b;
			indices[writeIndex++] = b;
			indices[writeIndex++] = c;
			indices[writeIndex++] = d;
		}
	}

	boundsCenter = glm::vec3(0.f, 0.5f * (minHeight + maxHeight), 0.f);
	const glm::vec3 halfExtents(
		0.5f * std::abs(landscape->size_.x),
		0.5f * std::abs(maxHeight - minHeight),
		0.5f * std::abs(landscape->size_.y));
	boundsRadius = glm::length(halfExtents);
	return true;
}



// Falloff of one emitter (distance in voxels) : same curve as the old voxel
// point lights evaluated on the voxel plane (lifted by half a voxel).
float VoxelEmitterFalloff(float distance, float falloff)
{
	const float d2 = distance * distance + 0.25f;
	return (0.5f / std::sqrt(d2)) / (1.f + d2 / std::max(falloff * falloff, 1e-6f));
}



// Energy of the falloff in the ring [a, b] (voxels) : 2 pi integral f(r) r dr.
float VoxelEmitterRingEnergy(float a, float b, float falloff)
{
	if (b <= a)
		return 0.f;
	constexpr int kSteps = 64; // Simpson
	const float h = (b - a) / kSteps;
	float sum = 0.f;
	for (int i = 0; i <= kSteps; ++i)
	{
		const float r = a + h * static_cast<float>(i);
		const float w = (i == 0 || i == kSteps) ? 1.f : ((i & 1) ? 4.f : 2.f);
		sum += w * VoxelEmitterFalloff(r, falloff) * r;
	}
	return 2.f * 3.14159265f * sum * h / 3.f;
}



// Weight of each pyramid level (cached : same falloff / texel size / levels).
const std::array<float, 10>& VoxelLightLevelWeights(float falloff, int s, int levels)
{
	static std::array<float, 10> weights{};
	static float cachedFalloff = -1.f;
	static int cachedS = -1;
	static int cachedLevels = -1;
	if (cachedFalloff == falloff && cachedS == s && cachedLevels == levels)
		return weights;
	cachedFalloff = falloff;
	cachedS = s;
	cachedLevels = levels;

	weights.fill(0.f);
	auto radius = [&](int l) { return l == 0 ? 0.f : 0.75f * static_cast<float>(s << l); };
	for (int l = 0; l < levels && l < 10; ++l)
		weights[static_cast<size_t>(l)] = VoxelEmitterRingEnergy(radius(l), radius(l + 1), falloff);
	// Level 0 only lights the emitter's own texel : capped there (else the
	// emitter and what overlaps it are over-lit), the rest goes one level up.
	constexpr float kLevel0Max = 0.6f;
	if (levels > 1 && weights[0] > kLevel0Max)
	{
		weights[1] += weights[0] - kLevel0Max;
		weights[0] = kLevel0Max;
	}
	return weights;
}



bool BuildGizmoOverlay(const hrl_scene_t* scene, const HRL_Viewport* viewport, HRL_id viewport_id, const glm::mat4& view, DebugRenderer& overlay)
{
    if (!scene || !viewport || !viewport->camera_ || viewport_id == HRL_INVALID_ID || scene->gizmos.empty()) return false;

    for (const auto& [id, g] : scene->gizmos) {
        (void)id;
        if (!g || !g->visible_ || g->viewport_ != viewport_id) continue;
        const float size = GizmoSizeRenderer(g, viewport, view);
        const glm::vec3 center = g->position_;

        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_TRANSLATE)) && g->show_translate_) {
            for (int axis=0; axis<3; ++axis) {
                if (!(g->translate_axes_ & (1<<axis))) continue;
                const glm::vec3 dir = GizmoAxisRenderer(g, axis);
                const glm::vec3 end = center + dir * size;
                const glm::vec4 col = (g->hovered_operation_ == HRL_GIZMO_OPERATION_TRANSLATE && g->hovered_part_ == static_cast<HRL_EGizmoPart>(axis+1)) ? g->hover_color_ : g->axis_colors_[axis];
                GizmoPushLine(overlay, center, end, col);
                glm::vec3 side = glm::cross(dir, glm::normalize(viewport->camera_->position_ - center));
                if (glm::length(side) < 1e-4f) side = glm::cross(dir, glm::vec3(0,1,0));
                side = glm::normalize(side);
                const float head = size * 0.14f;
                GizmoPushLine(overlay, end, end - dir*head + side*head*0.55f, col);
                GizmoPushLine(overlay, end, end - dir*head - side*head*0.55f, col);
            }
            if (g->hovered_operation_ == HRL_GIZMO_OPERATION_TRANSLATE && g->hovered_part_ == HRL_GIZMO_PART_CENTER) {
                const glm::vec3 forward = GetForwardVector(viewport->camera_->rotation_);
                glm::vec3 right = glm::cross(forward, glm::vec3(0,1,0));
                if (glm::length(right) < 1e-4f) right = glm::cross(forward, glm::vec3(1,0,0));
                right = glm::normalize(right);
                const glm::vec3 up = glm::normalize(glm::cross(right, forward));
                const float box=size*0.16f;
                const glm::vec4 col=g->hover_color_;
                GizmoPushLine(overlay, center-right*box-up*box, center+right*box-up*box, col);
                GizmoPushLine(overlay, center+right*box-up*box, center+right*box+up*box, col);
                GizmoPushLine(overlay, center+right*box+up*box, center-right*box+up*box, col);
                GizmoPushLine(overlay, center-right*box+up*box, center-right*box-up*box, col);
            }
        }

        // Scale intentionally mirrors translation geometry: one axis per handle, with a square endpoint.
        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_SCALE)) && g->show_scale_) {
            for (int axis=0; axis<3; ++axis) {
                if (!(g->scale_axes_ & (1<<axis))) continue;
                const glm::vec3 dir=GizmoAxisRenderer(g,axis);
                glm::vec3 side=(std::abs(dir.y)<0.9f)?glm::normalize(glm::cross(dir,glm::vec3(0,1,0))):glm::normalize(glm::cross(dir,glm::vec3(1,0,0)));
                const glm::vec3 up=glm::normalize(glm::cross(side,dir));
                const glm::vec3 end=center+dir*size;
                const float h=size*0.10f;
                const glm::vec4 col=(g->hovered_operation_==HRL_GIZMO_OPERATION_SCALE&&g->hovered_part_==static_cast<HRL_EGizmoPart>(axis+1))?g->hover_color_:g->axis_colors_[axis];
                GizmoPushLine(overlay,center,end,col);
                GizmoPushLine(overlay,end-side*h-up*h,end+side*h-up*h,col);
                GizmoPushLine(overlay,end+side*h-up*h,end+side*h+up*h,col);
                GizmoPushLine(overlay,end+side*h+up*h,end-side*h+up*h,col);
                GizmoPushLine(overlay,end-side*h+up*h,end-side*h-up*h,col);
            }
        }

        // Rotation uses three separate quarter wheels. They remain independently pickable.
        if ((static_cast<int>(g->mode_) & static_cast<int>(HRL_GIZMO_MODE_ROTATE)) && g->show_rotate_) {
            const float sweep=glm::radians(glm::clamp(g->rotate_arc_degrees_,15.f,170.f));
            for (int axis=0; axis<3; ++axis) {
                if (!(g->rotate_axes_ & (1<<axis))) continue;
                const glm::vec3 n=GizmoAxisRenderer(g,axis);
                // Keep every quarter-wheel aligned to the next positive axis:
                // X: +Y -> +Z, Y: +Z -> +X, Z: +X -> +Y.
                const glm::vec3 u=GizmoAxisRenderer(g,(axis+1)%3);
                const float start=0.f;
                const glm::vec4 col=(g->hovered_operation_==HRL_GIZMO_OPERATION_ROTATE&&g->hovered_part_==static_cast<HRL_EGizmoPart>(axis+1))?g->hover_color_:g->axis_colors_[axis];
                GizmoPushArc(overlay,center,n,u,size,start,sweep,20,col);
            }
        }

        if ((static_cast<int>(g->mode_) & (static_cast<int>(HRL_GIZMO_MODE_ROTATE) | static_cast<int>(HRL_GIZMO_MODE_SCALE))) != 0) {
            const float s=size*0.06f;
            const glm::vec4 col=g->center_color_;
            GizmoPushLine(overlay,center-glm::vec3(s,0,0),center+glm::vec3(s,0,0),col);
            GizmoPushLine(overlay,center-glm::vec3(0,s,0),center+glm::vec3(0,s,0),col);
            GizmoPushLine(overlay,center-glm::vec3(0,0,s),center+glm::vec3(0,0,s),col);
        }
    }
    return !overlay.lines.empty();
}
void ComputeLightShadowMatrices(const HRL_Light* light, glm::mat4& lightView, glm::mat4& projection)
{
	// Same matrices for the shadow rasterization and the lit shaders.
	lightView = glm::mat4(1.f);
	projection = glm::mat4(1.f);
	if (light->type_ == HRL_POINT_LIGHT)
	{
		projection = glm::perspective(glm::radians(90.f), 1.f, HRL_POINT_SHADOW_NEAR, HRL_POINT_SHADOW_FAR);
		return;
	}
	const glm::vec3 lightDir = GetLightDirection(light);
	const glm::vec3 center = (light->type_ == HRL_SPOT_LIGHT) ? light->position_ : glm::vec3(0.f);
	const glm::vec3 up = std::abs(glm::dot(lightDir, glm::vec3(0,1,0))) > 0.98f
		? glm::vec3(0,0,1) : glm::vec3(0,1,0);
	if (light->type_ == HRL_SPOT_LIGHT)
	{
		const float outerAngle = glm::clamp(light->outerCutoff, 1.f, 89.0f);
		projection = glm::perspective(glm::radians(outerAngle * 2.f), 1.f, 0.1f, 100.f);
		lightView = glm::lookAt(light->position_, light->position_ + lightDir, up);
	}
	else
	{
		const glm::vec3 lightPosition = center - lightDir * 80.f;
		projection = glm::ortho(-60.f, 60.f, -60.f, 60.f, 0.1f, 200.f);
		lightView = glm::lookAt(lightPosition, center, up);
	}
}

float HRL_InternalRenderTimeSeconds()
{
	// Time used by animated shader effects (clouds, film grain). Setting
	// HRL_DEBUG_FIXED_TIME freezes it, which makes renders reproducible.
	static const char* fixed = std::getenv("HRL_DEBUG_FIXED_TIME");
	if (fixed && *fixed)
		return static_cast<float>(std::atof(fixed));
	static const auto start = std::chrono::steady_clock::now();
	return static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}
