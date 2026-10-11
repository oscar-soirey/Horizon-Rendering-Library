// HRL - OpenGL ES 3.0 backend (mobile). See gles3_renderer.h.
//
// Structure and conventions follow the OpenGL 3.3 backend (gl33_renderer.cpp)
// so both stay easy to compare; the code shared verbatim with it (LOD
// selection, frustum culling, VFX particle transforms, PNG writer) is kept
// identical on purpose.
//
// Main differences with the desktop backend:
//  - A scene created with HRL_CreateScene(true) is drawn straight into the
//    default framebuffer (no intermediate HDR target, no blit): this is the
//    cheapest path on tile-based mobile GPUs. Off-screen scenes render into an
//    RGBA8 texture.
//  - One color output per shader (no bloom / picking / G-buffer attachments).
//  - Only OpenGL ES 3.0 entry points are used.
#include "gles3_renderer.h"

#include "../../hrl_gl.h"

#include "gles3_gl.h"
#include "gles3_shader.h"
#include "gles3_texture.h"
#include "gles3_shaders.h"
#include "../../ressources/ressources.h"
#include "../../core/utils_functions.h"
#include "../../core/widgets.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <cstring>

namespace
{

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

#define GLES3_MAX_LIGHTS 32

// Material texture units 0..5 follow tex_uniform_name[] of the GL 3.3 backend.
#define GLES3_ALBEDO_INT 0
#define GLES3_NORMAL_INT 1
#define GLES3_SPECULAR_INT 2
#define GLES3_ROUGHNESS_INT 3
#define GLES3_METALLIC_INT 4
#define GLES3_ALPHA_INT 5
#define GLES3_MATERIAL_TEXTURE_COUNT 6
#define GLES3_AO_TEXTURE_UNIT 6
#define GLES3_SKY_TEXTURE_UNIT 7

#define GLES3_MAX_SPRITE_BATCH_INSTANCES 16384

const char* const kTextureUniformNames[GLES3_MATERIAL_TEXTURE_COUNT] =
{
	"T_Albedo", "T_Normal", "T_Specular", "T_Roughness", "T_Metallic", "T_Alpha"
};

// std140 layout of "Light" in the shaders (same as GL_Light in the GL 3.3 backend).
struct GLES3_Light
{
	uint32_t type;
	float intensity;
	float attenuation;
	float innerCutoff;

	glm::vec3 position;
	float outerCutoff;

	glm::vec3 rotation;
	float padding3;

	glm::vec3 color;
	float shadowStrength;

	glm::mat4 shadowMatrix;
	glm::vec4 shadowParams;
};
static_assert(sizeof(GLES3_Light) == 144, "GLES3_Light must match the std140 Light layout");

struct GLES3_Scene
{
	bool on_screen = true;
	// Off-screen scenes only (on-screen scenes use the default framebuffer).
	GLuint fbo = 0;
	GLuint color_texture = 0;
	GLuint depth_rbo = 0;
	int width = 0;
	int height = 0;
};

struct MeshLOD_GPU
{
	GLuint vao = 0;
	GLuint vbo = 0;
	GLuint ebo = 0;
	GLsizei vertex_count = 0;
	GLsizei index_count = 0;
	bool indexed = false;
};

struct MeshGPU
{
	std::vector<MeshLOD_GPU> levels;
};

struct SkeletalMeshGPU
{
	GLuint vao = 0;
	GLuint vbo = 0;
	GLuint ebo = 0;
	GLuint bone_ubo = 0;
	GLsizei vertex_count = 0;
	GLsizei index_count = 0;
	HRL_uint bone_count = 0;
	bool indexed = false;
	uint64_t uploaded_pose_serial = 0;
};

struct SpriteBatchInstance
{
	glm::mat4 model;
	glm::vec4 uvRegion;
};

struct VFXRenderInstance
{
	glm::mat4 model{1.f};
	glm::vec4 color{1.f};
};

// One textured quad of the UI pass (widgets, text, screen messages).
struct UIQuad
{
	HRL_id texture = HRL_INVALID_ID; // HRL_INVALID_ID : plain color
	glm::vec4 tint{1.f};
	bool sdf = false;
};

struct GLES3_Backend
{
	// Dynamic buffers: UI quads and debug primitives.
	GLuint ui_vao = 0, ui_vbo = 0;
	size_t ui_vbo_capacity = 0;
	GLuint debug_vao = 0, debug_vbo = 0;
	size_t debug_vbo_capacity = 0;

	GLuint light_ubo = 0;

	std::unordered_map<HRL_id, GLES3_Scene*> gpu_scenes;
	std::unordered_map<HRL_id, GLES3_Shader*> shaders;
	std::unordered_map<HRL_id, GLES3_Texture*> textures;
	std::unordered_map<HRL_id, MeshGPU> meshes;
	std::unordered_map<HRL_id, SkeletalMeshGPU> skeletal_meshes;

	// Sampler forcing NEAREST filtering while sprites are drawn (same rule as
	// the desktop backend: sprites are always pixel-perfect).
	GLuint nearest_sampler = 0;

	// Shared sprite plane + per-instance stream.
	GLuint sprite_vao = 0;
	GLuint sprite_instanced_vao = 0;
	GLuint sprite_vbo = 0;
	GLuint sprite_ebo = 0;
	GLuint sprite_instance_vbo = 0;
	size_t sprite_instance_capacity = 0;

	// VFX billboards.
	GLuint vfx_vao = 0;
	GLuint vfx_vbo = 0;
	GLuint vfx_ebo = 0;
	GLuint vfx_instance_vbo = 0;
	size_t vfx_instance_capacity = 0;
	GLES3_Shader* vfx_shader = nullptr;

	// Sky sphere.
	GLuint sky_vao = 0;
	GLuint sky_vbo = 0;
	GLuint sky_ebo = 0;
	GLsizei sky_index_count = 0;
	GLES3_Shader* sky_shader = nullptr;

	GLES3_Shader* ui_shader = nullptr;
	GLuint ui_white_texture = 0;

	HRL_id fallback_textures[GLES3_MATERIAL_TEXTURE_COUNT] =
	{
		HRL_INVALID_ID, HRL_INVALID_ID, HRL_INVALID_ID, HRL_INVALID_ID, HRL_INVALID_ID, HRL_INVALID_ID
	};
	float fallback_metallic = 0.f;

	// Lights of the scene being drawn (see UploadSceneLights).
	int light_count = 0;
	bool only_sky_lights = true;
	glm::vec3 sky_light_sum{0.f};
	GLES3_Light light_cache[GLES3_MAX_LIGHTS];
	bool light_cache_valid = false;

	// Cached SDF textures of the screen messages.
	struct ScreenMessageTextGPU
	{
		HRL_id texture = HRL_INVALID_ID;
		HRL_id font = HRL_INVALID_ID;
		float size = 0.0f;
		std::string text;
		int width = 0;
		int height = 0;
	};
	std::unordered_map<HRL_id, ScreenMessageTextGPU> screen_message_textures;

	bool warned_post_process = false;
};

//Render context, used and updated every frame
struct GLES3_State
{
	HRL_Viewport* viewport = nullptr;
	GLES3_Shader* shader = nullptr;

	glm::mat4 proj_mat{1.f};
	glm::mat4 view_mat{1.f};

	hrl_fog_t* current_fog = nullptr;
	hrl_scene_t* current_scene = nullptr;

	// Material/shader cache used by renderer-side state batching.
	HRL_Material* bound_material = nullptr;
	GLES3_Shader* bound_shader = nullptr;
};

GLES3_Backend* bck_ = nullptr;
GLES3_State* ctx_ = nullptr;

bool MaterialIsTwoSided(const HRL_Material* material)
{
	return material && material->two_sided_;
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
	return model;
}

glm::mat4 CalculateProjectionMatrix()
{
	float viewportWidth  = (float)GetWindowWidth() * ctx_->viewport->width_;
	float viewportHeight = (float)GetWindowHeight() * ctx_->viewport->height_;
	if (viewportHeight < 1e-3f)
		viewportHeight = 1.f;
	const float aspect = viewportWidth / viewportHeight;

	const HRL_Camera* camera = ctx_->viewport->camera_;
	if (camera->type_ == HRL_PERSPECTIVE)
		return glm::perspective(glm::radians(camera->value_), aspect, camera->near_plane_, camera->far_plane_);

	const float halfHeight = camera->value_ * 0.5f;
	const float halfWidth  = halfHeight * aspect;
	return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, camera->near_plane_, camera->far_plane_);
}

glm::mat4 CalculateViewMatrix()
{
	const HRL_Camera* camera = ctx_->viewport->camera_;
	return glm::lookAt(
		camera->position_,
		camera->position_ + GetForwardVector(camera->rotation_),
		GetUpVector(camera->rotation_));
}

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

// ---------------------------------------------------------------------------
// Code shared verbatim with the OpenGL 3.3 backend
// ---------------------------------------------------------------------------

static int SelectMeshLOD(const HRL_Mesh* mesh, const glm::mat4& model,
    const glm::mat4& view, const glm::mat4& projection, float viewportHeight)
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
		if (ctx_->viewport && ctx_->viewport->camera_)
			cameraPos = ctx_->viewport->camera_->position_;
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
		const bool perspective = ctx_->viewport && ctx_->viewport->camera_ &&
			ctx_->viewport->camera_->type_ == HRL_PERSPECTIVE;
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

struct FrustumPlaneSet
{
	glm::vec4 planes[6];
};

static glm::vec4 NormalizePlane(const glm::vec4& p)
{
	const float len = glm::length(glm::vec3(p));
	return (len > 1e-6f) ? (p / len) : p;
}

static FrustumPlaneSet BuildFrustum(const glm::mat4& clip)
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

static bool SphereInsideFrustum(const FrustumPlaneSet& frustum, const glm::vec3& center, float radius)
{
	for (const glm::vec4& p : frustum.planes)
	{
		if (glm::dot(glm::vec3(p), center) + p.w < -radius)
			return false;
	}
	return true;
}

static bool IsMeshVisible(const HRL_Mesh* mesh, const glm::mat4& model, const FrustumPlaneSet& frustum)
{
	if (!mesh || mesh->bounds_radius_ <= 0.f)
		return true;
	const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
	const float maxScale = std::max({std::abs(mesh->scale_.x), std::abs(mesh->scale_.y), std::abs(mesh->scale_.z)});
	const float radius = mesh->bounds_radius_ * std::max(maxScale, 1e-6f);
	return SphereInsideFrustum(frustum, center, radius);
}

static bool IsFiniteBounds(const HRL_Mesh* mesh)
{
	return mesh && std::isfinite(mesh->bounds_center_.x) && std::isfinite(mesh->bounds_center_.y) &&
		std::isfinite(mesh->bounds_center_.z) && std::isfinite(mesh->bounds_radius_);
}

