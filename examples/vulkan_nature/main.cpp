// HRL example: an open landscape with foliage and grass (native Vulkan backend).
//
//   * a 1 x 1 km landscape (1025 x 1025 heights, continuous LOD);
//   * trees and bushes made of alpha tested leaf cards, rocks: tens of
//     thousands of instances culled and LOD-selected on the GPU, impostors
//     far away, wind;
//   * procedural grass around the camera that bends around the "player";
//   * shadow maps or ray traced shadows (R).
//
//   hrl_vulkan_nature                        opens a window
//   hrl_vulkan_nature --screenshot out.png   renders off-screen and writes a PNG
//   options: --size W H   --rt   --frames N   --grass DENSITY   --trees DENSITY
//
// Controls: right mouse button + mouse to look, W A S D to move, Q / E down /
// up, Shift faster. Left click paints bushes where the cursor points, Ctrl +
// left click erases them. R ray traced shadows, O occlusion culling, + / -
// wind, G grass on / off, P screenshot, Escape quits.
#include <hrl.h>
#include <hrl_vulkan.h>

#ifdef HRL_EXAMPLE_HAS_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const float kPi = 3.14159265358979f;
int g_errors = 0;

void OnError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	std::fprintf(stderr, "[HRL %s] %s: %s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), detail ? detail : "");
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL) ++g_errors;
}

// ---------------------------------------------------------------------------
// Procedural content
// ---------------------------------------------------------------------------
uint32_t Hash(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
	return x;
}
float Hash2(int x, int y) { return (float)(Hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u) & 0xFFFFFF) / 16777215.f; }

float ValueNoise(float x, float y)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	float fx = x - (float)ix, fy = y - (float)iy;
	fx = fx * fx * (3.f - 2.f * fx);
	fy = fy * fy * (3.f - 2.f * fy);
	const float a = Hash2(ix, iy), b = Hash2(ix + 1, iy), c = Hash2(ix, iy + 1), d = Hash2(ix + 1, iy + 1);
	return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

float Fbm(float x, float y, int octaves)
{
	float sum = 0.f, amplitude = 0.5f;
	for (int o = 0; o < octaves; ++o)
	{
		sum += amplitude * ValueNoise(x, y);
		x = x * 2.03f + 17.f;
		y = y * 2.03f - 9.f;
		amplitude *= 0.5f;
	}
	return sum;
}

// Rolling hills, a few ridges and a valley through the middle.
std::vector<float> MakeHeights(int n, float size)
{
	std::vector<float> h((size_t)n * n);
	for (int z = 0; z < n; ++z)
		for (int x = 0; x < n; ++x)
		{
			const float wx = ((float)x / (float)(n - 1) - 0.5f) * size, wz = ((float)z / (float)(n - 1) - 0.5f) * size;
			const float hills = Fbm(wx / 220.f, wz / 220.f, 6) * 70.f;
			const float ridge = 1.f - std::abs(Fbm(wx / 400.f + 3.f, wz / 400.f, 4) * 2.f - 1.f);
			const float valley = 1.f - std::exp(-(wx * 0.004f + std::sin(wz * 0.006f)) * (wx * 0.004f + std::sin(wz * 0.006f)) * 4.f);
			h[(size_t)z * n + x] = (hills + ridge * ridge * 45.f) * (0.35f + 0.65f * valley);
		}
	return h;
}

struct MeshData
{
	std::vector<HRL_Vertex3D> vertices;
	std::vector<HRL_uint> indices;
};

HRL_Vertex3D Vertex(const float p[3], const float n[3], float u, float v, const float t[3])
{
	HRL_Vertex3D r{};
	for (int i = 0; i < 3; ++i) { r.position[i] = p[i]; r.normal[i] = n[i]; r.tangent[i] = t[i]; }
	r.bitangent[0] = n[1] * t[2] - n[2] * t[1];
	r.bitangent[1] = n[2] * t[0] - n[0] * t[2];
	r.bitangent[2] = n[0] * t[1] - n[1] * t[0];
	r.uv[0] = u;
	r.uv[1] = v;
	return r;
}

