#include "gi.h"

#include "../common/hrl_gi_glsl.h"
#include "../../core/utils_functions.h"
#include "gl33_renderer.h"
#include "gl33_texture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// Texture units used by the GI passes. Units 6..14 and 16 hold the shadow
// maps, the environment map and the voxel light field of the renderer, which
// later passes of the frame still use.
const int kUnits[] = {0, 1, 2, 3, 4, 5, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25};
constexpr int kUnitCount = (int)(sizeof(kUnits) / sizeof(kUnits[0]));

constexpr int kDataTextureWidth = 2048;
constexpr int kDDGIRays = 64;
constexpr int kDDGIIrradianceTexels = 8;
constexpr int kDDGIDistanceTexels = 16;
constexpr int kVoxelLevels = 6;
constexpr int kLPVIterations = 12;
constexpr int kPathTracingBounces = 6;
constexpr int kRayTracingBounces = 2;
constexpr int kVoxelBounces = 2;     // extra light bounces computed in the voxel volume
// Empirical energy compensation of the volume methods (thin geometry loses
// coverage in the mips), calibrated against the path traced reference.
constexpr float kVCTGain = 2.2f;
constexpr float kLPVGain = 2.0f;

GLuint CreateTexture(GLenum internalFormat, int w, int h, GLenum format, GLenum type, const void* data, GLint filter)
{
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, std::max(1, w), std::max(1, h), 0, format, type, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

GLuint CreateFbo(const GLuint* textures, int count)
{
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	GLenum buffers[4];
	for (int i = 0; i < count; ++i)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, textures[i], 0);
		buffers[i] = GL_COLOR_ATTACHMENT0 + i;
	}
	glDrawBuffers(count, buffers);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return fbo;
}

template <typename T>
void CreateTarget(T* t, GLenum internalFormat, int w, int h, GLint filter)
{
	t->texture = CreateTexture(internalFormat, w, h, GL_RGBA, GL_FLOAT, nullptr, filter);
	t->fbo = CreateFbo(&t->texture, 1);
}

void DeleteTarget(GLuint& texture, GLuint& fbo)
{
	if (fbo) glDeleteFramebuffers(1, &fbo);
	if (texture) glDeleteTextures(1, &texture);
	fbo = 0;
	texture = 0;
}

// Uploads a float4 array as a RGBA32F texture of kDataTextureWidth texels per row.
GLuint UploadFloat4(const std::vector<glm::vec4>& data)
{
	const int count = std::max<int>(1, (int)data.size());
	const int w = std::min(count, kDataTextureWidth);
	const int h = (count + w - 1) / w;
	std::vector<glm::vec4> padded((size_t)w * (size_t)h, glm::vec4(0.f));
	std::copy(data.begin(), data.end(), padded.begin());
	return CreateTexture(GL_RGBA32F, w, h, GL_RGBA, GL_FLOAT, padded.data(), GL_NEAREST);
}

glm::mat3 RandomRotation(uint64_t frame)
{
	auto hash = [](uint32_t x) {
		x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
	const uint32_t a = hash((uint32_t)frame * 3u + 1u), b = hash((uint32_t)frame * 3u + 2u), c = hash((uint32_t)frame * 3u + 3u);
	const float u1 = (float)a / 4294967296.f, u2 = (float)b / 4294967296.f, u3 = (float)c / 4294967296.f;
	// Uniform random quaternion (Shoemake).
	const float s1 = std::sqrt(1.f - u1), s2 = std::sqrt(u1);
	const float t1 = 6.2831853f * u2, t2 = 6.2831853f * u3;
	const glm::quat q(s2 * std::cos(t2), s1 * std::sin(t1), s1 * std::cos(t1), s2 * std::sin(t2));
	return glm::mat3_cast(glm::normalize(q));
}

float ResolutionScale(int method)
{
	switch (method)
	{
	case HRL_GI_PATH_TRACING: return 1.f;
	case HRL_GI_SSGI:
	case HRL_GI_VCT:
	case HRL_GI_RAY_TRACING: return 0.5f;
	default: return 1.f;
	}
}
} // namespace

GL_33_GI::~GL_33_GI()
{
	Shutdown();
}

void GL_33_GI::Initialize(GLuint fullscreenVao)
{
	fullscreenVao_ = fullscreenVao;
}