static glm::mat4 MakeVFXSystemMatrix(const HRL_VFXSystem* system)
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
static glm::mat4 MakeVFXParticleModel(const HRL_VFXParticle& p, const glm::mat4* systemMatrix,
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

static bool WritePNG_RGBA8(const char* path, int width, int height, const std::vector<unsigned char>& rgbaTopDown)
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

// ---------------------------------------------------------------------------
// Resources helpers
// ---------------------------------------------------------------------------

GLES3_Shader* CreateBuiltinShader(const char* vert, const char* frag)
{
	auto* shader = new GLES3_Shader();
	if (shader->GLES3_Create(vert, std::strlen(vert), frag, std::strlen(frag)) != 0)
	{
		delete shader;
		return nullptr;
	}
	return shader;
}

void RegisterBuiltinShader(HRL_id id, const char* vert, const char* frag)
{
	if (GLES3_Shader* shader = CreateBuiltinShader(vert, frag))
		bck_->shaders.emplace(id, shader);
}

HRL_id CreateFallbackTexture(const unsigned char* png, size_t size)
{
	auto* t = new GLES3_Texture();
	// The default maps are large single-color images: keep them as 1x1.
	if (t->GLES3_Create(reinterpret_cast<const char*>(png), size, true) != 0)
	{
		delete t;
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	bck_->textures.emplace(id, t);
	return id;
}

GLuint FallbackTextureGL(int index)
{
	auto it = bck_->textures.find(bck_->fallback_textures[index]);
	return (it != bck_->textures.end() && it->second) ? it->second->GetGL_ID() : 0;
}

GLuint WhiteTextureGL()
{
	if (bck_->ui_white_texture == 0)
	{
		const unsigned char white[4] = {255, 255, 255, 255};
		glGenTextures(1, &bck_->ui_white_texture);
		glBindTexture(GL_TEXTURE_2D, bck_->ui_white_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	return bck_->ui_white_texture;
}

void DestroyMeshLOD(MeshLOD_GPU& gpu)
{
	if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
	if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
	if (gpu.ebo) glDeleteBuffers(1, &gpu.ebo);
	gpu = {};
}

void DestroySkeletalMesh(SkeletalMeshGPU& gpu)
{
	if (gpu.vao) glDeleteVertexArrays(1, &gpu.vao);
	if (gpu.vbo) glDeleteBuffers(1, &gpu.vbo);
	if (gpu.ebo) glDeleteBuffers(1, &gpu.ebo);
	if (gpu.bone_ubo) glDeleteBuffers(1, &gpu.bone_ubo);
	gpu = {};
}

const MeshLOD_GPU* GetMeshLOD_GPU(const MeshGPU& gpu, int level)
{
	if (level < 0 || gpu.levels.empty())
		return nullptr;
	const size_t index = (size_t)level;
	if (index >= gpu.levels.size())
		return &gpu.levels.front();
	const auto& lod = gpu.levels[index];
	if (lod.vao == 0 || lod.vertex_count <= 0)
		return nullptr;
	return &lod;
}

void SetVertex3DAttributes(GLsizei stride, size_t baseOffset)
{
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(baseOffset + offsetof(HRL_Vertex3D, position)));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(baseOffset + offsetof(HRL_Vertex3D, normal)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(baseOffset + offsetof(HRL_Vertex3D, uv)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(baseOffset + offsetof(HRL_Vertex3D, tangent)));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(baseOffset + offsetof(HRL_Vertex3D, bitangent)));
	glEnableVertexAttribArray(4);
}

bool UploadMeshLOD(MeshLOD_GPU& gpu,
	const HRL_Vertex3D* vertices, size_t vertexCount,
	const HRL_uint* indices, size_t indexCount)
{
	if (!vertices || vertexCount == 0 ||
		vertexCount > (size_t)std::numeric_limits<GLsizei>::max() ||
		indexCount > (size_t)std::numeric_limits<GLsizei>::max() ||
		(indexCount != 0 && !indices))
		return false;

	glGenVertexArrays(1, &gpu.vao);
	glGenBuffers(1, &gpu.vbo);
	if (!gpu.vao || !gpu.vbo)
	{
		DestroyMeshLOD(gpu);
		return false;
	}

	glBindVertexArray(gpu.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertexCount * sizeof(HRL_Vertex3D)), vertices, GL_STATIC_DRAW);
	SetVertex3DAttributes((GLsizei)sizeof(HRL_Vertex3D), 0);

	if (indexCount)
	{
		glGenBuffers(1, &gpu.ebo);
		if (!gpu.ebo)
		{
			glBindVertexArray(0);
			DestroyMeshLOD(gpu);
			return false;
		}
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * sizeof(HRL_uint)), indices, GL_STATIC_DRAW);
		gpu.indexed = true;
		gpu.index_count = (GLsizei)indexCount;
	}

	gpu.vertex_count = (GLsizei)vertexCount;
	glBindVertexArray(0);
	return true;
}

bool UploadSkeletalMesh(SkeletalMeshGPU& gpu,
	const HRL_SkeletalVertex* vertices, size_t vertexCount,
	const HRL_uint* indices, size_t indexCount, HRL_uint boneCount)
{
	if (!vertices || vertexCount < 3 || boneCount == 0 || boneCount > HRL_MAX_SKELETAL_BONES)
		return false;
	if (indexCount > 0 && !indices)
		return false;
	if (vertexCount > (size_t)std::numeric_limits<GLsizei>::max() || indexCount > (size_t)std::numeric_limits<GLsizei>::max())
		return false;

	glGenVertexArrays(1, &gpu.vao);
	glGenBuffers(1, &gpu.vbo);
	glGenBuffers(1, &gpu.bone_ubo);
	if (!gpu.vao || !gpu.vbo || !gpu.bone_ubo)
	{
		DestroySkeletalMesh(gpu);
		return false;
	}

	glBindVertexArray(gpu.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vertexCount * sizeof(HRL_SkeletalVertex)), vertices, GL_STATIC_DRAW);
	const GLsizei stride = (GLsizei)sizeof(HRL_SkeletalVertex);
	SetVertex3DAttributes(stride, offsetof(HRL_SkeletalVertex, vertex));
	glVertexAttribIPointer(5, 4, GL_UNSIGNED_INT, stride, reinterpret_cast<const void*>(offsetof(HRL_SkeletalVertex, boneIndices)));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HRL_SkeletalVertex, boneWeights)));
	glEnableVertexAttribArray(6);

	if (indexCount > 0)
	{
		glGenBuffers(1, &gpu.ebo);
		if (!gpu.ebo)
		{
			glBindVertexArray(0);
			DestroySkeletalMesh(gpu);
			return false;
		}
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * sizeof(HRL_uint)), indices, GL_STATIC_DRAW);
		gpu.indexed = true;
		gpu.index_count = (GLsizei)indexCount;
	}

	// Every bone slot starts as identity so unused indices are harmless.
	std::vector<glm::mat4> identity(HRL_MAX_SKELETAL_BONES, glm::mat4(1.f));
	glBindBuffer(GL_UNIFORM_BUFFER, gpu.bone_ubo);
	glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)(HRL_MAX_SKELETAL_BONES * sizeof(glm::mat4)), identity.data(), GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);

	gpu.vertex_count = (GLsizei)vertexCount;
	gpu.bone_count = boneCount;
	gpu.uploaded_pose_serial = 0;
	glBindVertexArray(0);
	return true;
}

void UploadSkeletalBones(HRL_SkeletalMesh* mesh, SkeletalMeshGPU& gpu)
{
	if (!mesh || gpu.bone_ubo == 0)
		return;
	if (gpu.uploaded_pose_serial == mesh->pose_serial_)
		return;

	const size_t boneCount = std::min(mesh->bone_matrices_.size(), (size_t)HRL_MAX_SKELETAL_BONES);
	glBindBuffer(GL_UNIFORM_BUFFER, gpu.bone_ubo);
	if (boneCount > 0)
		glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)(boneCount * sizeof(glm::mat4)), mesh->bone_matrices_.data());
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	gpu.uploaded_pose_serial = mesh->pose_serial_;
}

// Unit plane shared by sprites and VFX billboards.
void UploadUnitPlane(GLuint vbo, GLuint ebo)
{
	const HRL_Vertex3D vertices[4] = {
		{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{ 0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
		{{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}
	};
	const HRL_uint indices[6] = {0, 1, 2, 2, 3, 0};
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}

void SetUnitPlaneAttributes()
{
	const GLsizei stride = (GLsizei)sizeof(HRL_Vertex3D);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(HRL_Vertex3D, position));
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(HRL_Vertex3D, uv));
	glEnableVertexAttribArray(2);
}

void SetInstanceMatrixAttributes(GLuint firstLocation, GLsizei stride, size_t offset)
{
	for (GLuint column = 0; column < 4; ++column)
	{
		const GLuint location = firstLocation + column;
		glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, stride,
			reinterpret_cast<const void*>(offset + sizeof(glm::vec4) * column));
		glEnableVertexAttribArray(location);
		glVertexAttribDivisor(location, 1);
	}
}

void CreateSpriteGeometry()
{
	glGenVertexArrays(1, &bck_->sprite_vao);
	glGenVertexArrays(1, &bck_->sprite_instanced_vao);
	glGenBuffers(1, &bck_->sprite_vbo);
	glGenBuffers(1, &bck_->sprite_ebo);
	glGenBuffers(1, &bck_->sprite_instance_vbo);

	glBindVertexArray(bck_->sprite_vao);
	UploadUnitPlane(bck_->sprite_vbo, bck_->sprite_ebo);
	SetUnitPlaneAttributes();

	glBindVertexArray(bck_->sprite_instanced_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->sprite_ebo);
	SetUnitPlaneAttributes();

	glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_instance_vbo);
	bck_->sprite_instance_capacity = sizeof(SpriteBatchInstance) * 256u;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->sprite_instance_capacity, nullptr, GL_STREAM_DRAW);
	const GLsizei instanceStride = (GLsizei)sizeof(SpriteBatchInstance);
	SetInstanceMatrixAttributes(5, instanceStride, offsetof(SpriteBatchInstance, model));
	glVertexAttribPointer(9, 4, GL_FLOAT, GL_FALSE, instanceStride, (const void*)offsetof(SpriteBatchInstance, uvRegion));
	glEnableVertexAttribArray(9);
	glVertexAttribDivisor(9, 1);
	glBindVertexArray(0);
}

void CreateVFXGeometry()
{
	glGenVertexArrays(1, &bck_->vfx_vao);
	glGenBuffers(1, &bck_->vfx_vbo);
	glGenBuffers(1, &bck_->vfx_ebo);
	glGenBuffers(1, &bck_->vfx_instance_vbo);

	glBindVertexArray(bck_->vfx_vao);
	UploadUnitPlane(bck_->vfx_vbo, bck_->vfx_ebo);
	SetUnitPlaneAttributes();

	glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_instance_vbo);
	bck_->vfx_instance_capacity = sizeof(VFXRenderInstance) * 256u;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->vfx_instance_capacity, nullptr, GL_STREAM_DRAW);
	const GLsizei instanceStride = (GLsizei)sizeof(VFXRenderInstance);
	SetInstanceMatrixAttributes(3, instanceStride, offsetof(VFXRenderInstance, model));
	glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, instanceStride, (const void*)offsetof(VFXRenderInstance, color));
	glEnableVertexAttribArray(7);
	glVertexAttribDivisor(7, 1);
	glBindVertexArray(0);
}

void CreateSkySphereGeometry()
{
	constexpr int slices = 32;
	constexpr int stacks = 16;
	constexpr float pi = 3.14159265358979323846f;
	constexpr float twoPi = 6.28318530717958647692f;

	std::vector<glm::vec3> positions;
	positions.reserve((size_t)(slices + 1) * (size_t)(stacks + 1));
	for (int y = 0; y <= stacks; ++y)
	{
		const float phi = pi * (float)y / (float)stacks;
		for (int x = 0; x <= slices; ++x)
		{
			const float theta = twoPi * (float)x / (float)slices;
			positions.emplace_back(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
		}
	}

	std::vector<HRL_uint> indices;
	indices.reserve((size_t)slices * (size_t)stacks * 6u);
	for (int y = 0; y < stacks; ++y)
	{
		for (int x = 0; x < slices; ++x)
		{
			const HRL_uint a = (HRL_uint)(y * (slices + 1) + x);
			const HRL_uint b = a + 1u;
			const HRL_uint c = a + (HRL_uint)(slices + 1);
			const HRL_uint d = c + 1u;
			// Outside winding; the renderer culls front faces to draw the inside.
			indices.push_back(a); indices.push_back(b); indices.push_back(c);
			indices.push_back(b); indices.push_back(d); indices.push_back(c);
		}
	}

	glGenVertexArrays(1, &bck_->sky_vao);
	glGenBuffers(1, &bck_->sky_vbo);
	glGenBuffers(1, &bck_->sky_ebo);
	glBindVertexArray(bck_->sky_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->sky_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(positions.size() * sizeof(glm::vec3)), positions.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)sizeof(glm::vec3), (void*)0);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bck_->sky_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(HRL_uint)), indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0);
	bck_->sky_index_count = (GLsizei)indices.size();
}