// Tapered cylinder along +Y.
MeshData MakeTrunk(float r0, float r1, float height, int sides)
{
	MeshData m;
	for (int k = 0; k <= sides; ++k)
	{
		const float a = (float)k / (float)sides * 2.f * kPi, c = std::cos(a), s = std::sin(a);
		const float slope = (r0 - r1) / height, len = std::sqrt(1.f + slope * slope);
		const float n[3] = {c / len, slope / len, s / len}, t[3] = {-s, 0.f, c};
		const float p0[3] = {c * r0, -0.3f, s * r0}, p1[3] = {c * r1, height, s * r1};
		m.vertices.push_back(Vertex(p0, n, (float)k / (float)sides * 2.f, 0.f, t));
		m.vertices.push_back(Vertex(p1, n, (float)k / (float)sides * 2.f, height * 0.5f, t));
	}
	for (int k = 0; k < sides; ++k)
	{
		const HRL_uint a = 2u * (HRL_uint)k;
		m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 2, a + 1, a + 3});
	}
	return m;
}

// Cloud of leaf cards (two sided quads) around an ellipsoid: a crown, or a bush.
MeshData MakeLeafCards(float cx, float cy, float cz, float rx, float ry, float rz, int cards, float cardSize, uint32_t seed)
{
	MeshData m;
	for (int i = 0; i < cards; ++i)
	{
		auto rnd = [&]() { seed = Hash(seed + 0x9e3779b9u); return (float)(seed & 0xFFFFFF) / 16777215.f; };
		// Point in the ellipsoid, biased towards its surface.
		const float u = rnd() * 2.f * kPi, v = std::acos(1.f - 2.f * rnd()), r = std::pow(rnd(), 0.35f);
		const float dx = std::sin(v) * std::cos(u), dy = std::cos(v), dz = std::sin(v) * std::sin(u);
		const float center[3] = {cx + dx * rx * r, cy + dy * ry * r, cz + dz * rz * r};
		// Card facing outwards, rolled randomly; its normal points away from the centre (soft lighting).
		const float n[3] = {dx, dy * 0.5f + 0.5f, dz};
		const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		const float normal[3] = {n[0] / nl, n[1] / nl, n[2] / nl};
		const float roll = rnd() * 2.f * kPi;
		float t[3] = {-dz, 0.f, dx};
		const float tl = std::sqrt(t[0] * t[0] + t[2] * t[2]) + 1e-4f;
		t[0] /= tl; t[2] /= tl;
		const float b[3] = {normal[1] * t[2] - normal[2] * t[1], normal[2] * t[0] - normal[0] * t[2], normal[0] * t[1] - normal[1] * t[0]};
		const float ca = std::cos(roll), sa = std::sin(roll);
		float e0[3], e1[3];
		for (int k = 0; k < 3; ++k)
		{
			e0[k] = (t[k] * ca + b[k] * sa) * cardSize;
			e1[k] = (-t[k] * sa + b[k] * ca) * cardSize;
		}
		const HRL_uint base = (HRL_uint)m.vertices.size();
		const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
		for (const auto& c : corners)
		{
			const float p[3] = {center[0] + e0[0] * c[0] + e1[0] * c[1], center[1] + e0[1] * c[0] + e1[1] * c[1],
				center[2] + e0[2] * c[0] + e1[2] * c[1]};
			const float tangent[3] = {e0[0] / cardSize, e0[1] / cardSize, e0[2] / cardSize};
			m.vertices.push_back(Vertex(p, normal, c[0] * 0.5f + 0.5f, c[1] * 0.5f + 0.5f, tangent));
		}
		m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
	}
	return m;
}