bool GL_33_GI::Supports(int method)
{
	return method == HRL_GI_SSGI || method == HRL_GI_VCT || method == HRL_GI_LPV || method == HRL_GI_DDGI ||
		method == HRL_GI_PATH_TRACING || method == HRL_GI_RAY_TRACING;
}

uint32_t GL_33_GI::SupportedMethods()
{
	uint32_t mask = 0;
	for (int m = HRL_GI_SSGI; m <= HRL_GI_RAY_TRACING; ++m)
		if (Supports(m)) mask |= 1u << (unsigned)m;
	return mask;
}

void GL_33_GI::Shutdown()
{
	for (auto& [id, g] : scenes_) ReleaseSceneGPU(*g);
	scenes_.clear();
	for (auto& [id, v] : views_) ReleaseView(*v);
	views_.clear();
	programs_.clear();
}

void GL_33_GI::ReleaseScene(HRL_id sceneId)
{
	auto it = scenes_.find(sceneId);
	if (it != scenes_.end())
	{
		ReleaseSceneGPU(*it->second);
		scenes_.erase(it);
	}
	for (auto vit = views_.begin(); vit != views_.end();)
	{
		if (vit->second->sceneId == sceneId) { ReleaseView(*vit->second); vit = views_.erase(vit); }
		else ++vit;
	}
}

void GL_33_GI::ReleaseViewport(HRL_id viewportId)
{
	auto it = views_.find(viewportId);
	if (it == views_.end()) return;
	ReleaseView(*it->second);
	views_.erase(it);
}

void GL_33_GI::ReleaseView(ViewGPU& v)
{
	DeleteTarget(v.target.texture, v.target.fbo);
	for (auto& h : v.history) DeleteTarget(h.texture, h.fbo);
	v.historyValid = false;
	v.width = v.height = 0;
}

void GL_33_GI::ReleaseSceneGPU(SceneGPU& g)
{
	if (g.nodes) glDeleteTextures(1, &g.nodes);
	if (g.triangles) glDeleteTextures(1, &g.triangles);
	g.nodes = g.triangles = 0;
	DeleteTarget(g.ddgiRays.texture, g.ddgiRays.fbo);
	for (int i = 0; i < 2; ++i)
	{
		DeleteTarget(g.ddgiIrradiance[i].texture, g.ddgiIrradiance[i].fbo);
		DeleteTarget(g.ddgiDistance[i].texture, g.ddgiDistance[i].fbo);
	}
	g.ddgiReady = false;
	if (g.voxelAlbedo) glDeleteTextures(1, &g.voxelAlbedo);
	if (g.voxelNormal) glDeleteTextures(1, &g.voxelNormal);
	g.voxelAlbedo = g.voxelNormal = 0;
	for (auto& l : g.voxelLevels) DeleteTarget(l.texture, l.fbo);
	DeleteTarget(g.voxelScratch.texture, g.voxelScratch.fbo);
	g.voxelsReady = false;
	g.voxelsLit = false;
	for (auto& set : g.lpvTex)
		for (auto& t : set)
			if (t) { glDeleteTextures(1, &t); t = 0; }
	for (auto& f : g.lpvFbo)
		if (f) { glDeleteFramebuffers(1, &f); f = 0; }
	g.lpvReady = false;
	g.uploadedRevision = 0;
}

// ---------------------------------------------------------------------------
// Programs and bindings
// ---------------------------------------------------------------------------
GL33_Shader* GL_33_GI::Program(const char* key)
{
	auto it = programs_.find(key);
	if (it != programs_.end()) return it->second.get();
	using namespace hrl_gi_glsl;
	const std::string k = key;
	const std::string src = AssembleFragment(key);
	auto shader = std::make_unique<GL33_Shader>();
	if (shader->GL33_Create(kVertex, std::strlen(kVertex), src.data(), src.size()) != 0)
	{
		SetErrorCode(HRL_SHADER_COMPILE_FAIL, HRL_SEVERITY_ERROR, std::string("GI shader '") + key + "' failed to compile");
		programs_[k] = nullptr;
		return nullptr;
	}
	GL33_Shader* raw = shader.get();
	programs_[k] = std::move(shader);
	return raw;
}