// (Re)creates the render target of an off-screen scene.
bool AllocateSceneTarget(GLES3_Scene* scene, int width, int height)
{
	width = std::max(1, width);
	height = std::max(1, height);

	if (scene->fbo == 0) glGenFramebuffers(1, &scene->fbo);
	if (scene->color_texture == 0) glGenTextures(1, &scene->color_texture);
	if (scene->depth_rbo == 0) glGenRenderbuffers(1, &scene->depth_rbo);

	glBindTexture(GL_TEXTURE_2D, scene->color_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindRenderbuffer(GL_RENDERBUFFER, scene->depth_rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, scene->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene->color_texture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, scene->depth_rbo);
	const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	scene->width = width;
	scene->height = height;
	if (!complete)
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3: off-screen scene framebuffer is incomplete");
	return complete;
}

void DestroySceneTarget(GLES3_Scene* scene)
{
	if (scene->fbo) glDeleteFramebuffers(1, &scene->fbo);
	if (scene->color_texture) glDeleteTextures(1, &scene->color_texture);
	if (scene->depth_rbo) glDeleteRenderbuffers(1, &scene->depth_rbo);
	scene->fbo = 0;
	scene->color_texture = 0;
	scene->depth_rbo = 0;
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

void UploadSceneLights(const hrl_scene_t* scene)
{
	GLES3_Light gpu_lights[GLES3_MAX_LIGHTS];
	std::memset(gpu_lights, 0, sizeof(gpu_lights));

	int count = 0;
	bool onlySky = true;
	glm::vec3 skySum(0.f);
	if (scene)
	{
		for (const auto& [id, light] : scene->lights)
		{
			(void)id;
			if (count >= GLES3_MAX_LIGHTS)
				break;
			if (!light)
				continue;
			GLES3_Light& dst = gpu_lights[count];
			dst.type = light->type_;
			dst.intensity = light->intensity_;
			dst.attenuation = light->attenuation_;
			dst.innerCutoff = std::cos(glm::radians(light->innerCutoff));
			dst.position = light->position_;
			dst.outerCutoff = std::cos(glm::radians(light->outerCutoff));
			dst.rotation = GetLightDirection(light);
			dst.color = light->color_;
			dst.shadowStrength = 0.f;
			dst.shadowMatrix = glm::mat4(1.f);
			dst.shadowParams = glm::vec4(0.f, 0.f, -1.f, 0.f); // no shadow map
			++count;

			if (light->intensity_ <= 0.f)
				continue;
			if (light->type_ == HRL_SKY_LIGHT)
				skySum += light->color_ * light->intensity_;
			else
				onlySky = false;
		}
	}

	bck_->light_count = count;
	bck_->only_sky_lights = onlySky;
	bck_->sky_light_sum = skySum;

	// The UBO is rewritten only when the lights actually changed.
	if (bck_->light_cache_valid && std::memcmp(bck_->light_cache, gpu_lights, sizeof(gpu_lights)) == 0)
		return;
	std::memcpy(bck_->light_cache, gpu_lights, sizeof(gpu_lights));
	bck_->light_cache_valid = true;

	glBindBuffer(GL_UNIFORM_BUFFER, bck_->light_ubo);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)sizeof(gpu_lights), gpu_lights);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

bool IsBuiltinMeshShader(HRL_id shader)
{
	return shader == HRL_MESH_3D_SHADER || shader == HRL_SKINNED_3D_MESH_SHADER;
}

bool BindMaterial(HRL_Material* mat, const HRL_Mesh* mesh, const glm::mat4& model)
{
	auto it = bck_->shaders.find(mat->shader_);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "Bind material error: shader doesn't exist");
		return false;
	}
	auto* s = it->second;
	ctx_->shader = s;
	const bool shaderChanged = ctx_->bound_shader != s;
	const bool materialChanged = shaderChanged || ctx_->bound_material != mat;
	if (shaderChanged)
	{
		s->Use();
		ctx_->bound_shader = s;
	}

	// Per-object state (redundant values are filtered by GLES3_Shader).
	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);
	s->SetMat4("model", model);
	if (mat->shader_ == HRL_MESH_3D_SHADER)
	{
		const glm::mat3 model3(model);
		glm::mat3 normalMatrix(1.0f);
		const float det = glm::determinant(model3);
		if (std::isfinite(det) && std::abs(det) > 1e-8f)
			normalMatrix = glm::transpose(glm::inverse(model3));
		s->SetMat3("normalMatrix", normalMatrix);
	}
	s->SetVec4("UVRegion", {mesh->region_[0], mesh->region_[1], mesh->region_[2], mesh->region_[3]});
	s->SetVec3("CamPos", ctx_->viewport->camera_->position_);

	if (shaderChanged)
	{
		if (mat->shader_ == HRL_SPRITE_SHADER)
			s->SetInt("uInstanced", 0);
		s->SetInt("LightCount", bck_->light_count);
		s->SetInt("FogEnabled", ctx_->current_fog->enabled);
		s->SetInt("FogMode", ctx_->current_fog->mode);
		s->SetVec4("FogColor", {ctx_->current_fog->r, ctx_->current_fog->g, ctx_->current_fog->b, 1.f});
		s->SetFloat("FogStart", ctx_->current_fog->range_start);
		s->SetFloat("FogEnd", ctx_->current_fog->range_end);
		s->SetFloat("FogDensity", ctx_->current_fog->density);
		s->SetVec3("TintColor", glm::vec3(1.f));

		if (IsBuiltinMeshShader(mat->shader_))
		{
			// Defaults of the built-in FBX material reconstruction uniforms.
			s->SetFloat("BaseColorAlpha", 1.f);
			s->SetFloat("RoughnessValue", 1.f);
			s->SetFloat("MetallicValue", 0.f);
			s->SetFloat("SpecularValue", 1.f);
			s->SetFloat("OpacityValue", 1.f);
			s->SetInt("RoughnessUseValue", 0);
			s->SetInt("MetallicUseValue", 0);
			s->SetInt("SpecularUseValue", 0);
			s->SetInt("OpacityUseValue", 0);
			s->SetInt("RoughnessInvert", 0);
			s->SetInt("AlphaInvert", 0);
		}
	}

	if (materialChanged)
	{
		// Values left by the previous material on this shader must not leak.
		s->SetVec3("TintColor", glm::vec3(1.f));
		s->SetInt(HRL_MATERIAL_PARAM_TWO_SIDED, 0);

		for (const auto& [name, value] : mat->intParams_) s->SetInt(name, value);

		static const std::string kTextureKeys[GLES3_MATERIAL_TEXTURE_COUNT] = {
			kTextureUniformNames[0], kTextureUniformNames[1], kTextureUniformNames[2],
			kTextureUniformNames[3], kTextureUniformNames[4], kTextureUniformNames[5]
		};
		static const std::string kAmbientOcclusionKey = HRL_T_AMBIENT_OCCLUSION;

		bool hasPBRMaps = false;
		for (int i = 0; i < GLES3_MATERIAL_TEXTURE_COUNT; ++i)
		{
			s->SetInt(kTextureUniformNames[i], i);
			GLuint glTexture = 0;
			auto itParam = mat->textureParams_.find(kTextureKeys[i]);
			if (itParam != mat->textureParams_.end())
			{
				auto itTexture = bck_->textures.find(itParam->second);
				if (itTexture != bck_->textures.end() && itTexture->second)
				{
					glTexture = itTexture->second->GetGL_ID();
					if (i >= GLES3_NORMAL_INT && i <= GLES3_METALLIC_INT)
						hasPBRMaps = true;
				}
			}
			if (glTexture == 0)
				glTexture = FallbackTextureGL(i);
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, glTexture);
		}

		// Ambient occlusion: white (roughness fallback) when the material has none.
		s->SetInt(HRL_T_AMBIENT_OCCLUSION, GLES3_AO_TEXTURE_UNIT);
		{
			GLuint glTexture = 0;
			auto aoParam = mat->textureParams_.find(kAmbientOcclusionKey);
			if (aoParam != mat->textureParams_.end())
			{
				auto aoTexture = bck_->textures.find(aoParam->second);
				if (aoTexture != bck_->textures.end() && aoTexture->second)
					glTexture = aoTexture->second->GetGL_ID();
			}
			if (glTexture == 0)
				glTexture = FallbackTextureGL(GLES3_ROUGHNESS_INT);
			glActiveTexture(GL_TEXTURE0 + GLES3_AO_TEXTURE_UNIT);
			glBindTexture(GL_TEXTURE_2D, glTexture);
		}
		glActiveTexture(GL_TEXTURE0);

		if (mat->shader_ == HRL_SPRITE_SHADER)
		{
			// See the sprite fragment shader: constant lighting, two texture fetches.
			const bool ambientOnly = bck_->only_sky_lights && !hasPBRMaps;
			s->SetInt("uAmbientOnly", ambientOnly ? 1 : 0);
			s->SetVec3("uAmbientLight", bck_->sky_light_sum * (1.f - bck_->fallback_metallic));
		}

		for (const auto& [name, value] : mat->floatParams_) s->SetFloat(name, value);
		for (const auto& [name, value] : mat->vec2Params_) s->SetVec2(name, value);
		for (const auto& [name, value] : mat->vec3Params_) s->SetVec3(name, value);
		for (const auto& [name, value] : mat->vec4Params_) s->SetVec4(name, value);
		ctx_->bound_material = mat;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Scene passes
// ---------------------------------------------------------------------------

void DrawSkySphere(const hrl_scene_t* scene)
{
	if (!scene || !scene->sky_sphere_enabled || !bck_->sky_shader || bck_->sky_index_count <= 0)
		return;

	// The sphere is centered in view space: removing the camera translation
	// makes it effectively infinitely far away.
	const glm::mat4 skyView = glm::mat4(glm::mat3(ctx_->view_mat));
	glm::mat4 rotation(1.f);
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.x), glm::vec3(1.f, 0.f, 0.f));
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.y), glm::vec3(0.f, 1.f, 0.f));
	rotation = glm::rotate(rotation, glm::radians(scene->sky_rotation.z), glm::vec3(0.f, 0.f, 1.f));

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);

	GLES3_Shader* s = bck_->sky_shader;
	s->Use();
	ctx_->bound_shader = nullptr;
	ctx_->bound_material = nullptr;
	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", skyView);
	s->SetMat4("rotation", rotation);
	s->SetVec3("SkyTopColor", scene->sky_top_color);
	s->SetVec3("SkyHorizonColor", scene->sky_horizon_color);
	s->SetVec3("SkyBottomColor", scene->sky_bottom_color);

	bool hasSkyTexture = false;
	glActiveTexture(GL_TEXTURE0 + GLES3_SKY_TEXTURE_UNIT);
	if (scene->sky_texture != HRL_INVALID_ID)
	{
		auto sky_it = bck_->textures.find(scene->sky_texture);
		if (sky_it != bck_->textures.end() && sky_it->second)
		{
			hasSkyTexture = true;
			glBindTexture(GL_TEXTURE_2D, sky_it->second->GetGL_ID());
		}
	}
	if (!hasSkyTexture)
		glBindTexture(GL_TEXTURE_2D, WhiteTextureGL());
	s->SetInt("SkyTexture", GLES3_SKY_TEXTURE_UNIT);
	s->SetInt("SkyTextureEnabled", hasSkyTexture ? 1 : 0);
	glActiveTexture(GL_TEXTURE0);

	glBindVertexArray(bck_->sky_vao);
	glDrawElements(GL_TRIANGLES, bck_->sky_index_count, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);

	glCullFace(GL_BACK);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);
}