// Lumpy rock: a displaced sphere, flat at the bottom.
MeshData MakeRock(uint32_t seed)
{
	MeshData m;
	const int rings = 10, sectors = 14;
	for (int j = 0; j <= rings; ++j)
	{
		const float v = (float)j / (float)rings, theta = v * kPi;
		for (int k = 0; k <= sectors; ++k)
		{
			const float u = (float)k / (float)sectors, phi = u * 2.f * kPi;
			const float d[3] = {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
			const int kk = k % sectors;
			const float bump = 0.75f + 0.5f * Hash2(kk * 7 + (int)seed, j * 13);
			const float p[3] = {d[0] * bump, std::max(d[1] * bump * 0.6f, -0.1f), d[2] * bump};
			const float t[3] = {-std::sin(phi), 0.f, std::cos(phi)};
			m.vertices.push_back(Vertex(p, d, u * 3.f, v * 2.f, t));
		}
	}
	for (int j = 0; j < rings; ++j)
		for (int k = 0; k < sectors; ++k)
		{
			const HRL_uint a = (HRL_uint)(j * (sectors + 1) + k), b = a + (HRL_uint)sectors + 1;
			m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	return m;
}

// Leaves on a transparent background: the cards are alpha tested.
HRL_id MakeLeafTexture(float r, float g, float b, uint32_t seed)
{
	const int size = 128;
	std::vector<unsigned char> rgba((size_t)size * size * 4, 0);
	for (int i = 0; i < 26; ++i)
	{
		seed = Hash(seed + 0x9e3779b9u);
		const float cx = 16.f + (float)(seed % 96), cy = 16.f + (float)((seed >> 8) % 96);
		const float angle = (float)((seed >> 16) % 628) / 100.f, len = 14.f + (float)((seed >> 4) % 10), wid = len * 0.45f;
		const float shade = 0.7f + 0.3f * (float)((seed >> 12) % 100) / 100.f;
		for (int y = 0; y < size; ++y)
			for (int x = 0; x < size; ++x)
			{
				const float dx = (float)x - cx, dy = (float)y - cy;
				const float a = dx * std::cos(angle) + dy * std::sin(angle), c = -dx * std::sin(angle) + dy * std::cos(angle);
				if ((a * a) / (len * len) + (c * c) / (wid * wid) > 1.f) continue;
				unsigned char* p = &rgba[((size_t)y * size + x) * 4];
				const float vein = std::abs(c) < 0.8f ? 0.8f : 1.f;
				p[0] = (unsigned char)std::min(255.f, r * shade * vein * 255.f);
				p[1] = (unsigned char)std::min(255.f, g * shade * vein * 255.f);
				p[2] = (unsigned char)std::min(255.f, b * shade * vein * 255.f);
				p[3] = 255;
			}
	}
	return HRL_CreateTextureFromPixels(rgba.data(), size, size);
}

HRL_id MakeNoiseTexture(float r, float g, float b, float variation)
{
	const int size = 256;
	std::vector<unsigned char> rgba((size_t)size * size * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			// Tileable: the noise wraps every 16 cells.
			const float n = Fbm((float)x / 16.f, (float)y / 16.f, 3) * 2.f - 0.5f;
			const float k = 1.f + (n - 0.25f) * variation;
			unsigned char* p = &rgba[((size_t)y * size + x) * 4];
			p[0] = (unsigned char)std::clamp(r * k * 255.f, 0.f, 255.f);
			p[1] = (unsigned char)std::clamp(g * k * 255.f, 0.f, 255.f);
			p[2] = (unsigned char)std::clamp(b * k * 255.f, 0.f, 255.f);
			p[3] = 255;
		}
	return HRL_CreateTextureFromPixels(rgba.data(), size, size);
}

HRL_id MakeMaterial(HRL_id albedo, float roughness, bool leaves)
{
	const HRL_id m = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	if (albedo != HRL_INVALID_ID) HRL_MaterialSetTexture(m, HRL_T_ALBEDO, albedo);
	HRL_MaterialSetInt(m, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(m, "RoughnessValue", roughness);
	HRL_MaterialSetInt(m, "MetallicUseValue", 1);
	HRL_MaterialSetFloat(m, "MetallicValue", 0.f);
	if (leaves)
	{
		// Alpha tested: the opacity value (1) times the alpha of the albedo texture.
		HRL_MaterialSetInt(m, "OpacityUseValue", 1);
		HRL_MaterialSetFloat(m, "OpacityValue", 1.f);
		HRL_MaterialSetBool(m, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
	}
	return m;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
struct Options
{
	int width = 1280, height = 720;
	std::string screenshot;
	bool rayTracedShadows = false;
	int frames = 0;
	float grassDensity = 60.f;
	float treeDensity = 0.004f;
};

struct World
{
	HRL_id scene = 0, camera = 0, land = 0, grass = 0, post = 0;
	HRL_id trees = 0, bushes = 0, rocks = 0;
	HRL_FoliageScatter bushScatter{};
	float camX = -40.f, camY = 0.f, camZ = -120.f, pitch = -8.f, yaw = 70.f;
	float wind = 0.5f;
};

void CreateGrass(World& w, float density)
{
	w.grass = HRL_CreateGrass(w.land);
	HRL_SetGrassDensity(w.grass, density);
	HRL_SetGrassBladeSize(w.grass, 0.55f, 0.05f, 0.5f);
	HRL_SetGrassColors(w.grass, 0.05f, 0.13f, 0.02f, 0.38f, 0.5f, 0.14f);
	HRL_SetGrassDistance(w.grass, 70.f);
	HRL_SetGrassMaxSlope(w.grass, 35.f);
	HRL_SetGrassCastShadows(w.grass, HRL_TRUE);
}

void AddFoliageMesh(HRL_id foliage, const MeshData& m, HRL_id material)
{
	HRL_AddFoliageMesh(foliage, m.vertices.data(), m.vertices.size(), m.indices.data(), m.indices.size(), material);
}

void BuildWorld(World& w, const Options& opt)
{
	w.scene = HRL_CreateScene(HRL_TRUE);
	w.camera = HRL_CreateCamera(w.scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(w.camera, 60.f);
	HRL_SetCameraNearPlane(w.camera, 0.1f);
	HRL_SetCameraFarPlane(w.camera, 2500.f);
	const HRL_id viewport = HRL_CreateViewport(w.scene, w.camera, 0.f, 0.f, 1.f, 1.f);
	w.post = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetFloat(w.post, "exposure", -0.3f);
	HRL_MaterialSetFloat(w.post, "bloomStrength", 0.4f);
	HRL_CreatePostProcess(viewport, w.post, 0);

	HRL_SetSkySphereEnabled(w.scene, HRL_TRUE);
	HRL_SetSkySphereColors(w.scene, 0.12f, 0.28f, 0.62f, 0.62f, 0.72f, 0.85f, 0.3f, 0.32f, 0.3f);
	HRL_SetEnvironmentMappingEnabled(w.scene, HRL_TRUE);
	HRL_SetFogEnabled(w.scene, HRL_TRUE);
	HRL_SetFogMode(w.scene, HRL_FOG_EXPONENTIAL);
	HRL_SetFogColor(w.scene, 0.62f, 0.72f, 0.85f);
	HRL_SetFogDensity(w.scene, 0.0012f);
	const HRL_id sun = HRL_CreateLight(w.scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -32.f, 55.f, 0.f);
	HRL_SetLightColor(sun, 1.f, 0.93f, 0.8f);
	HRL_SetLightIntensity(sun, 4.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	HRL_SetLightShadowResolution(sun, 2048);
	HRL_Vulkan_SetShadowDistance(220.f);

	// ---- Landscape: 1 km, one height per metre ----
	const int n = 1025;
	const float size = 1024.f;
	const std::vector<float> heights = MakeHeights(n, size);
	w.land = HRL_CreateLandscapeFromHeights(w.scene, heights.data(), (HRL_uint)n, (HRL_uint)n);
	HRL_SetLandscapeSize(w.land, size, size);
	HRL_SetLandscapeUVScale(w.land, 160.f, 160.f);
	HRL_SetLandscapeMaterial(w.land, MakeMaterial(MakeNoiseTexture(0.32f, 0.36f, 0.18f, 0.8f), 0.95f, false));

	// ---- Grass ----
	if (opt.grassDensity > 0.f) CreateGrass(w, opt.grassDensity);

	// ---- Trees: trunk + crown of leaf cards ----
	const HRL_id leaves = MakeMaterial(MakeLeafTexture(0.25f, 0.5f, 0.12f, 11u), 0.7f, true);
	const HRL_id bark = MakeMaterial(MakeNoiseTexture(0.3f, 0.22f, 0.15f, 1.2f), 0.9f, false);
	w.trees = HRL_CreateFoliage(w.scene);
	AddFoliageMesh(w.trees, MakeTrunk(0.35f, 0.18f, 7.f, 10), bark);
	AddFoliageMesh(w.trees, MakeLeafCards(0.f, 7.5f, 0.f, 3.2f, 3.f, 3.2f, 260, 0.9f, 5u), leaves);
	HRL_SetFoliageDrawDistance(w.trees, 900.f);
	HRL_SetFoliageLODDistance(w.trees, 35.f);
	HRL_SetFoliageImpostorDistance(w.trees, 140.f);
	HRL_FoliageScatter s = HRL_DefaultFoliageScatter();
	s.density = opt.treeDensity;
	s.min_scale = 0.75f;
	s.max_scale = 1.35f;
	s.max_slope = 28.f;
	s.min_spacing = 6.f;
	s.sink = 0.2f;
	s.random_tilt = 3.f;
	s.seed = 1;
	const size_t treeCount = HRL_ScatterFoliage(w.trees, w.land, &s);

	// ---- Bushes ----
	const HRL_id bushLeaves = MakeMaterial(MakeLeafTexture(0.3f, 0.45f, 0.1f, 23u), 0.75f, true);
	w.bushes = HRL_CreateFoliage(w.scene);
	AddFoliageMesh(w.bushes, MakeLeafCards(0.f, 0.6f, 0.f, 1.1f, 0.8f, 1.1f, 70, 0.45f, 9u), bushLeaves);
	HRL_SetFoliageDrawDistance(w.bushes, 220.f);
	HRL_SetFoliageLODDistance(w.bushes, 20.f);
	HRL_SetFoliageWindResponse(w.bushes, 1.6f);
	w.bushScatter = HRL_DefaultFoliageScatter();
	w.bushScatter.density = 0.02f;
	w.bushScatter.min_scale = 0.6f;
	w.bushScatter.max_scale = 1.3f;
	w.bushScatter.max_slope = 32.f;
	w.bushScatter.min_spacing = 2.f;
	w.bushScatter.align_to_normal = 0.5f;
	w.bushScatter.seed = 2;
	const size_t bushCount = HRL_ScatterFoliage(w.bushes, w.land, &w.bushScatter);

	// ---- Rocks: rigid, follow the slope, half buried ----
	w.rocks = HRL_CreateFoliage(w.scene);
	AddFoliageMesh(w.rocks, MakeRock(3u), MakeMaterial(MakeNoiseTexture(0.42f, 0.4f, 0.37f, 1.5f), 0.85f, false));
	HRL_SetFoliageWindResponse(w.rocks, 0.f);
	HRL_SetFoliageDrawDistance(w.rocks, 400.f);
	HRL_FoliageScatter r = HRL_DefaultFoliageScatter();
	r.density = 0.003f;
	r.min_scale = 0.5f;
	r.max_scale = 2.5f;
	r.max_slope = 60.f;
	r.align_to_normal = 1.f;
	r.sink = 0.25f;
	r.random_tilt = 15.f;
	r.seed = 3;
	const size_t rockCount = HRL_ScatterFoliage(w.rocks, w.land, &r);
	std::printf("trees %zu, bushes %zu, rocks %zu\n", treeCount, bushCount, rockCount);

	HRL_SetWind(w.scene, 1.f, 0.4f, w.wind, 0.6f);
	HRL_Vulkan_SetRayTracedShadows(opt.rayTracedShadows ? HRL_TRUE : HRL_FALSE);
	float ground = 0.f;
	HRL_GetLandscapeHeightAt(w.land, w.camX, w.camZ, &ground);
	w.camY = ground + 2.2f;
}

void UpdateCamera(World& w)
{
	HRL_SetCameraLocation(w.camera, w.camX, w.camY, w.camZ);
	HRL_SetCameraRotation(w.camera, w.pitch, w.yaw, 0.f);
	// The "player" bends the grass and the bushes around the point under the camera.
	float ground = 0.f;
	if (HRL_GetLandscapeHeightAt(w.land, w.camX, w.camZ, &ground))
		HRL_SetFoliageInteractor(w.scene, 0, w.camX, ground, w.camZ, 1.2f);
}

void PrintStats(const World& w)
{
	const char* names[3] = {"trees", "bushes", "rocks"};
	const HRL_id kinds[3] = {w.trees, w.bushes, w.rocks};
	for (int i = 0; i < 3; ++i)
	{
		HRL_VulkanFoliageStats s{};
		HRL_Vulkan_GetFoliageStats(kinds[i], &s);
		std::printf("%s: %zu instances, drawn LOD0 %u, LOD1 %u, LOD2 %u, impostors %u\n", names[i],
			HRL_GetFoliageInstanceCount(kinds[i]), s.lod_instances[0], s.lod_instances[1], s.lod_instances[2], s.impostor_instances);
	}
}

#ifdef HRL_EXAMPLE_HAS_GLFW
extern "C" int glfwCreateWindowSurface(void* instance, GLFWwindow* window, const void* allocator, void* surface);

int CreateSurface(void* instance, void* surfaceOut, void* user)
{
	return glfwCreateWindowSurface(instance, static_cast<GLFWwindow*>(user), nullptr, surfaceOut);
}

// Point of the landscape under the cursor: march along the view ray.
bool CursorOnGround(const World& w, double mx, double my, int width, int height, float& x, float& z)
{
	const float p = w.pitch * kPi / 180.f, y = w.yaw * kPi / 180.f;
	const float f[3] = {std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y)};
	const float r[3] = {-std::sin(y), 0.f, std::cos(y)};
	const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
	const float t = std::tan(60.f * 0.5f * kPi / 180.f), aspect = (float)width / (float)std::max(height, 1);
	const float nx = (float)(2.0 * mx / width - 1.0) * t * aspect, ny = (float)(1.0 - 2.0 * my / height) * t;
	float d[3] = {f[0] + r[0] * nx + u[0] * ny, f[1] + r[1] * nx + u[1] * ny, f[2] + r[2] * nx + u[2] * ny};
	const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	for (float s = 0.5f; s < 600.f; s *= 1.02f)
	{
		const float px = w.camX + d[0] / len * s, py = w.camY + d[1] / len * s, pz = w.camZ + d[2] / len * s;
		float ground = 0.f;
		if (HRL_GetLandscapeHeightAt(w.land, px, pz, &ground) && py <= ground) { x = px; z = pz; return true; }
	}
	return false;
}

int RunWindow(Options& opt)
{
	if (!glfwInit() || !glfwVulkanSupported()) { std::fprintf(stderr, "GLFW / Vulkan not available\n"); return 1; }
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "HRL - Vulkan nature", nullptr, nullptr);
	if (!window) { glfwTerminate(); return 1; }
	uint32_t extensionCount = 0;
	const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
	HRL_RegisterErrorCallback(OnError);
	HRL_Vulkan_SetInstanceExtensions(extensions, extensionCount);
	HRL_Vulkan_SetSurfaceCallback(CreateSurface, window);
	HRL_Init(HRL_VULKAN);
	int fbw = 0, fbh = 0;
	glfwGetFramebufferSize(window, &fbw, &fbh);
	HRL_InitContext((HRL_uint)fbw, (HRL_uint)fbh, nullptr);
	if (g_errors) { glfwTerminate(); return 1; }
	std::printf("GPU: %s | hardware ray tracing: %s\n", HRL_Vulkan_GetDeviceName(), HRL_Vulkan_IsRayTracingSupported() ? "yes" : "no");

	World w;
	BuildWorld(w, opt);
	double last = glfwGetTime(), lastX = 0, lastY = 0, statsTime = last;
	int statsFrames = 0, shot = 0;
	bool looking = false;
	bool previous[GLFW_KEY_LAST + 1] = {};
	auto pressed = [&](int key)
	{
		const bool down = glfwGetKey(window, key) == GLFW_PRESS;
		const bool edge = down && !previous[key];
		previous[key] = down;
		return edge;
	};
	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();
		const double now = glfwGetTime();
		const float dt = (float)(now - last);
		last = now;
		int width = 0, height = 0;
		glfwGetFramebufferSize(window, &width, &height);
		if (width != fbw || height != fbh) { fbw = width; fbh = height; HRL_WindowResizeCallback(width, height); }
		if (width == 0 || height == 0) { glfwWaitEvents(); continue; }
		if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);

		double mx = 0, my = 0;
		glfwGetCursorPos(window, &mx, &my);
		const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
		if (rmb && looking)
		{
			w.yaw += (float)(mx - lastX) * 0.15f;
			w.pitch = std::clamp(w.pitch - (float)(my - lastY) * 0.15f, -89.f, 89.f);
		}
		looking = rmb;
		lastX = mx; lastY = my;
		const float p = w.pitch * kPi / 180.f, yw = w.yaw * kPi / 180.f;
		const float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 40.f : 8.f) * dt;
		auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS ? 1.f : 0.f; };
		const float forward = key(GLFW_KEY_W) - key(GLFW_KEY_S), right = key(GLFW_KEY_D) - key(GLFW_KEY_A), up = key(GLFW_KEY_E) - key(GLFW_KEY_Q);
		w.camX += (std::cos(p) * std::cos(yw) * forward - std::sin(yw) * right) * speed;
		w.camY += (std::sin(p) * forward + up) * speed;
		w.camZ += (std::cos(p) * std::sin(yw) * forward + std::cos(yw) * right) * speed;
		float ground = 0.f;
		if (HRL_GetLandscapeHeightAt(w.land, w.camX, w.camZ, &ground)) w.camY = std::max(w.camY, ground + 1.f);

		// Brush: paint / erase bushes under the cursor.
		if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
		{
			float bx = 0.f, bz = 0.f;
			int ww = 0, wh = 0;
			glfwGetWindowSize(window, &ww, &wh);
			if (CursorOnGround(w, mx, my, ww, wh, bx, bz))
			{
				if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) HRL_EraseFoliage(w.bushes, bx, bz, 5.f);
				else
				{
					w.bushScatter.density = 0.15f;
					HRL_PaintFoliage(w.bushes, w.land, bx, bz, 5.f, &w.bushScatter);
				}
			}
		}
		if (pressed(GLFW_KEY_R)) HRL_Vulkan_SetRayTracedShadows(!HRL_Vulkan_GetRayTracedShadows());
		if (pressed(GLFW_KEY_O)) HRL_Vulkan_SetOcclusionCulling(!HRL_Vulkan_GetOcclusionCulling());
		if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) w.wind = std::min(w.wind + 0.1f, 1.5f);
		if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) w.wind = std::max(w.wind - 0.1f, 0.f);
		HRL_SetWind(w.scene, 1.f, 0.4f, w.wind, 0.6f);
		if (pressed(GLFW_KEY_G))
		{
			if (w.grass) { HRL_DeleteGrass(w.grass); w.grass = 0; }
			else CreateGrass(w, opt.grassDensity > 0.f ? opt.grassDensity : 60.f);
		}
		if (pressed(GLFW_KEY_P))
		{
			const std::string path = "hrl_nature_" + std::to_string(shot++) + ".png";
			HRL_TakeScreenshot(w.scene, path.c_str());
			std::printf("screenshot: %s\n", path.c_str());
		}

		UpdateCamera(w);
		HRL_BeginFrame();
		HRL_EndFrame();
		++statsFrames;
		if (now - statsTime > 2.0)
		{
			std::printf("%.1f fps | ray traced shadows %s | occlusion culling %s | wind %.1f\n", statsFrames / (now - statsTime),
				HRL_Vulkan_GetRayTracedShadows() ? "on" : "off", HRL_Vulkan_GetOcclusionCulling() ? "on" : "off", w.wind);
			PrintStats(w);
			statsTime = now;
			statsFrames = 0;
		}
		if (opt.frames > 0 && --opt.frames == 0) break;
	}
	HRL_Shutdown();
	glfwDestroyWindow(window);
	glfwTerminate();
	return g_errors ? 1 : 0;
}
#endif