void GL_33_GI::Bind(GL33_Shader* s, const char* name, GLuint texture, int& unit)
{
	if (unit >= kUnitCount) return;
	glActiveTexture(GL_TEXTURE0 + kUnits[unit]);
	glBindTexture(GL_TEXTURE_2D, texture);
	s->SetInt(name, kUnits[unit]);
	++unit;
}

void GL_33_GI::Quad()
{
	glBindVertexArray(fullscreenVao_);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
	glBindVertexArray(0);
}

void GL_33_GI::SetCommon(GL33_Shader* s, SceneGPU& g, const hrl_scene_t* scene, int& unit)
{
	s->Use();
	Bind(s, "uGINodes", g.nodes, unit);
	Bind(s, "uGITris", g.triangles, unit);
	const glm::vec3 extent = g.data.boundsMax - g.data.boundsMin;
	s->SetFloat("uGIBias", std::max(1e-3f * std::max(extent.x, std::max(extent.y, extent.z)), 1e-4f));
	s->SetInt("uGISkyEnabled", scene->sky_sphere_enabled ? 1 : 0);
	GLuint skyTexture = 0;
	if (scene->sky_sphere_enabled && scene->sky_texture != HRL_INVALID_ID)
		if (const GL33_Texture* t = GL33_FindTexture(scene->sky_texture)) skyTexture = t->GetGL_ID();
	s->SetInt("uGISkyTextureEnabled", skyTexture ? 1 : 0);
	if (skyTexture) Bind(s, "uGISkyTexture", skyTexture, unit);
	s->SetVec3("uGISkyTop", scene->sky_top_color);
	s->SetVec3("uGISkyHorizon", scene->sky_horizon_color);
	s->SetVec3("uGISkyBottom", scene->sky_bottom_color);
	s->SetInt("uGIFrame", (int)(g.frame & 0x7fffffff));
}

void GL_33_GI::SetDDGI(GL33_Shader* s, SceneGPU& g, int& unit)
{
	Bind(s, "uDDGIIrradiance", g.ddgiIrradiance[g.ddgiCurrent].texture, unit);
	Bind(s, "uDDGIDistance", g.ddgiDistance[g.ddgiCurrent].texture, unit);
	s->SetVec3("uDDGIOrigin", g.data.probeOrigin);
	s->SetVec3("uDDGISpacing", g.data.probeSpacing);
	s->SetVec3("uDDGICountsF", glm::vec3(g.data.probeCounts));
	s->SetInt("uDDGITilesPerRow", g.ddgiTilesPerRow);
	s->SetInt("uDDGIRays", kDDGIRays);
	s->SetMat3("uDDGIRayRotation", RandomRotation(g.ddgiFrames));
}

void GL_33_GI::SetVCT(GL33_Shader* s, SceneGPU& g, int& unit)
{
	static const char* names[kVoxelLevels] = {"uVCTLevel0", "uVCTLevel1", "uVCTLevel2", "uVCTLevel3", "uVCTLevel4", "uVCTLevel5"};
	for (int l = 0; l < kVoxelLevels; ++l)
	{
		Bind(s, names[l], g.voxelLevels[l].texture, unit);
		s->SetInt((std::string("uVCTTiles[") + std::to_string(l) + "]").c_str(),
			HRL_GIAtlasLayout::For(g.data.voxelResolution >> l).tilesPerRow);
	}
	s->SetInt("uVCTRes", g.data.voxelResolution);
	s->SetFloat("uVCTGain", kVCTGain);
	s->SetVec3("uVCTOrigin", g.data.voxelOrigin);
	s->SetFloat("uVCTVoxelSize", g.data.voxelSize);
}

void GL_33_GI::SetLPV(GL33_Shader* s, SceneGPU& g, int& unit)
{
	Bind(s, "uLPVR", g.lpvTex[2][0], unit);
	Bind(s, "uLPVG", g.lpvTex[2][1], unit);
	Bind(s, "uLPVB", g.lpvTex[2][2], unit);
	const int res = g.data.voxelResolution / 2;
	s->SetInt("uLPVRes", res);
	s->SetInt("uLPVTiles", HRL_GIAtlasLayout::For(res).tilesPerRow);
	s->SetVec3("uLPVOrigin", g.data.voxelOrigin);
	s->SetFloat("uLPVCellSize", g.data.voxelSize * 2.f);
	s->SetFloat("uLPVGain", kLPVGain);
}