void DrawOpaqueMeshes(const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, const FrustumPlaneSet& frustum)
{
	struct DrawItem
	{
		HRL_Mesh* mesh;
		HRL_Material* material;
		const MeshLOD_GPU* staticGpu;
		SkeletalMeshGPU* skeletalGpu;
		glm::mat4 model;
		float distance2;
	};

	static std::vector<DrawItem> visible;
	visible.clear();
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;

	for (const auto& [id, mesh] : meshes)
	{
		if (!mesh || mesh->type_ == HRL_SPRITE || !IsFiniteBounds(mesh))
			continue;

		const glm::mat4 model = CalculateModelMatrix(mesh);
		if (mesh->type_ != HRL_3D_SKELETAL_MESH && !IsMeshVisible(mesh, model, frustum))
			continue;

		auto materialIt = GetPrivateContext()->materials.find(mesh->material_);
		if (materialIt == GetPrivateContext()->materials.end() || !materialIt->second)
			continue;

		SkeletalMeshGPU* skeletalGpu = nullptr;
		const MeshLOD_GPU* staticGpu = nullptr;

		if (mesh->type_ == HRL_3D_SKELETAL_MESH)
		{
			auto gpuIt = bck_->skeletal_meshes.find(id);
			if (gpuIt == bck_->skeletal_meshes.end() || gpuIt->second.vao == 0)
				continue;
			skeletalGpu = &gpuIt->second;
			UploadSkeletalBones(static_cast<HRL_SkeletalMesh*>(mesh), *skeletalGpu);
		}
		else
		{
			auto gpuIt = bck_->meshes.find(id);
			if (gpuIt == bck_->meshes.end())
				continue;
			const int lodLevel = SelectMeshLOD(mesh, model, ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight());
			staticGpu = GetMeshLOD_GPU(gpuIt->second, lodLevel);
			if (!staticGpu)
				continue;
			mesh->last_lod_level_ = lodLevel;
		}

		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
		visible.push_back({mesh, materialIt->second, staticGpu, skeletalGpu, model, glm::dot(center - cameraPos, center - cameraPos)});
	}

	if (visible.empty())
		return;

	// Sorted by shader, material, geometry, then front to back.
	std::sort(visible.begin(), visible.end(), [](const DrawItem& a, const DrawItem& b)
	{
		if (a.material->shader_ != b.material->shader_)
			return a.material->shader_ < b.material->shader_;
		if (a.mesh->material_ != b.mesh->material_)
			return a.mesh->material_ < b.mesh->material_;
		const GLuint vaoA = a.staticGpu ? a.staticGpu->vao : a.skeletalGpu->vao;
		const GLuint vaoB = b.staticGpu ? b.staticGpu->vao : b.skeletalGpu->vao;
		if (vaoA != vaoB)
			return vaoA < vaoB;
		return a.distance2 < b.distance2;
	});

	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	bool cullEnabled = true;

	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;

	for (const DrawItem& item : visible)
	{
		const bool cull = !MaterialIsTwoSided(item.material);
		if (cull != cullEnabled)
		{
			if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
			cullEnabled = cull;
		}
		if (!BindMaterial(item.material, item.mesh, item.model))
			continue;

		if (item.skeletalGpu)
		{
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, item.skeletalGpu->bone_ubo);
			glBindVertexArray(item.skeletalGpu->vao);
			if (item.skeletalGpu->indexed)
				glDrawElements(GL_TRIANGLES, item.skeletalGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, item.skeletalGpu->vertex_count);
		}
		else
		{
			glBindVertexArray(item.staticGpu->vao);
			if (item.staticGpu->indexed)
				glDrawElements(GL_TRIANGLES, item.staticGpu->index_count, GL_UNSIGNED_INT, nullptr);
			else
				glDrawArrays(GL_TRIANGLES, 0, item.staticGpu->vertex_count);
		}
	}

	glBindVertexArray(0);
	glDisable(GL_CULL_FACE);
}

// Texture units a sprite material can sample from (see BindMaterial).
void BindSpriteSamplers(GLuint sampler)
{
	for (GLuint unit = 0; unit < GLES3_MATERIAL_TEXTURE_COUNT; ++unit)
		glBindSampler(unit, sampler);
	glBindSampler((GLuint)GLES3_AO_TEXTURE_UNIT, sampler);
}

void DrawSprites(const std::unordered_map<HRL_id, HRL_Mesh*>& meshes, const FrustumPlaneSet& frustum)
{
	struct SpriteItem { HRL_Mesh* mesh; HRL_Material* material; glm::mat4 model; float distance2; };
	static std::vector<SpriteItem> sprites;
	sprites.clear();
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;
	for (const auto& [id, mesh] : meshes)
	{
		(void)id;
		if (!mesh || mesh->type_ != HRL_SPRITE || mesh->material_ == HRL_INVALID_ID) continue;
		const glm::mat4 model = CalculateModelMatrix(mesh);
		if (!IsMeshVisible(mesh, model, frustum)) continue;
		auto matIt = GetPrivateContext()->materials.find(mesh->material_);
		if (matIt == GetPrivateContext()->materials.end() || !matIt->second) continue;
		const glm::vec3 center = glm::vec3(model * glm::vec4(mesh->bounds_center_, 1.f));
		sprites.push_back({mesh, matIt->second, model, glm::dot(center - cameraPos, center - cameraPos)});
	}
	if (sprites.empty())
		return;

	// Back to front, then by draw order (same rule as the desktop backend).
	std::stable_sort(sprites.begin(), sprites.end(), [](const SpriteItem& a, const SpriteItem& b)
	{
		if (std::abs(a.distance2 - b.distance2) > 1e-6f) return a.distance2 > b.distance2;
		return a.mesh->draw_order_ < b.mesh->draw_order_;
	});

	// Sprites are transparent: they test against the depth written by opaque
	// geometry but never write depth themselves.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	ctx_->bound_material = nullptr;
	ctx_->bound_shader = nullptr;

	BindSpriteSamplers(bck_->nearest_sampler);

	static std::vector<SpriteBatchInstance> instances;

	size_t i = 0;
	while (i < sprites.size())
	{
		const SpriteItem& first = sprites[i];
		if (first.material->shader_ != HRL_SPRITE_SHADER)
		{
			// Custom shader: one draw per sprite, non-instanced attributes only.
			if (BindMaterial(first.material, first.mesh, first.model))
			{
				glBindVertexArray(bck_->sprite_vao);
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
			}
			++i;
			continue;
		}

		size_t end = i + 1;
		while (end < sprites.size() && sprites[end].material == first.material && (end - i) < GLES3_MAX_SPRITE_BATCH_INSTANCES) ++end;
		instances.clear();
		instances.reserve(end - i);
		for (size_t j = i; j < end; ++j)
		{
			const HRL_Mesh* m = sprites[j].mesh;
			instances.push_back({sprites[j].model, glm::vec4(m->region_[0], m->region_[1], m->region_[2], m->region_[3])});
		}
		if (BindMaterial(first.material, first.mesh, first.model))
		{
			ctx_->shader->SetInt("uInstanced", 1);
			const size_t required = instances.size() * sizeof(SpriteBatchInstance);
			glBindBuffer(GL_ARRAY_BUFFER, bck_->sprite_instance_vbo);
			if (required > bck_->sprite_instance_capacity)
				bck_->sprite_instance_capacity = required * 2u;
			// Orphan the previous storage so the GPU never waits on the last batch.
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->sprite_instance_capacity, nullptr, GL_STREAM_DRAW);
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)required, instances.data());
			glBindVertexArray(bck_->sprite_instanced_vao);
			glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, (GLsizei)instances.size());
		}
		i = end;
	}

	// Restore the textures' own filtering for everything drawn after sprites.
	BindSpriteSamplers(0);

	glBindVertexArray(0);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

GLuint ResolveVFXTexture(const HRL_VFXEmitter* emitter)
{
	if (!emitter) return 0;
	HRL_id textureId = emitter->texture_;
	if (textureId == HRL_INVALID_ID && emitter->material_ != HRL_INVALID_ID)
	{
		auto matIt = GetPrivateContext()->materials.find(emitter->material_);
		if (matIt != GetPrivateContext()->materials.end() && matIt->second && !matIt->second->textureParams_.empty())
			textureId = matIt->second->textureParams_.begin()->second;
	}
	if (textureId != HRL_INVALID_ID)
	{
		auto texIt = bck_->textures.find(textureId);
		if (texIt != bck_->textures.end() && texIt->second)
			return texIt->second->GetGL_ID();
	}
	return FallbackTextureGL(GLES3_ALBEDO_INT);
}

void SetVFXBlendMode(HRL_EVFXBlendMode mode)
{
	switch (mode)
	{
	case HRL_VFX_BLEND_ADDITIVE: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
	case HRL_VFX_BLEND_MULTIPLY: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;
	case HRL_VFX_BLEND_ALPHA:
	default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
	}
}

void DrawVFX(const hrl_scene_t* scene)
{
	if (!scene || scene->vfx_systems.empty() || !bck_->vfx_shader)
		return;

	struct Batch
	{
		HRL_EVFXBlendMode blend = HRL_VFX_BLEND_ALPHA;
		GLuint texture = 0;
		std::vector<VFXRenderInstance> instances;
	};

	// Persistent buffers: no per-frame allocation.
	static std::vector<Batch> batches;
	size_t batchCount = 0;
	static std::vector<VFXRenderInstance> unsorted;
	static std::vector<std::pair<float, uint32_t>> order;
	const glm::mat4 inverseView = glm::inverse(ctx_->view_mat);
	const glm::mat4 billboardBasis = glm::mat4(glm::mat3(inverseView));
	const glm::vec3 cameraPos = ctx_->viewport->camera_->position_;
	bool hasMeshEmitters = false;

	for (const auto& [sid, system] : scene->vfx_systems)
	{
		(void)sid;
		if (!system || !system->enabled_) continue;
		const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);
		const bool systemIsIdentity = systemMatrix == glm::mat4(1.f);
		for (const auto& [eid, emitter] : system->emitters_)
		{
			(void)eid;
			if (!emitter || !emitter->enabled_ || emitter->particles_.empty()) continue;
			if (emitter->render_mode_ == HRL_VFX_RENDER_MESH)
			{
				hasMeshEmitters = true;
				continue;
			}
			const glm::mat4* particleSystemMatrix =
				(emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL && !systemIsIdentity) ? &systemMatrix : nullptr;
			const bool stretched = emitter->render_mode_ == HRL_VFX_RENDER_STRETCHED_BILLBOARD;

			if (batchCount == batches.size())
				batches.emplace_back();
			Batch& batch = batches[batchCount];
			batch.blend = emitter->blend_mode_;
			batch.texture = ResolveVFXTexture(emitter);
			batch.instances.clear();
			batch.instances.reserve(emitter->particles_.size());

			if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
			{
				// Alpha blending needs back-to-front particles.
				unsorted.clear();
				order.clear();
				for (const auto& particle : emitter->particles_)
				{
					VFXRenderInstance inst;
					inst.model = MakeVFXParticleModel(particle, particleSystemMatrix, billboardBasis, stretched, emitter->stretch_);
					inst.color = particle.color;
					const glm::vec3 pos = glm::vec3(inst.model[3]);
					order.emplace_back(glm::dot(pos - cameraPos, pos - cameraPos), static_cast<uint32_t>(unsorted.size()));
					unsorted.push_back(inst);
				}
				std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				for (const auto& entry : order) batch.instances.push_back(unsorted[entry.second]);
			}
			else
			{
				for (const auto& particle : emitter->particles_)
				{
					VFXRenderInstance inst;
					inst.model = MakeVFXParticleModel(particle, particleSystemMatrix, billboardBasis, stretched, emitter->stretch_);
					inst.color = particle.color;
					batch.instances.push_back(inst);
				}
			}
			if (!batch.instances.empty()) ++batchCount;
		}
	}

	if (batchCount == 0 && !hasMeshEmitters)
		return;

	// VFX depth policy: tested against the opaque scene, never written.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);

	if (batchCount > 0)
	{
		GLES3_Shader* s = bck_->vfx_shader;
		s->Use();
		ctx_->bound_shader = nullptr;
		ctx_->bound_material = nullptr;
		s->SetMat4("projection", ctx_->proj_mat);
		s->SetMat4("view", ctx_->view_mat);
		s->SetInt("uTexture", 0);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(bck_->vfx_vao);

		for (size_t batchIndex = 0; batchIndex < batchCount; ++batchIndex)
		{
			const Batch& batch = batches[batchIndex];
			SetVFXBlendMode(batch.blend);
			glBindTexture(GL_TEXTURE_2D, batch.texture != 0 ? batch.texture : WhiteTextureGL());
			s->SetInt("uUseTexture", batch.texture != 0 ? 1 : 0);

			const size_t required = batch.instances.size() * sizeof(VFXRenderInstance);
			glBindBuffer(GL_ARRAY_BUFFER, bck_->vfx_instance_vbo);
			if (required > bck_->vfx_instance_capacity)
				bck_->vfx_instance_capacity = required * 2u;
			glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->vfx_instance_capacity, nullptr, GL_STREAM_DRAW);
			glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)required, batch.instances.data());
			glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, (GLsizei)batch.instances.size());
		}
	}

	// Mesh particles use their HRL static mesh as a template: each particle gets
	// its own transform while reusing the mesh material (or the emitter's one).
	if (hasMeshEmitters)
	{
		ctx_->bound_material = nullptr;
		ctx_->bound_shader = nullptr;

		for (const auto& [sid, system] : scene->vfx_systems)
		{
			(void)sid;
			if (!system || !system->enabled_) continue;
			const glm::mat4 systemMatrix = MakeVFXSystemMatrix(system);

			for (const auto& [eid, emitter] : system->emitters_)
			{
				(void)eid;
				if (!emitter || !emitter->enabled_ ||
					emitter->render_mode_ != HRL_VFX_RENDER_MESH ||
					emitter->mesh_ == HRL_INVALID_ID ||
					emitter->particles_.empty())
					continue;

				auto meshIt = GetPrivateContext()->meshes.find(emitter->mesh_);
				if (meshIt == GetPrivateContext()->meshes.end() || !meshIt->second ||
					meshIt->second->type_ != HRL_3D_MESH)
					continue;

				auto gpuIt = bck_->meshes.find(emitter->mesh_);
				if (gpuIt == bck_->meshes.end()) continue;
				const HRL_Mesh& mesh = *meshIt->second;
				const int lodLevel = SelectMeshLOD(meshIt->second, CalculateModelMatrix(&mesh),
					ctx_->view_mat, ctx_->proj_mat, (float)GetWindowHeight());
				const MeshLOD_GPU* gpu = GetMeshLOD_GPU(gpuIt->second, lodLevel);
				if (!gpu || gpu->vao == 0 || gpu->vertex_count <= 0) continue;

				HRL_Material* material = nullptr;
				if (emitter->material_ != HRL_INVALID_ID)
				{
					auto matIt = GetPrivateContext()->materials.find(emitter->material_);
					if (matIt != GetPrivateContext()->materials.end() && matIt->second &&
						matIt->second->shader_ == HRL_MESH_3D_SHADER)
						material = matIt->second;
				}
				if (!material && mesh.material_ != HRL_INVALID_ID)
				{
					auto matIt = GetPrivateContext()->materials.find(mesh.material_);
					if (matIt != GetPrivateContext()->materials.end()) material = matIt->second;
				}
				if (!material || material->shader_ != HRL_MESH_3D_SHADER)
					continue;

				static std::vector<std::pair<float, const HRL_VFXParticle*>> ordered;
				ordered.clear();
				ordered.reserve(emitter->particles_.size());
				for (const auto& particle : emitter->particles_)
				{
					glm::vec3 worldPos = particle.position;
					if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
						worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos, 1.f));
					const glm::vec3 d = worldPos - cameraPos;
					ordered.emplace_back(glm::dot(d, d), &particle);
				}
				if (emitter->blend_mode_ == HRL_VFX_BLEND_ALPHA)
					std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

				SetVFXBlendMode(emitter->blend_mode_);

				GLuint overrideTexture = 0;
				if (emitter->texture_ != HRL_INVALID_ID)
				{
					auto texIt = bck_->textures.find(emitter->texture_);
					if (texIt != bck_->textures.end() && texIt->second)
						overrideTexture = texIt->second->GetGL_ID();
				}

				bool emitterStateBound = false;
				for (const auto& entry : ordered)
				{
					const HRL_VFXParticle& particle = *entry.second;
					glm::vec3 worldPos = particle.position;
					if (emitter->simulation_space_ == HRL_VFX_SIMULATION_LOCAL)
						worldPos = glm::vec3(systemMatrix * glm::vec4(worldPos, 1.f));

					glm::mat4 model(1.f);
					model = glm::translate(model, worldPos);
					model = glm::translate(model, mesh.pivot_point_);
					model = glm::rotate(model, glm::radians(particle.rotation.x + emitter->mesh_rotation_offset_.x), glm::vec3(1.f, 0.f, 0.f));
					model = glm::rotate(model, glm::radians(particle.rotation.y + emitter->mesh_rotation_offset_.y), glm::vec3(0.f, 1.f, 0.f));
					model = glm::rotate(model, glm::radians(particle.rotation.z + emitter->mesh_rotation_offset_.z), glm::vec3(0.f, 0.f, 1.f));
					model = glm::translate(model, -mesh.pivot_point_);
					model = glm::scale(model, emitter->mesh_scale_ * std::max(0.f, particle.size.x));

					if (!BindMaterial(material, &mesh, model)) continue;
					ctx_->shader->SetVec3("TintColor", glm::vec3(particle.color));

					if (!emitterStateBound)
					{
						if (overrideTexture != 0)
						{
							glActiveTexture(GL_TEXTURE0);
							glBindTexture(GL_TEXTURE_2D, overrideTexture);
						}
						glBindVertexArray(gpu->vao);
						emitterStateBound = true;
					}
					if (gpu->indexed && gpu->index_count > 0) glDrawElements(GL_TRIANGLES, gpu->index_count, GL_UNSIGNED_INT, nullptr);
					else glDrawArrays(GL_TRIANGLES, 0, gpu->vertex_count);
				}
				// The override replaced the material texture on unit 0, and the tint
				// was changed per particle: force a full rebind for the next user.
				ctx_->bound_material = nullptr;
			}
		}
	}

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glBindVertexArray(0);
}