int RunOffscreen(const Options& opt)
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext((HRL_uint)opt.width, (HRL_uint)opt.height, nullptr);
	if (g_errors) return 1;
	std::printf("GPU: %s | hardware ray tracing: %s\n", HRL_Vulkan_GetDeviceName(), HRL_Vulkan_IsRayTracingSupported() ? "yes" : "no");
	if (opt.rayTracedShadows && !HRL_Vulkan_IsRayTracingSupported())
	{
		std::fprintf(stderr, "--rt requested but hardware ray tracing is not available\n");
		HRL_Shutdown();
		return 2;
	}
	World w;
	BuildWorld(w, opt);
	UpdateCamera(w);
	const int frames = opt.frames > 0 ? opt.frames : 4;
	for (int i = 0; i < frames; ++i)
	{
		HRL_BeginFrame();
		if (i == frames - 1) HRL_TakeScreenshot(w.scene, opt.screenshot.c_str());
		HRL_EndFrame();
	}
	PrintStats(w);
	HRL_Shutdown();
	std::printf("wrote %s\n", opt.screenshot.c_str());
	return g_errors ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
	Options opt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--screenshot" && i + 1 < argc) opt.screenshot = argv[++i];
		else if (a == "--size" && i + 2 < argc) { opt.width = std::atoi(argv[++i]); opt.height = std::atoi(argv[++i]); }
		else if (a == "--frames" && i + 1 < argc) opt.frames = std::atoi(argv[++i]);
		else if (a == "--grass" && i + 1 < argc) opt.grassDensity = (float)std::atof(argv[++i]);
		else if (a == "--trees" && i + 1 < argc) opt.treeDensity = (float)std::atof(argv[++i]);
		else if (a == "--rt") opt.rayTracedShadows = true;
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
	}
	if (opt.width < 16 || opt.height < 16 || opt.frames < 0 || opt.treeDensity <= 0.f) { std::fprintf(stderr, "invalid options\n"); return 1; }
	if (!opt.screenshot.empty()) return RunOffscreen(opt);
#ifdef HRL_EXAMPLE_HAS_GLFW
	return RunWindow(opt);
#else
	std::fprintf(stderr, "built without GLFW: use --screenshot <file.png>\n");
	return 1;
#endif
}