void GL_33_GI::SetScreen(GL33_Shader* s, const View& v, float scale, int& unit)
{
	Bind(s, "uDepth", v.depth, unit);
	Bind(s, "uNormal", v.normal, unit);
	Bind(s, "uAlbedo", v.albedo, unit);
	const glm::mat4 vp = v.projection * v.view;
	s->SetMat4("uViewProjection", vp);
	s->SetMat4("uInvViewProjection", glm::inverse(vp));
	s->SetVec3("uCameraPos", v.cameraPosition);
	s->SetVec4("uViewportRect", glm::vec4((float)v.x, (float)v.y, (float)v.width, (float)v.height));
	s->SetFloat("uResolutionScale", scale);
}

// ---------------------------------------------------------------------------
// World space
// ---------------------------------------------------------------------------
bool GL_33_GI::UploadScene(SceneGPU& g)
{
	if (g.uploadedRevision == g.data.geometryRevision && g.nodes) return true;
	ReleaseSceneGPU(g);
	g.nodes = UploadFloat4(g.data.nodes);
	g.triangles = UploadFloat4(g.data.triangles);
	g.uploadedRevision = g.data.geometryRevision;
	return true;
}

void GL_33_GI::UpdateDDGI(SceneGPU& g, const hrl_scene_t* scene)
{
	const int probes = g.data.ProbeCount();
	if (probes <= 0) return;
	if (!g.ddgiReady)
	{
		g.ddgiTilesPerRow = std::max(1, (int)std::ceil(std::sqrt((double)probes)));
		const int rows = (probes + g.ddgiTilesPerRow - 1) / g.ddgiTilesPerRow;
		CreateTarget(&g.ddgiRays, GL_RGBA32F, kDDGIRays, probes, GL_NEAREST);
		for (int i = 0; i < 2; ++i)
		{
			CreateTarget(&g.ddgiIrradiance[i], GL_RGBA16F, g.ddgiTilesPerRow * (kDDGIIrradianceTexels + 2), rows * (kDDGIIrradianceTexels + 2), GL_LINEAR);
			CreateTarget(&g.ddgiDistance[i], GL_RGBA32F, g.ddgiTilesPerRow * (kDDGIDistanceTexels + 2), rows * (kDDGIDistanceTexels + 2), GL_LINEAR);
			for (Target* t : {&g.ddgiIrradiance[i], &g.ddgiDistance[i]})
			{
				glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
				glClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
		}
		g.ddgiReady = true;
		g.ddgiFrames = 0;
		g.ddgiCurrent = 0;
	}
	if (g.ddgiLightingRevision != scene->gi_lighting_revision)
	{
		// Lights changed : converge again quickly from the current state.
		g.ddgiLightingRevision = scene->gi_lighting_revision;
		g.ddgiFrames = std::min<uint64_t>(g.ddgiFrames, 4);
	}

	const int irrRows = (probes + g.ddgiTilesPerRow - 1) / g.ddgiTilesPerRow;
	// 1. Probe rays.
	GL33_Shader* trace = Program("ddgi_trace");
	GL33_Shader* update = Program("ddgi_update");
	if (!trace || !update) return;
	int unit = 0;
	SetCommon(trace, g, scene, unit);
	SetDDGI(trace, g, unit);
	trace->SetInt("uDDGIHasHistory", g.ddgiFrames > 0 ? 1 : 0);
	glBindFramebuffer(GL_FRAMEBUFFER, g.ddgiRays.fbo);
	glViewport(0, 0, kDDGIRays, probes);
	Quad();

	// 2. Irradiance and depth moments, blended with the previous frame.
	const float hysteresis = g.ddgiFrames == 0 ? 0.f : std::min(0.97f, 1.f - 1.f / (float)(g.ddgiFrames + 1));
	const float maxSpacing = std::max(g.data.probeSpacing.x, std::max(g.data.probeSpacing.y, g.data.probeSpacing.z));
	const int next = 1 - g.ddgiCurrent;
	for (int pass = 0; pass < 2; ++pass)
	{
		const bool distance = pass == 1;
		const int texels = distance ? kDDGIDistanceTexels : kDDGIIrradianceTexels;
		unit = 0;
		SetCommon(update, g, scene, unit);
		SetDDGI(update, g, unit);
		Bind(update, "uRays", g.ddgiRays.texture, unit);
		Bind(update, "uPrevious", distance ? g.ddgiDistance[g.ddgiCurrent].texture : g.ddgiIrradiance[g.ddgiCurrent].texture, unit);
		update->SetInt("uTexels", texels);
		update->SetInt("uDistance", distance ? 1 : 0);
		update->SetFloat("uHysteresis", hysteresis);
		update->SetFloat("uMaxDistance", maxSpacing * 1.5f);
		glBindFramebuffer(GL_FRAMEBUFFER, distance ? g.ddgiDistance[next].fbo : g.ddgiIrradiance[next].fbo);
		glViewport(0, 0, g.ddgiTilesPerRow * (texels + 2), irrRows * (texels + 2));
		Quad();
	}
	g.ddgiCurrent = next;
	++g.ddgiFrames;
}

void GL_33_GI::UpdateVoxels(SceneGPU& g, const hrl_scene_t* scene)
{
	const int res = g.data.voxelResolution;
	if (res <= 0) return;
	if (!g.voxelsReady)
	{
		const HRL_GIAtlasLayout atlas = HRL_GIAtlasLayout::For(res);
		g.voxelAlbedo = CreateTexture(GL_RGBA8, atlas.width, atlas.height, GL_RGBA, GL_UNSIGNED_BYTE, g.data.voxelAlbedo.data(), GL_NEAREST);
		g.voxelNormal = CreateTexture(GL_RGBA8, atlas.width, atlas.height, GL_RGBA, GL_UNSIGNED_BYTE, g.data.voxelNormal.data(), GL_NEAREST);
		for (int l = 0; l < kVoxelLevels; ++l)
		{
			const HRL_GIAtlasLayout a = HRL_GIAtlasLayout::For(std::max(1, res >> l));
			// Six directional volumes side by side (+X, -X, +Y, -Y, +Z, -Z).
			CreateTarget(&g.voxelLevels[l], GL_RGBA16F, a.width * 6, a.height, GL_NEAREST);
		}
		g.voxelsReady = true;
		g.voxelsLit = false;
	}
	if (g.voxelsLit && g.voxelLightingRevision == scene->gi_lighting_revision)
		return;

	GL33_Shader* inject = Program("voxel_inject");
	GL33_Shader* down = Program("voxel_down");
	if (!inject || !down) return;
	const HRL_GIAtlasLayout atlas = HRL_GIAtlasLayout::For(res);
	if (!g.voxelScratch.fbo)
		CreateTarget(&g.voxelScratch, GL_RGBA16F, atlas.width * 6, atlas.height, GL_NEAREST);
	for (int bounce = 0; bounce <= kVoxelBounces; ++bounce)
	{
		int unit = 0;
		SetCommon(inject, g, scene, unit);
		Bind(inject, "uVoxelAlbedo", g.voxelAlbedo, unit);
		Bind(inject, "uVoxelNormal", g.voxelNormal, unit);
		inject->SetInt("uVoxelRes", res);
		inject->SetInt("uVoxelTiles", atlas.tilesPerRow);
		inject->SetInt("uVoxelWidth", atlas.width);
		inject->SetVec3("uVoxelOrigin", g.data.voxelOrigin);
		inject->SetFloat("uVoxelSize", g.data.voxelSize);
		inject->SetInt("uBounce", bounce > 0 ? 1 : 0);
		if (bounce > 0) SetVCT(inject, g, unit);
		// Level 0 is read by the bounce : render into the scratch target and swap.
		glBindFramebuffer(GL_FRAMEBUFFER, g.voxelScratch.fbo);
		glViewport(0, 0, atlas.width * 6, atlas.height);
		Quad();
		std::swap(g.voxelScratch, g.voxelLevels[0]);

		for (int l = 1; l < kVoxelLevels; ++l)
		{
			const HRL_GIAtlasLayout src = HRL_GIAtlasLayout::For(std::max(1, res >> (l - 1)));
			const HRL_GIAtlasLayout dst = HRL_GIAtlasLayout::For(std::max(1, res >> l));
			down->Use();
			unit = 0;
			Bind(down, "uSource", g.voxelLevels[l - 1].texture, unit);
			down->SetInt("uSourceRes", src.resolution);
			down->SetInt("uSourceTiles", src.tilesPerRow);
			down->SetInt("uRes", dst.resolution);
			down->SetInt("uTiles", dst.tilesPerRow);
			down->SetInt("uSourceWidth", src.width);
			down->SetInt("uWidth", dst.width);
			glBindFramebuffer(GL_FRAMEBUFFER, g.voxelLevels[l].fbo);
			glViewport(0, 0, dst.width * 6, dst.height);
			Quad();
		}
	}
	g.voxelsLit = true;
	g.voxelLightingRevision = scene->gi_lighting_revision;
	++g.voxelSerial;
}

void GL_33_GI::UpdateLPV(SceneGPU& g, const hrl_scene_t* scene)
{
	const int res = g.data.voxelResolution / 2;
	if (res <= 0 || !g.voxelsLit) return;
	const HRL_GIAtlasLayout atlas = HRL_GIAtlasLayout::For(res);
	if (!g.lpvReady)
	{
		for (int s = 0; s < 3; ++s)
		{
			for (int c = 0; c < 3; ++c)
				g.lpvTex[s][c] = CreateTexture(GL_RGBA16F, atlas.width, atlas.height, GL_RGBA, GL_FLOAT, nullptr, GL_NEAREST);
			g.lpvFbo[s] = CreateFbo(g.lpvTex[s], 3);
		}
		g.lpvReady = true;
		g.lpvVoxelSerial = 0;
	}
	if (g.lpvVoxelSerial == g.voxelSerial) return;

	GL33_Shader* inject = Program("lpv_inject");
	GL33_Shader* propagate = Program("lpv_propagate");
	GL33_Shader* accumulate = Program("lpv_accumulate");
	if (!inject || !propagate || !accumulate) return;
	const HRL_GIAtlasLayout voxelAtlas = HRL_GIAtlasLayout::For(g.data.voxelResolution);
	auto setLPV = [&](GL33_Shader* s, int set, int& unit) {
		Bind(s, "uLPVR", g.lpvTex[set][0], unit);
		Bind(s, "uLPVG", g.lpvTex[set][1], unit);
		Bind(s, "uLPVB", g.lpvTex[set][2], unit);
		s->SetInt("uLPVRes", res);
		s->SetInt("uLPVTiles", atlas.tilesPerRow);
	};
	glViewport(0, 0, atlas.width, atlas.height);

	// Injection into the current set and the accumulation.
	int unit = 0;
	inject->Use();
	Bind(inject, "uRadiance", g.voxelLevels[1].texture, unit);
	inject->SetInt("uRadianceWidth", atlas.width);
	Bind(inject, "uVoxelNormal", g.voxelNormal, unit);
	inject->SetInt("uVoxelRes", g.data.voxelResolution);
	inject->SetInt("uVoxelTiles", voxelAtlas.tilesPerRow);
	inject->SetInt("uLPVRes", res);
	inject->SetInt("uLPVTiles", atlas.tilesPerRow);
	glBindFramebuffer(GL_FRAMEBUFFER, g.lpvFbo[0]);
	Quad();
	glBindFramebuffer(GL_FRAMEBUFFER, g.lpvFbo[2]);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	int current = 0;
	for (int i = 0; i <= kLPVIterations; ++i)
	{
		if (i > 0)
		{
			const int next = 1 - current;
			unit = 0;
			propagate->Use();
			setLPV(propagate, current, unit);
			Bind(propagate, "uOpacity", g.voxelLevels[1].texture, unit);
			propagate->SetInt("uOpacityWidth", atlas.width);
			glBindFramebuffer(GL_FRAMEBUFFER, g.lpvFbo[next]);
			Quad();
			current = next;
		}
		unit = 0;
		accumulate->Use();
		setLPV(accumulate, current, unit);
		glBindFramebuffer(GL_FRAMEBUFFER, g.lpvFbo[2]);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		Quad();
		glDisable(GL_BLEND);
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	g.lpvVoxelSerial = g.voxelSerial;
}

void GL_33_GI::UpdateScene(HRL_id sceneId, const hrl_scene_t* scene)
{
	if (!scene || !scene->global_illumination_enabled || !Supports(scene->global_illumination_method))
		return;
	auto& ptr = scenes_[sceneId];
	if (!ptr) ptr = std::make_unique<SceneGPU>();
	SceneGPU& g = *ptr;
	if (g.data.geometryRevision != scene->gi_geometry_revision)
	{
		ReleaseSceneGPU(g);
		HRL_UpdateGISceneData(sceneId, g.data);
	}
	if (!g.data.valid) return;
	UploadScene(g);
	++g.frame;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	const int method = scene->global_illumination_method;
	if (method == HRL_GI_DDGI) UpdateDDGI(g, scene);
	if (method == HRL_GI_VCT || method == HRL_GI_LPV) UpdateVoxels(g, scene);
	if (method == HRL_GI_LPV) UpdateLPV(g, scene);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glActiveTexture(GL_TEXTURE0);
}

// ---------------------------------------------------------------------------
// Screen space
// ---------------------------------------------------------------------------
bool GL_33_GI::Apply(const View& v)
{
	const hrl_scene_t* scene = v.scene;
	if (!scene || !scene->global_illumination_enabled || !Supports(scene->global_illumination_method) || !v.depth)
		return false;
	auto sit = scenes_.find(v.sceneId);
	const int method = scene->global_illumination_method;
	const bool needsWorld = method != HRL_GI_SSGI;
	SceneGPU* g = sit != scenes_.end() ? sit->second.get() : nullptr;
	if (needsWorld && (!g || !g->data.valid || !g->nodes)) return false;
	if (method == HRL_GI_DDGI && (!g->ddgiReady || g->ddgiFrames == 0)) return false;
	if ((method == HRL_GI_VCT) && !g->voxelsLit) return false;
	if (method == HRL_GI_LPV && (!g->lpvReady || g->lpvVoxelSerial == 0)) return false;
	// SSGI does not need the scene data, but the common uniforms reference it.
	static SceneGPU empty;
	if (!g) g = &empty;

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	GLuint indirect = 0;
	float scale = 1.f;
	int filter = 0;
	glm::vec2 targetSize(1.f);
	if (method == HRL_GI_SSGI || method == HRL_GI_VCT || method == HRL_GI_RAY_TRACING || method == HRL_GI_PATH_TRACING)
	{
		scale = ResolutionScale(method);
		auto& vptr = views_[v.viewportId];
		if (!vptr) vptr = std::make_unique<ViewGPU>();
		ViewGPU& view = *vptr;
		const int w = std::max(1, (int)std::ceil(v.sceneWidth * scale));
		const int h = std::max(1, (int)std::ceil(v.sceneHeight * scale));
		if (view.width != w || view.height != h || view.method != method || view.sceneId != v.sceneId)
		{
			ReleaseView(view);
			const GLenum format = method == HRL_GI_PATH_TRACING ? GL_RGBA32F : GL_RGBA16F;
			CreateTarget(&view.target, format, w, h, GL_NEAREST);
			for (auto& t : view.history) CreateTarget(&t, format, w, h, GL_LINEAR);
			view.width = w;
			view.height = h;
			view.method = method;
			view.sceneId = v.sceneId;
			view.scale = scale;
			view.historyValid = false;
		}
		targetSize = glm::vec2((float)w, (float)h);
		const int rx = (int)std::floor(v.x * scale), ry = (int)std::floor(v.y * scale);
		const int rw = std::max(1, (int)std::ceil((v.x + v.width) * scale) - rx);
		const int rh = std::max(1, (int)std::ceil((v.y + v.height) * scale) - ry);

		// 1. Indirect light of this frame.
		const char* key = method == HRL_GI_SSGI ? "ssgi" : method == HRL_GI_VCT ? "vct" : method == HRL_GI_RAY_TRACING ? "rt" : "pt";
		GL33_Shader* s = Program(key);
		if (!s) return false;
		int unit = 0;
		SetCommon(s, *g, scene, unit);
		SetScreen(s, v, scale, unit);
		if (method == HRL_GI_SSGI)
		{
			Bind(s, "uSceneColor", v.sceneColor, unit);
			s->SetInt("uRays", 4);
			s->SetInt("uSteps", 16);
			const glm::vec3 extent = g->data.valid ? g->data.boundsMax - g->data.boundsMin : glm::vec3(10.f);
			s->SetFloat("uMaxDistance", std::max(0.5f, 0.15f * std::max(extent.x, std::max(extent.y, extent.z))));
			s->SetFloat("uThickness", 0.15f);
		}
		else if (method == HRL_GI_VCT) SetVCT(s, *g, unit);
		else if (method == HRL_GI_PATH_TRACING) { s->SetInt("uMaxBounces", kPathTracingBounces); s->SetInt("uSamples", 1); }
		else if (method == HRL_GI_RAY_TRACING) { s->SetInt("uMaxBounces", kRayTracingBounces); s->SetInt("uSamples", 2); }
		glBindFramebuffer(GL_FRAMEBUFFER, view.target.fbo);
		glViewport(rx, ry, rw, rh);
		Quad();
		indirect = view.target.texture;

		// 2. Temporal accumulation.
		if (method != HRL_GI_VCT)
		{
			const glm::mat4 vp = v.projection * v.view;
			const glm::vec4 rect((float)v.x, (float)v.y, (float)v.width, (float)v.height);
			bool valid = view.historyValid && view.geometryRevision == scene->gi_geometry_revision;
			if (method == HRL_GI_PATH_TRACING)
			{
				valid = valid && view.lightingRevision == scene->gi_lighting_revision &&
					view.previousViewProjection == vp && view.previousRect == rect;
				view.sampleCount = valid ? view.sampleCount + 1.f : 0.f;
			}
			GL33_Shader* t = Program("temporal");
			if (!t) return false;
			unit = 0;
			t->Use();
			SetScreen(t, v, scale, unit);
			Bind(t, "uCurrent", view.target.texture, unit);
			Bind(t, "uHistory", view.history[view.current].texture, unit);
			t->SetMat4("uPrevViewProjection", view.previousViewProjection);
			t->SetVec3("uPrevCameraPos", view.previousCamera);
			t->SetVec4("uPrevViewportRect", view.previousRect);
			t->SetVec2("uTargetSize", targetSize);
			t->SetInt("uMode", method == HRL_GI_PATH_TRACING ? 1 : 0);
			t->SetInt("uHistoryValid", valid ? 1 : 0);
			t->SetFloat("uBlend", method == HRL_GI_RAY_TRACING ? 0.06f : 0.12f);
			t->SetFloat("uSampleCount", view.sampleCount);
			const int next = 1 - view.current;
			glBindFramebuffer(GL_FRAMEBUFFER, view.history[next].fbo);
			glViewport(rx, ry, rw, rh);
			Quad();
			view.current = next;
			indirect = view.history[next].texture;
			view.historyValid = true;
			view.previousViewProjection = vp;
			view.previousCamera = v.cameraPosition;
			view.previousRect = rect;
			view.geometryRevision = scene->gi_geometry_revision;
			view.lightingRevision = scene->gi_lighting_revision;
		}
		filter = method == HRL_GI_PATH_TRACING ? 0 : method == HRL_GI_VCT ? 1 : 3;
	}

	// 3. Composite.
	GL33_Shader* c = Program("composite");
	if (!c) return false;
	int unit = 0;
	SetCommon(c, *g, scene, unit);
	SetScreen(c, v, scale, unit);
	Bind(c, "uScene", v.sceneColor, unit);
	int source = 0;
	if (method == HRL_GI_DDGI) { source = 1; SetDDGI(c, *g, unit); }
	else if (method == HRL_GI_LPV) { source = 2; SetLPV(c, *g, unit); }
	else Bind(c, "uIndirect", indirect, unit);
	c->SetInt("uSource", source);
	c->SetInt("uFilterRadius", filter);
	c->SetVec2("uTargetSize", targetSize);
	c->SetFloat("uIntensity", scene->global_illumination_intensity);
	static const bool debugIndirect = std::getenv("HRL_GI_DEBUG_INDIRECT") != nullptr;
	c->SetInt("uDebugIndirect", debugIndirect ? 1 : 0);
	c->SetInt("uFogEnabled", scene->fog.enabled ? 1 : 0);
	c->SetInt("uFogMode", (int)scene->fog.mode);
	c->SetFloat("uFogStart", scene->fog.range_start);
	c->SetFloat("uFogEnd", scene->fog.range_end);
	c->SetFloat("uFogDensity", scene->fog.density);
	glBindFramebuffer(GL_FRAMEBUFFER, v.outputFbo);
	glViewport(v.x, v.y, v.width, v.height);
	Quad();
	glActiveTexture(GL_TEXTURE0);
	return true;
}