void DrawDebug(const DebugRenderer& renderer, float line_thickness)
{
	if (renderer.lines.empty() && renderer.triangles.empty())
		return;

	auto it = bck_->shaders.find(HRL_DEBUG_SHADER);
	if (it == bck_->shaders.end())
		return;
	auto* s = it->second;

	// Debug primitives are an overlay: never hidden by the scene geometry.
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	s->Use();
	ctx_->shader = s;
	ctx_->bound_shader = nullptr;
	ctx_->bound_material = nullptr;
	s->SetMat4("projection", ctx_->proj_mat);
	s->SetMat4("view", ctx_->view_mat);

	glBindVertexArray(bck_->debug_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->debug_vbo);
	// Wide lines are optional in OpenGL ES; drivers clamp to what they support.
	glLineWidth(std::max(1.0f, line_thickness));

	const auto draw = [](GLenum mode, const std::vector<DebugVertex>& vertices)
	{
		if (vertices.empty())
			return;
		const size_t size = vertices.size() * sizeof(DebugVertex);
		if (size > bck_->debug_vbo_capacity)
			bck_->debug_vbo_capacity = size * 2u;
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->debug_vbo_capacity, nullptr, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)size, vertices.data());
		glDrawArrays(mode, 0, (GLsizei)vertices.size());
	};
	draw(GL_LINES, renderer.lines);
	draw(GL_TRIANGLES, renderer.triangles);

	glBindVertexArray(0);
	glLineWidth(1.0f);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// UI pass (widgets, text, screen messages)
// ---------------------------------------------------------------------------

// Quads are accumulated on the CPU and uploaded once per pass (6 vertices of
// x, y, u, v each), instead of rewriting the vertex buffer for every widget.
std::vector<UIQuad> ui_quads_;
std::vector<float> ui_vertices_;

void PushUIQuad(const UIQuad& quad, float x, float y, float w, float h, float u0, float u1, float vTop, float vBottom)
{
	const float v[24] = {
		x,     y,     u0, vTop,
		x + w, y,     u1, vTop,
		x + w, y + h, u1, vBottom,
		x + w, y + h, u1, vBottom,
		x,     y + h, u0, vBottom,
		x,     y,     u0, vTop,
	};
	ui_vertices_.insert(ui_vertices_.end(), v, v + 24);
	ui_quads_.push_back(quad);
}

// Draws the accumulated quads in the given viewport rectangle (pixels).
void FlushUIQuads(int viewportX, int viewportY, int viewportW, int viewportH)
{
	if (ui_quads_.empty() || !bck_->ui_shader)
	{
		ui_quads_.clear();
		ui_vertices_.clear();
		return;
	}

	glViewport(viewportX, viewportY, viewportW, viewportH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	GLES3_Shader* s = bck_->ui_shader;
	s->Use();
	ctx_->bound_shader = nullptr;
	ctx_->bound_material = nullptr;
	// Positions are normalized to the viewport, origin at the top-left corner.
	s->SetMat4("projection", glm::ortho(0.f, 1.f, 1.f, 0.f, -1.f, 1.f));
	s->SetInt("uTexture", 0);
	glActiveTexture(GL_TEXTURE0);

	glBindVertexArray(bck_->ui_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->ui_vbo);
	const size_t required = ui_vertices_.size() * sizeof(float);
	if (required > bck_->ui_vbo_capacity)
		bck_->ui_vbo_capacity = required * 2u;
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->ui_vbo_capacity, nullptr, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)required, ui_vertices_.data());

	GLuint boundTexture = 0;
	for (size_t i = 0; i < ui_quads_.size(); ++i)
	{
		const UIQuad& quad = ui_quads_[i];
		GLuint glTexture = 0;
		if (quad.texture != HRL_INVALID_ID)
		{
			// The texture may have been deleted by a widget callback since the
			// quad was queued: skip it rather than drawing a white rectangle.
			auto texIt = bck_->textures.find(quad.texture);
			if (texIt == bck_->textures.end() || !texIt->second)
				continue;
			glTexture = texIt->second->GetGL_ID();
		}
		else
			glTexture = WhiteTextureGL();

		if (glTexture != boundTexture)
		{
			glBindTexture(GL_TEXTURE_2D, glTexture);
			boundTexture = glTexture;
		}
		s->SetVec4("uTintColor", quad.tint);
		s->SetInt("uSDFText", quad.sdf ? 1 : 0);
		glDrawArrays(GL_TRIANGLES, (GLint)(i * 6u), 6);
	}

	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	ui_quads_.clear();
	ui_vertices_.clear();
}

void DrawScreenMessages(const hrl_scene_t* scene, const HRL_Viewport* viewport)
{
	HRL_Context* privateContext = GetPrivateContext();

	// Release the cached textures of the messages that expired.
	if (!bck_->screen_message_textures.empty())
	{
		std::unordered_set<HRL_id> activeIds;
		for (const auto& [sceneId, otherScene] : privateContext->scenes)
		{
			(void)sceneId;
			if (!otherScene) continue;
			for (const HRL_ScreenMessage& message : otherScene->screen_messages)
				activeIds.insert(message.id);
		}
		for (auto it = bck_->screen_message_textures.begin(); it != bck_->screen_message_textures.end(); )
		{
			if (activeIds.find(it->first) == activeIds.end())
			{
				if (it->second.texture != HRL_INVALID_ID && HRL_IsValidTexture(it->second.texture))
					HRL_DeleteTexture(it->second.texture);
				it = bck_->screen_message_textures.erase(it);
			}
			else
				++it;
		}
	}

	if (!scene || !viewport || scene->screen_messages.empty() || privateContext->fonts.empty())
		return;

	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	const int viewportX = static_cast<int>(viewport->x_ * winW);
	const int viewportY = static_cast<int>(viewport->y_ * winH);
	const int viewportW = std::max(1, static_cast<int>(viewport->width_ * winW));
	const int viewportH = std::max(1, static_cast<int>(viewport->height_ * winH));

	// Unreal-style stack: newest messages are appended below older ones.
	float cursorY = 16.0f;
	const float leftMargin = 16.0f;
	const float lineGap = 4.0f;

	for (const HRL_ScreenMessage& message : scene->screen_messages)
	{
		HRL_id fontId = message.font;
		if (fontId == HRL_INVALID_ID || privateContext->fonts.find(fontId) == privateContext->fonts.end())
			fontId = privateContext->fonts.begin()->first;

		auto& cache = bck_->screen_message_textures[message.id];
		if (cache.texture == HRL_INVALID_ID ||
			cache.font != fontId ||
			cache.size != message.size ||
			cache.text != message.text ||
			!HRL_IsValidTexture(cache.texture))
		{
			if (cache.texture != HRL_INVALID_ID && HRL_IsValidTexture(cache.texture))
				HRL_DeleteTexture(cache.texture);

			cache = {};
			cache.font = fontId;
			cache.size = message.size;
			cache.text = message.text;
			cache.texture = HRL_InternalCreateSDFTextTexture(message.text.c_str(), fontId, message.size);
			if (cache.texture == HRL_INVALID_ID)
				continue;
			HRL_GetTextureSize(cache.texture, &cache.width, &cache.height);
		}

		if (cache.texture == HRL_INVALID_ID || cache.width <= 0 || cache.height <= 0)
			continue;

		const float lineHeight = std::max(1.0f, message.size);
		const float lineCount = 1.0f + static_cast<float>(std::count(message.text.begin(), message.text.end(), '\n'));
		const float pixelHeight = lineHeight * lineCount;
		const float pixelWidth = pixelHeight * static_cast<float>(cache.width) / static_cast<float>(cache.height);

		const float x = leftMargin / static_cast<float>(viewportW);
		const float y = cursorY / static_cast<float>(viewportH);
		const float w = pixelWidth / static_cast<float>(viewportW);
		const float h = pixelHeight / static_cast<float>(viewportH);
		const float shadowX = 1.5f / static_cast<float>(viewportW);
		const float shadowY = 1.5f / static_cast<float>(viewportH);

		// A dark translucent shadow keeps messages readable over the scene.
		UIQuad quad;
		quad.texture = cache.texture;
		quad.sdf = true;
		quad.tint = glm::vec4(0.f, 0.f, 0.f, message.color.a * 0.75f);
		PushUIQuad(quad, x + shadowX, y + shadowY, w, h, 0.f, 1.f, 1.f, 0.f);
		quad.tint = message.color;
		PushUIQuad(quad, x, y, w, h, 0.f, 1.f, 1.f, 0.f);

		cursorY += pixelHeight + lineGap;
	}

	FlushUIQuads(viewportX, viewportY, viewportW, viewportH);
}

void DrawWidgets(const std::unordered_map<HRL_id, HRL_Widget*>& widgets, const HRL_Viewport* viewport)
{
	if (!viewport || widgets.empty())
		return;

	const float winW = static_cast<float>(GetWindowWidth());
	const float winH = static_cast<float>(GetWindowHeight());
	const int viewportX = static_cast<int>(viewport->x_ * winW);
	const int viewportY = static_cast<int>(viewport->y_ * winH);
	const int viewportW = std::max(1, static_cast<int>(viewport->width_ * winW));
	const int viewportH = std::max(1, static_cast<int>(viewport->height_ * winH));

	static std::vector<HRL_Widget*> sortedWidgets;
	sortedWidgets.clear();
	sortedWidgets.reserve(widgets.size());
	for (const auto& [id, widget] : widgets)
	{
		(void)id;
		if (widget)
			sortedWidgets.push_back(widget);
	}
	std::sort(sortedWidgets.begin(), sortedWidgets.end(), [](const HRL_Widget* a, const HRL_Widget* b) {
		if (a->GetZIndex() != b->GetZIndex())
			return a->GetZIndex() < b->GetZIndex();
		return a->GetId() < b->GetId();
	});

	HRL_Context* privateContext = GetPrivateContext();
	const float mouseX = privateContext->mouseX;
	const float mouseY = privateContext->mouseY;
	const float vpX = static_cast<float>(viewportX);
	const float vpY = static_cast<float>(viewportY);
	const float vpW = static_cast<float>(viewportW);
	const float vpH = static_cast<float>(viewportH);

	// First update hover state for every widget without dispatching the mouse button.
	for (HRL_Widget* w : sortedWidgets)
		w->UpdateInput(mouseX, mouseY, false, false, false, vpX, vpY, vpW, vpH);

	// A press is captured by the highest-z interactive widget under the cursor.
	if (privateContext->mouseLeftPressed && privateContext->mouseCaptureWidget == HRL_INVALID_ID && privateContext->mouseCaptureGizmo == HRL_INVALID_ID)
	{
		for (auto it = sortedWidgets.rbegin(); it != sortedWidgets.rend(); ++it)
		{
			HRL_Widget* w = *it;
			if (w->IsPointerInteractive() && w->IsVisible() && w->IsEnabled() && w->IsHovered())
			{
				privateContext->mouseCaptureWidget = w->GetId();
				break;
			}
		}
	}

	static std::vector<HRL_Widget::WidgetDrawInfos> geometries;
	for (HRL_Widget* w : sortedWidgets)
	{
		const bool isCapture = (w->GetId() == privateContext->mouseCaptureWidget);
		w->UpdateInput(
			mouseX, mouseY,
			isCapture ? privateContext->mouseLeftDown : false,
			isCapture ? privateContext->mouseLeftPressed : false,
			isCapture ? privateContext->mouseLeftReleased : false,
			vpX, vpY, vpW, vpH);
		w->Logic();

		geometries.clear();
		w->GetDrawInfos(geometries);

		// A world-projected widget keeps its regular 2D geometry, but its anchor
		// point is replaced by the 3D point projected through the viewport camera.
		if (w->IsWorldPositionEnabled())
		{
			if (!viewport->camera_)
				continue;
			const glm::vec4 clip = ctx_->proj_mat * ctx_->view_mat * glm::vec4(w->GetWorldPosition(), 1.0f);
			if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.w) || clip.w <= 1e-6f)
				continue;
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y))
				continue;
			const glm::vec2 projected(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y);
			const glm::vec2 delta = projected - w->GetPosition();
			for (auto& g : geometries)
			{
				g.px += delta.x;
				g.py += delta.y;
			}
		}

		for (const auto& g : geometries)
		{
			if (g.sx <= 0.0f || g.sy <= 0.0f || g.a <= 0.0f)
				continue;

			UIQuad quad;
			// No texture (progress bar, slider, checkbox, plain colors): white texel.
			quad.texture = (bck_->textures.find(g.texture) != bck_->textures.end()) ? g.texture : HRL_INVALID_ID;
			quad.tint = glm::vec4(g.r, g.g, g.b, g.a);
			quad.sdf = g.sdf;
			// UV: top of the quad = v 1 (texture convention of the UI).
			// u0/t0/u1/t1 crop the image (text clipped by its widget).
			PushUIQuad(quad, g.px, g.py, g.sx, g.sy, g.u0, g.u1, 1.0f - g.t0, 1.0f - g.t1);
		}
	}

	FlushUIQuads(viewportX, viewportY, viewportW, viewportH);

	if (privateContext->mouseLeftReleased)
		privateContext->mouseCaptureWidget = HRL_INVALID_ID;
}

} // namespace

// ===========================================================================
// GL loader
// ===========================================================================

bool GLES3_LoadGL(void* loader)
{
	if (!loader)
		return false;
	const GLADloadproc load = reinterpret_cast<GLADloadproc>(loader);
	const char* missing = nullptr;

	// Every entry point used by this backend. All of them are OpenGL ES 3.0 core.
#define HRL_GLES3_LOAD(name) \
	do { \
		glad_##name = reinterpret_cast<decltype(glad_##name)>(load(#name)); \
		if (!glad_##name && !missing) missing = #name; \
	} while (0)

	HRL_GLES3_LOAD(glActiveTexture);
	HRL_GLES3_LOAD(glAttachShader);
	HRL_GLES3_LOAD(glBindBuffer);
	HRL_GLES3_LOAD(glBindBufferBase);
	HRL_GLES3_LOAD(glBindFramebuffer);
	HRL_GLES3_LOAD(glBindRenderbuffer);
	HRL_GLES3_LOAD(glBindSampler);
	HRL_GLES3_LOAD(glBindTexture);
	HRL_GLES3_LOAD(glBindVertexArray);
	HRL_GLES3_LOAD(glBlendFunc);
	HRL_GLES3_LOAD(glBufferData);
	HRL_GLES3_LOAD(glBufferSubData);
	HRL_GLES3_LOAD(glCheckFramebufferStatus);
	HRL_GLES3_LOAD(glClear);
	HRL_GLES3_LOAD(glClearColor);
	HRL_GLES3_LOAD(glColorMask);
	HRL_GLES3_LOAD(glCompileShader);
	HRL_GLES3_LOAD(glCreateProgram);
	HRL_GLES3_LOAD(glCreateShader);
	HRL_GLES3_LOAD(glCullFace);
	HRL_GLES3_LOAD(glDeleteBuffers);
	HRL_GLES3_LOAD(glDeleteFramebuffers);
	HRL_GLES3_LOAD(glDeleteProgram);
	HRL_GLES3_LOAD(glDeleteRenderbuffers);
	HRL_GLES3_LOAD(glDeleteSamplers);
	HRL_GLES3_LOAD(glDeleteShader);
	HRL_GLES3_LOAD(glDeleteTextures);
	HRL_GLES3_LOAD(glDeleteVertexArrays);
	HRL_GLES3_LOAD(glDepthFunc);
	HRL_GLES3_LOAD(glDepthMask);
	HRL_GLES3_LOAD(glDisable);
	HRL_GLES3_LOAD(glDrawArrays);
	HRL_GLES3_LOAD(glDrawElements);
	HRL_GLES3_LOAD(glDrawElementsInstanced);
	HRL_GLES3_LOAD(glEnable);
	HRL_GLES3_LOAD(glEnableVertexAttribArray);
	HRL_GLES3_LOAD(glFramebufferRenderbuffer);
	HRL_GLES3_LOAD(glFramebufferTexture2D);
	HRL_GLES3_LOAD(glGenBuffers);
	HRL_GLES3_LOAD(glGenFramebuffers);
	HRL_GLES3_LOAD(glGenRenderbuffers);
	HRL_GLES3_LOAD(glGenSamplers);
	HRL_GLES3_LOAD(glGenTextures);
	HRL_GLES3_LOAD(glGenVertexArrays);
	HRL_GLES3_LOAD(glGenerateMipmap);
	HRL_GLES3_LOAD(glGetProgramInfoLog);
	HRL_GLES3_LOAD(glGetProgramiv);
	HRL_GLES3_LOAD(glGetShaderInfoLog);
	HRL_GLES3_LOAD(glGetShaderiv);
	HRL_GLES3_LOAD(glGetUniformBlockIndex);
	HRL_GLES3_LOAD(glGetUniformLocation);
	HRL_GLES3_LOAD(glLineWidth);
	HRL_GLES3_LOAD(glLinkProgram);
	HRL_GLES3_LOAD(glPixelStorei);
	HRL_GLES3_LOAD(glReadBuffer);
	HRL_GLES3_LOAD(glReadPixels);
	HRL_GLES3_LOAD(glRenderbufferStorage);
	HRL_GLES3_LOAD(glSamplerParameteri);
	HRL_GLES3_LOAD(glShaderSource);
	HRL_GLES3_LOAD(glTexImage2D);
	HRL_GLES3_LOAD(glTexParameteri);
	HRL_GLES3_LOAD(glUniform1f);
	HRL_GLES3_LOAD(glUniform1i);
	HRL_GLES3_LOAD(glUniform1ui);
	HRL_GLES3_LOAD(glUniform2f);
	HRL_GLES3_LOAD(glUniform3f);
	HRL_GLES3_LOAD(glUniform4f);
	HRL_GLES3_LOAD(glUniformBlockBinding);
	HRL_GLES3_LOAD(glUniformMatrix3fv);
	HRL_GLES3_LOAD(glUniformMatrix4fv);
	HRL_GLES3_LOAD(glUseProgram);
	HRL_GLES3_LOAD(glVertexAttribDivisor);
	HRL_GLES3_LOAD(glVertexAttribIPointer);
	HRL_GLES3_LOAD(glVertexAttribPointer);
	HRL_GLES3_LOAD(glViewport);

#undef HRL_GLES3_LOAD

	if (missing)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL,
			std::string("GLES3: OpenGL ES 3.0 function not available: ") + missing);
		return false;
	}
	return true;
}

// ===========================================================================
// Control
// ===========================================================================

void GLES3_Init()
{
}

void GLES3_InitContext(HRL_uint _width, HRL_uint _height, void* loader)
{
	(void)_width;
	(void)_height;
	if (!GLES3_LoadGL(loader))
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_FATAL,
			"GLES3_InitContext: failed to load OpenGL ES 3.0 (is an ES 3 context current, and was a loader passed to HRL_InitContext?)");
		return;
	}

	bck_ = new GLES3_Backend();
	ctx_ = new GLES3_State();

	//UI (dynamic quads: x, y, u, v)
	glGenVertexArrays(1, &bck_->ui_vao);
	glGenBuffers(1, &bck_->ui_vbo);
	glBindVertexArray(bck_->ui_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->ui_vbo);
	bck_->ui_vbo_capacity = 64u * 24u * sizeof(float);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bck_->ui_vbo_capacity, nullptr, GL_STREAM_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);

	//DEBUG (position, color)
	glGenVertexArrays(1, &bck_->debug_vao);
	glGenBuffers(1, &bck_->debug_vbo);
	glBindVertexArray(bck_->debug_vao);
	glBindBuffer(GL_ARRAY_BUFFER, bck_->debug_vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(1);
	glBindVertexArray(0);

	//SHADERS
	RegisterBuiltinShader(HRL_SPRITE_SHADER, gles3_shaders::kSpriteVertex, gles3_shaders::kSpriteFragment);
	RegisterBuiltinShader(HRL_MESH_3D_SHADER, gles3_shaders::kMeshVertex, gles3_shaders::kMeshFragment);
	RegisterBuiltinShader(HRL_SKINNED_3D_MESH_SHADER, gles3_shaders::kSkinnedMeshVertex, gles3_shaders::kMeshFragment);
	RegisterBuiltinShader(HRL_DEBUG_SHADER, gles3_shaders::kDebugVertex, gles3_shaders::kDebugFragment);
	bck_->ui_shader = CreateBuiltinShader(gles3_shaders::kUIVertex, gles3_shaders::kUIFragment);
	bck_->vfx_shader = CreateBuiltinShader(gles3_shaders::kVFXVertex, gles3_shaders::kVFXFragment);
	bck_->sky_shader = CreateBuiltinShader(gles3_shaders::kSkyVertex, gles3_shaders::kSkyFragment);

	//GEOMETRY
	CreateSpriteGeometry();
	CreateVFXGeometry();
	CreateSkySphereGeometry();

	glGenSamplers(1, &bck_->nearest_sampler);
	glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glSamplerParameteri(bck_->nearest_sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);

	//FALLBACK TEXTURES
	bck_->fallback_textures[GLES3_ALBEDO_INT] = CreateFallbackTexture(res_default_albedo_png, res_default_albedo_png_len);
	bck_->fallback_textures[GLES3_NORMAL_INT] = CreateFallbackTexture(res_default_normal_png, res_default_normal_png_len);
	bck_->fallback_textures[GLES3_SPECULAR_INT] = CreateFallbackTexture(res_default_specular_png, res_default_specular_png_len);
	bck_->fallback_textures[GLES3_ROUGHNESS_INT] = CreateFallbackTexture(res_default_roughness_png, res_default_roughness_png_len);
	bck_->fallback_textures[GLES3_METALLIC_INT] = CreateFallbackTexture(res_default_metallic_png, res_default_metallic_png_len);
	bck_->fallback_textures[GLES3_ALPHA_INT] = CreateFallbackTexture(res_default_alpha_png, res_default_alpha_png_len);
	{
		auto metallicIt = bck_->textures.find(bck_->fallback_textures[GLES3_METALLIC_INT]);
		if (metallicIt != bck_->textures.end() && metallicIt->second)
			bck_->fallback_metallic = (float)metallicIt->second->GetFirstTexel()[0] / 255.f;
	}

	//UBO: lights (binding point 0)
	glGenBuffers(1, &bck_->light_ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, bck_->light_ubo);
	glBufferData(GL_UNIFORM_BUFFER, GLES3_MAX_LIGHTS * sizeof(GLES3_Light), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->light_ubo);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DITHER);
}

void GLES3_Shutdown()
{
	if (!bck_)
		return;

	for (auto& [id, mesh] : bck_->meshes)
	{
		(void)id;
		for (auto& lod : mesh.levels)
			DestroyMeshLOD(lod);
	}
	for (auto& [id, mesh] : bck_->skeletal_meshes)
	{
		(void)id;
		DestroySkeletalMesh(mesh);
	}
	for (auto& [id, scene] : bck_->gpu_scenes)
	{
		(void)id;
		if (scene)
		{
			DestroySceneTarget(scene);
			delete scene;
		}
	}
	for (auto& [id, shader] : bck_->shaders) { (void)id; delete shader; }
	for (auto& [id, texture] : bck_->textures) { (void)id; delete texture; }
	delete bck_->ui_shader;
	delete bck_->vfx_shader;
	delete bck_->sky_shader;

	const GLuint vaos[] = {bck_->ui_vao, bck_->debug_vao, bck_->sprite_vao, bck_->sprite_instanced_vao, bck_->vfx_vao, bck_->sky_vao};
	glDeleteVertexArrays((GLsizei)(sizeof(vaos) / sizeof(vaos[0])), vaos);
	const GLuint buffers[] = {
		bck_->ui_vbo, bck_->debug_vbo, bck_->light_ubo,
		bck_->sprite_vbo, bck_->sprite_ebo, bck_->sprite_instance_vbo,
		bck_->vfx_vbo, bck_->vfx_ebo, bck_->vfx_instance_vbo,
		bck_->sky_vbo, bck_->sky_ebo};
	glDeleteBuffers((GLsizei)(sizeof(buffers) / sizeof(buffers[0])), buffers);
	if (bck_->nearest_sampler) glDeleteSamplers(1, &bck_->nearest_sampler);
	if (bck_->ui_white_texture) glDeleteTextures(1, &bck_->ui_white_texture);

	delete bck_;
	delete ctx_;
	bck_ = nullptr;
	ctx_ = nullptr;
	ui_quads_.clear();
	ui_vertices_.clear();
}

void GLES3_WindowResizeCallback(int width, int height)
{
	if (!bck_ || width <= 0 || height <= 0)
		return;

	glViewport(0, 0, width, height);

	// Like the desktop backend, off-screen scenes follow the window size.
	for (const auto& [id, scene] : bck_->gpu_scenes)
	{
		(void)id;
		if (!scene)
			continue;
		if (scene->on_screen)
		{
			scene->width = width;
			scene->height = height;
		}
		else
			AllocateSceneTarget(scene, width, height);
	}
}

void GLES3_ResetFramebuffer()
{
}

// ===========================================================================
// Frame
// ===========================================================================

void GLES3_DrawScene(hrl_scene_t* scene, HRL_id scene_id)
{
	if (!bck_ || !scene)
		return;

	auto scene_it = bck_->gpu_scenes.find(scene_id);
	if (scene_it == bck_->gpu_scenes.end() || !scene_it->second)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_DrawScene: tried to draw scene with invalid gpu ID");
		return;
	}
	GLES3_Scene* gpu_scene = scene_it->second;

	// A scene can be switched between on-screen and off-screen after creation.
	const bool onScreen = scene->draw_on_screen != 0;
	if (onScreen != gpu_scene->on_screen)
	{
		gpu_scene->on_screen = onScreen;
		if (onScreen)
			DestroySceneTarget(gpu_scene);
	}
	if (!onScreen && gpu_scene->fbo == 0)
		AllocateSceneTarget(gpu_scene, (int)GetWindowWidth(), (int)GetWindowHeight());
	const GLuint target_fbo = onScreen ? 0u : gpu_scene->fbo;

	// Redundant uniforms are filtered by GLES3_Shader. Start every scene from an
	// empty cache so the renderer stays correct if the application writes
	// uniforms itself on an HRL program.
	GLES3_Shader::InvalidateAllValueCaches();
	ctx_->current_scene = scene;
	ctx_->current_fog = &scene->fog;

	const int winW = (int)GetWindowWidth();
	const int winH = (int)GetWindowHeight();

	glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
	glViewport(0, 0, winW, winH);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// The light UBO does not depend on the camera: once per scene.
	UploadSceneLights(scene);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, bck_->light_ubo);

	// Viewports are drawn in creation order (increasing IDs): the last created
	// one ends up on top.
	static std::vector<std::pair<HRL_id, HRL_Viewport*>> orderedViewports;
	orderedViewports.assign(scene->viewports.begin(), scene->viewports.end());
	std::sort(orderedViewports.begin(), orderedViewports.end(),
		[](const auto& a, const auto& b) { return a.first < b.first; });

	const auto debugIt = GetPrivateContext()->debug_renderers.find(scene_id);
	const bool hasDebug = debugIt != GetPrivateContext()->debug_renderers.end();

	bool firstViewport = true;
	for (const auto& v : orderedViewports)
	{
		HRL_Viewport* viewport = v.second;
		if (!viewport || !viewport->camera_)
			continue;
		ctx_->viewport = viewport;
		ctx_->bound_material = nullptr;
		ctx_->bound_shader = nullptr;

		const int rectX = (int)(viewport->x_ * (float)winW);
		const int rectY = (int)(viewport->y_ * (float)winH);
		const int rectW = (int)(viewport->width_ * (float)winW);
		const int rectH = (int)(viewport->height_ * (float)winH);
		glViewport(rectX, rectY, rectW, rectH);

		// Each viewport starts from an empty depth buffer (already the case for
		// the first one, cleared with the scene).
		if (!firstViewport)
		{
			glDepthMask(GL_TRUE);
			glClear(GL_DEPTH_BUFFER_BIT);
		}
		firstViewport = false;

		ctx_->proj_mat = CalculateProjectionMatrix();
		ctx_->view_mat = CalculateViewMatrix();
		const FrustumPlaneSet cameraFrustum = BuildFrustum(ctx_->proj_mat * ctx_->view_mat);

		// Sky, opaque 3D geometry, then the transparent passes.
		DrawSkySphere(scene);
		DrawOpaqueMeshes(scene->meshes, cameraFrustum);
		DrawSprites(scene->meshes, cameraFrustum);
		DrawVFX(scene);

		// Overlays.
		if (hasDebug)
			DrawDebug(debugIt->second, GetPrivateContext()->debug_line_thickness);
		DrawScreenMessages(scene, viewport);
		DrawWidgets(viewport->widgets, viewport);
	}

	// Known state for the next scene / the application.
	glViewport(0, 0, winW, winH);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glBindVertexArray(0);
	glUseProgram(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLES3_TakeScreenshot(HRL_id scene, const char* target_path)
{
	if (!bck_ || !target_path || *target_path == '\0')
		return;

	auto gpuIt = bck_->gpu_scenes.find(scene);
	if (gpuIt == bck_->gpu_scenes.end() || !gpuIt->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_TakeScreenshot: invalid scene ID");
		return;
	}

	const GLES3_Scene* gpuScene = gpuIt->second;
	const int width = std::max(1, gpuScene->on_screen ? (int)GetWindowWidth() : gpuScene->width);
	const int height = std::max(1, gpuScene->on_screen ? (int)GetWindowHeight() : gpuScene->height);
	std::vector<unsigned char> pixels((size_t)width * (size_t)height * 4u);

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (gpuScene->on_screen)
	{
		// The scene was just rendered into the back buffer (before the swap).
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glReadBuffer(GL_BACK);
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, gpuScene->fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
	}
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	const size_t rowBytes = (size_t)width * 4u;
	for (int y = 0; y < height / 2; ++y)
	{
		unsigned char* top = pixels.data() + (size_t)y * rowBytes;
		unsigned char* bottom = pixels.data() + (size_t)(height - 1 - y) * rowBytes;
		for (size_t x = 0; x < rowBytes; ++x)
			std::swap(top[x], bottom[x]);
	}
	// The default framebuffer may have no alpha channel: the image is opaque.
	for (size_t i = 3; i < pixels.size(); i += 4u)
		pixels[i] = 255;

	if (!WritePNG_RGBA8(target_path, width, height, pixels))
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR, "GLES3_TakeScreenshot: failed to write PNG");
}

// ===========================================================================
// Meshes
// ===========================================================================

int GLES3_CreateSpriteMesh(HRL_id id)
{
	if (!bck_ || id == HRL_INVALID_ID || bck_->sprite_vao == 0 || bck_->sprite_instanced_vao == 0)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_ERROR, "GLES3_CreateSpriteMesh: shared sprite geometry is not initialized");
		return HRL_FALSE;
	}
	// Sprites use the backend-owned shared plane: no per-sprite GPU buffers.
	return HRL_TRUE;
}

int GLES3_CreateMesh(HRL_id id, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count)
{
	if (!bck_)
		return HRL_FALSE;
	if (bck_->meshes.find(id) != bck_->meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_CreateMesh: mesh ID already exists");
		return HRL_FALSE;
	}
	MeshGPU gpu;
	gpu.levels.resize(1);
	if (!UploadMeshLOD(gpu.levels[0], vertices, vertex_count, indices, index_count))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GLES3_CreateMesh: invalid vertex/index data");
		return HRL_FALSE;
	}
	bck_->meshes.emplace(id, std::move(gpu));
	return HRL_TRUE;
}

int GLES3_CreateMeshLOD(HRL_id id, HRL_uint level, const HRL_Vertex3D* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count)
{
	if (!bck_)
		return HRL_FALSE;
	auto it = bck_->meshes.find(id);
	if (it == bck_->meshes.end() || level == 0)
		return HRL_FALSE;
	if (it->second.levels.size() <= level)
		it->second.levels.resize((size_t)level + 1);
	MeshLOD_GPU& gpu = it->second.levels[level];
	DestroyMeshLOD(gpu);
	return UploadMeshLOD(gpu, vertices, vertex_count, indices, index_count) ? HRL_TRUE : HRL_FALSE;
}

void GLES3_DeleteMeshLODs(HRL_id id)
{
	if (!bck_)
		return;
	auto it = bck_->meshes.find(id);
	if (it == bck_->meshes.end())
		return;
	for (size_t k = 1; k < it->second.levels.size(); ++k)
		DestroyMeshLOD(it->second.levels[k]);
	it->second.levels.resize(1);
}

int GLES3_CreateSkeletalMesh(HRL_id id, const HRL_SkeletalVertex* vertices, size_t vertex_count, const HRL_uint* indices, size_t index_count, HRL_uint bone_count)
{
	if (!bck_)
		return HRL_FALSE;
	if (bck_->skeletal_meshes.find(id) != bck_->skeletal_meshes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_CreateSkeletalMesh: skeletal mesh ID already exists");
		return HRL_FALSE;
	}
	SkeletalMeshGPU gpu;
	if (!UploadSkeletalMesh(gpu, vertices, vertex_count, indices, index_count, bone_count))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "GLES3_CreateSkeletalMesh: invalid skeletal vertex/index data");
		return HRL_FALSE;
	}
	bck_->skeletal_meshes.emplace(id, gpu);
	return HRL_TRUE;
}

void GLES3_DeleteMesh(HRL_id id)
{
	if (!bck_)
		return;
	auto it = bck_->meshes.find(id);
	if (it != bck_->meshes.end())
	{
		for (auto& lod : it->second.levels)
			DestroyMeshLOD(lod);
		bck_->meshes.erase(it);
		return;
	}
	auto sit = bck_->skeletal_meshes.find(id);
	if (sit != bck_->skeletal_meshes.end())
	{
		DestroySkeletalMesh(sit->second);
		bck_->skeletal_meshes.erase(sit);
	}
}

// ===========================================================================
// Lights
// ===========================================================================

void GLES3_UpdateLights(const std::vector<HRL_Light*>& lights)
{
	// GLES3_DrawScene -> UploadSceneLights() sends the lights of the scene right
	// before it is rendered.
	(void)lights;
}

void GLES3_DeleteLight(HRL_id id)
{
	(void)id;
}

// ===========================================================================
// Scenes
// ===========================================================================

void GLES3_CreateScene(HRL_id _newSceneid, int _renderOnScreen)
{
	if (!bck_)
		return;
	auto* scene = new GLES3_Scene();
	scene->on_screen = _renderOnScreen != 0;
	scene->width = (int)GetWindowWidth();
	scene->height = (int)GetWindowHeight();
	if (!scene->on_screen)
		AllocateSceneTarget(scene, scene->width, scene->height);
	bck_->gpu_scenes.emplace(_newSceneid, scene);
}

void GLES3_DeleteScene(HRL_id _sceneid)
{
	if (!bck_)
		return;
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_DeleteScene error: invalid scene id");
		return;
	}
	if (it->second)
	{
		DestroySceneTarget(it->second);
		delete it->second;
	}
	bck_->gpu_scenes.erase(it);
}

void GLES3_ResizeSceneTexture(HRL_id _sceneid, int _width, int _height)
{
	if (!bck_)
		return;
	auto it = bck_->gpu_scenes.find(_sceneid);
	if (it == bck_->gpu_scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_ResizeSceneTexture error: invalid scene id");
		return;
	}
	if (it->second->on_screen)
	{
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING, "GLES3_ResizeSceneTexture: the scene is rendered on the screen");
		return;
	}
	AllocateSceneTarget(it->second, _width, _height);
}

// ===========================================================================
// Shaders
// ===========================================================================

HRL_id GLES3_CreateShader(const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize)
{
	if (!bck_)
		return HRL_INVALID_ID;
	auto* s = new GLES3_Shader();
	if (s->GLES3_Create(_vertContent, _vertSize, _fragContent, _fragSize) != 0)
	{
		delete s;
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	bck_->shaders.emplace(id, s);
	return id;
}

HRL_id GLES3_CreateShaderWithId(HRL_id id, const char* _vertContent, size_t _vertSize, const char* _fragContent, size_t _fragSize)
{
	if (!bck_ || id == HRL_INVALID_ID || bck_->shaders.find(id) != bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_CreateShaderWithId: ID already exists or is invalid");
		return HRL_INVALID_ID;
	}
	auto* s = new GLES3_Shader();
	if (s->GLES3_Create(_vertContent, _vertSize, _fragContent, _fragSize) != 0)
	{
		delete s;
		return HRL_INVALID_ID;
	}
	bck_->shaders.emplace(id, s);
	return id;
}

void GLES3_DeleteShader(HRL_id _id)
{
	if (!bck_)
		return;
	auto it = bck_->shaders.find(_id);
	if (it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteShader error: Shader ID doesn't exists");
		return;
	}
	if (ctx_->bound_shader == it->second) ctx_->bound_shader = nullptr;
	if (ctx_->shader == it->second) ctx_->shader = nullptr;
	delete it->second;
	bck_->shaders.erase(it);
}

// ===========================================================================
// Textures
// ===========================================================================

HRL_id GLES3_CreateTexture(const char* _imageContent, size_t _imageSize)
{
	if (!bck_)
		return HRL_INVALID_ID;
	auto* t = new GLES3_Texture();
	if (t->GLES3_Create(_imageContent, _imageSize) != 0)
	{
		delete t;
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	bck_->textures.emplace(id, t);
	return id;
}

HRL_id GLES3_CreateTextureFromBitmap(BitmapResult bmp)
{
	if (!bck_)
		return HRL_INVALID_ID;
	auto* t = new GLES3_Texture();
	if (t->GLES3_CreateFromBitmap(&bmp) != 0)
	{
		delete t;
		return HRL_INVALID_ID;
	}
	const HRL_id id = GenerateHRL_ID();
	bck_->textures.emplace(id, t);
	return id;
}

HRL_id GLES3_CreateTextureFromBitmapWithId(HRL_id id, BitmapResult bmp)
{
	if (!bck_ || id == HRL_INVALID_ID || bck_->textures.find(id) != bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_CreateTextureFromBitmapWithId: ID already exists or is invalid");
		return HRL_INVALID_ID;
	}
	auto* t = new GLES3_Texture();
	if (t->GLES3_CreateFromBitmap(&bmp) != 0)
	{
		delete t;
		return HRL_INVALID_ID;
	}
	bck_->textures.emplace(id, t);
	return id;
}

void GLES3_DeleteTexture(HRL_id _id)
{
	if (!bck_)
		return;
	auto it = bck_->textures.find(_id);
	if (it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "DeleteTexture error: Texture ID doesn't exists");
		return;
	}
	delete it->second;
	bck_->textures.erase(it);
	// The texture may be bound through the cached material.
	ctx_->bound_material = nullptr;
}

void GLES3_GetTextureSize(HRL_id id, int* width, int* height)
{
	auto it = bck_ ? bck_->textures.find(id) : decltype(bck_->textures.end()){};
	if (!bck_ || it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_GetTextureSize error: Texture ID doesn't exists");
		return;
	}
	if (width) *width = (int)it->second->GetWidth();
	if (height) *height = (int)it->second->GetHeight();
}

void GLES3_SetTextureMinFilter(HRL_id id, HRL_EFilterType _filter)
{
	auto it = bck_ ? bck_->textures.find(id) : decltype(bck_->textures.end()){};
	if (!bck_ || it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_SetTextureMinFilter error: Texture ID doesn't exists");
		return;
	}
	it->second->SetMinFilter(_filter);
}

void GLES3_SetTextureMaxFilter(HRL_id id, HRL_EFilterType _filter)
{
	auto it = bck_ ? bck_->textures.find(id) : decltype(bck_->textures.end()){};
	if (!bck_ || it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "GLES3_SetTextureMaxFilter error: Texture ID doesn't exists");
		return;
	}
	it->second->SetMaxFilter(_filter);
}

// ===========================================================================
// Features outside the scope of this backend
// ===========================================================================

void GLES3_CreatePostProcess(HRL_id mat, int priority)
{
	(void)mat;
	(void)priority;
	if (bck_ && !bck_->warned_post_process)
	{
		bck_->warned_post_process = true;
		SetErrorCode(HRL_INVALID_BACKEND_OPERATION, HRL_SEVERITY_WARNING,
			"Post-processes are not supported by the OpenGL ES backend: they are ignored");
	}
}

void GLES3_DeletePostProcess(HRL_id post)
{
	(void)post;
}

void GLES3_FogPropertyChanged(HRL_id scene, hrl_fog_t* fog_ptr)
{
	// The distance fog is read from the scene each time a shader is bound.
	(void)scene;
	(void)fog_ptr;
}

void GLES3_EnableColorPickingBuffer(HRL_id scene, int _enable)
{
	(void)scene;
	(void)_enable;
}

void GLES3_SetAntialiasingMode(int samples)
{
	// Multisampling of the default framebuffer is chosen with the EGL config.
	(void)samples;
}

int GLES3_IsGlobalIlluminationMethodSupported(int method)
{
	return method == HRL_GI_NONE ? HRL_TRUE : HRL_FALSE;
}

uint32_t GLES3_GetGlobalIlluminationSupportedMethods()
{
	return 0u;
}

// ===========================================================================
// Matrices / debug / requests
// ===========================================================================

void GLES3_GetProjectionMatrix(float* aa)
{
	if (ctx_ && aa)
		std::memcpy(aa, glm::value_ptr(ctx_->proj_mat), sizeof(float) * 16);
}

void GLES3_GetViewMatrix(float* aa)
{
	if (ctx_ && aa)
		std::memcpy(aa, glm::value_ptr(ctx_->view_mat), sizeof(float) * 16);
}

void GLES3_GetModelMatrix(HRL_Mesh* mesh, float* aa)
{
	if (!mesh || !aa)
		return;
	const glm::mat4 model = CalculateModelMatrix(mesh);
	std::memcpy(aa, glm::value_ptr(model), sizeof(float) * 16);
}

// Debug primitives are drawn inside GLES3_DrawScene, for every viewport.
void GLES3_DrawDebugAfterScene(const DebugRenderer& _renderer, float line_thickness)
{
	(void)_renderer;
	(void)line_thickness;
}

int GLES3_IsValidTexture(HRL_id tex)
{
	return (bck_ && bck_->textures.find(tex) != bck_->textures.end()) ? 1 : 0;
}

int GLES3_IsValidShader(HRL_id shader)
{
	return (bck_ && bck_->shaders.find(shader) != bck_->shaders.end()) ? 1 : 0;
}

// ===========================================================================
// hrl_gl.h
// ===========================================================================
// These entry points are implemented by the OpenGL 3.3 backend. When it is not
// compiled (mobile builds), the ES backend provides them.
#ifdef HRL_DISABLE_OPENGL33

unsigned int HRL_GL_GetTextureGL_ID(HRL_id _textureid)
{
	auto it = bck_ ? bck_->textures.find(_textureid) : decltype(bck_->textures.end()){};
	if (!bck_ || it == bck_->textures.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetTextureGL_ID error: Texture ID doesn't exists");
		return GL_INVALID_VALUE;
	}
	return it->second->GetGL_ID();
}

unsigned int HRL_GL_GetShaderGL_ID(HRL_id _shaderid)
{
	auto it = bck_ ? bck_->shaders.find(_shaderid) : decltype(bck_->shaders.end()){};
	if (!bck_ || it == bck_->shaders.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetShaderGL_ID error: Shader ID doesn't exists");
		return GL_INVALID_VALUE;
	}
	return (unsigned int)it->second->GetId();
}

// Color texture of an off-screen scene. 0 for a scene rendered on the screen
// (it has no intermediate texture with this backend).
unsigned int HRL_GL_GetSceneTextureGL_ID(HRL_id _sceneid)
{
	auto it = bck_ ? bck_->gpu_scenes.find(_sceneid) : decltype(bck_->gpu_scenes.end()){};
	if (!bck_ || it == bck_->gpu_scenes.end() || !it->second)
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_GL_GetSceneTextureGL_ID: scene ID is not valid");
		return GL_INVALID_VALUE;
	}
	return it->second->color_texture;
}

// Picking and G-buffers do not exist with this backend.
unsigned int HRL_GL_GetSceneColorBufferGL_ID(HRL_id _sceneid) { (void)_sceneid; return 0; }
unsigned int HRL_GL_GetSceneColorPickingBufferGL_ID(HRL_id _sceneid) { (void)_sceneid; return 0; }
unsigned int HRL_GL_GetSceneAlbedoBufferGL_ID(HRL_id _sceneid) { (void)_sceneid; return 0; }
unsigned int HRL_GL_GetSceneNormalBufferGL_ID(HRL_id _sceneid) { (void)_sceneid; return 0; }

HRL_id HRL_GL_GetHoveredObject(HRL_id _scene, int mouseX, int mouseY, HRL_EMeshType* mesh_type)
{
	(void)_scene; (void)mouseX; (void)mouseY; (void)mesh_type;
	return HRL_INVALID_ID;
}

#endif // HRL_DISABLE_OPENGL33
