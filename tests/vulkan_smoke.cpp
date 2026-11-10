// Off-screen integration test of the native Vulkan backend.
//
// Exercises resource creation / deletion, textures, automatic LODs, several
// viewports, shadows (maps and ray traced when available), fog, debug
// primitives, window resize, picking and screenshots. Any HRL error (which
// includes Vulkan validation messages when HRL_VULKAN_VALIDATION=1) fails the test.
#include <hrl.h>
#include <hrl_vulkan.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int g_errors = 0;
int g_failures = 0;

void OnError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	// Missing validation layers are not a failure of HRL.
	const std::string text = detail ? detail : "";
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL ||
		text.find("Vulkan validation") != std::string::npos)
	{
		++g_errors;
		std::fprintf(stderr, "[HRL %s] %s: %.600s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), text.c_str());
	}
}

#define CHECK(cond) do { if (!(cond)) { ++g_failures; std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

// ---- Minimal PNG writer (stored deflate blocks) for in-memory test textures --
uint32_t Crc32(const unsigned char* data, size_t size, uint32_t crc = 0)
{
	crc = ~crc;
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
	for (int s = 24; s >= 0; s -= 8) out.push_back((unsigned char)(v >> s));
}

void PutChunk(std::vector<unsigned char>& out, const char* type, const std::vector<unsigned char>& data)
{
	PutU32(out, (uint32_t)data.size());
	std::vector<unsigned char> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	out.insert(out.end(), body.begin(), body.end());
	PutU32(out, Crc32(body.data(), body.size()));
}

std::vector<unsigned char> EncodePNG(int width, int height, const std::vector<unsigned char>& rgba)
{
	std::vector<unsigned char> raw;
	for (int y = 0; y < height; ++y)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgba.begin() + (size_t)y * width * 4, rgba.begin() + (size_t)(y + 1) * width * 4);
	}
	std::vector<unsigned char> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (size_t pos = 0; pos < raw.size();)
	{
		const size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
		z.push_back(pos + n == raw.size() ? 1 : 0);
		z.push_back((unsigned char)(n & 0xFF)); z.push_back((unsigned char)(n >> 8));
		z.push_back((unsigned char)(~n & 0xFF)); z.push_back((unsigned char)((~n >> 8) & 0xFF));
		for (size_t i = 0; i < n; ++i) { a = (a + raw[pos + i]) % 65521u; b = (b + a) % 65521u; }
		z.insert(z.end(), raw.begin() + (long)pos, raw.begin() + (long)(pos + n));
		pos += n;
	}
	PutU32(z, (b << 16) | a);

	std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<unsigned char> ihdr;
	PutU32(ihdr, (uint32_t)width);
	PutU32(ihdr, (uint32_t)height);
	ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
	PutChunk(png, "IHDR", ihdr);
	PutChunk(png, "IDAT", z);
	PutChunk(png, "IEND", {});
	return png;
}

HRL_id MakeCheckerTexture(int size, unsigned char r, unsigned char g, unsigned char b)
{
	std::vector<unsigned char> px((size_t)size * size * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x)
		{
			const bool on = ((x / 8) + (y / 8)) % 2 == 0;
			unsigned char* p = &px[((size_t)y * size + x) * 4];
			p[0] = on ? r : 40; p[1] = on ? g : 40; p[2] = on ? b : 40; p[3] = 255;
		}
	const std::vector<unsigned char> png = EncodePNG(size, size, px);
	return HRL_CreateTexture(reinterpret_cast<const char*>(png.data()), png.size());
}

// ---- Geometry ---------------------------------------------------------------
struct MeshData { std::vector<HRL_Vertex3D> v; std::vector<HRL_uint> i; };

MeshData MakeSphere(float radius, int rings, int sectors)
{
	const float pi = 3.14159265f;
	MeshData m;
	for (int r = 0; r <= rings; ++r)
		for (int s = 0; s <= sectors; ++s)
		{
			const float theta = pi * (float)r / (float)rings, phi = 2.f * pi * (float)s / (float)sectors;
			HRL_Vertex3D v{};
			v.normal[0] = std::sin(theta) * std::cos(phi); v.normal[1] = std::cos(theta); v.normal[2] = std::sin(theta) * std::sin(phi);
			for (int k = 0; k < 3; ++k) v.position[k] = v.normal[k] * radius;
			v.uv[0] = (float)s / (float)sectors; v.uv[1] = 1.f - (float)r / (float)rings;
			v.tangent[0] = -std::sin(phi); v.tangent[2] = std::cos(phi);
			v.bitangent[0] = v.normal[1] * v.tangent[2] - v.normal[2] * v.tangent[1];
			v.bitangent[1] = v.normal[2] * v.tangent[0] - v.normal[0] * v.tangent[2];
			v.bitangent[2] = v.normal[0] * v.tangent[1] - v.normal[1] * v.tangent[0];
			m.v.push_back(v);
		}
	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < sectors; ++s)
		{
			const HRL_uint a = (HRL_uint)(r * (sectors + 1) + s), b = a + (HRL_uint)sectors + 1;
			m.i.insert(m.i.end(), {a, a + 1, b, a + 1, b + 1, b});
		}
	return m;
}

MeshData MakeGround(float half)
{
	MeshData m;
	const float p[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
	for (int k = 0; k < 4; ++k)
	{
		HRL_Vertex3D v{};
		v.position[0] = p[k][0]; v.position[2] = p[k][1];
		v.normal[1] = 1.f; v.tangent[0] = 1.f; v.bitangent[2] = -1.f;
		v.uv[0] = p[k][0] / half; v.uv[1] = p[k][1] / half;
		m.v.push_back(v);
	}
	m.i = {0, 2, 1, 0, 3, 2}; // counter-clockwise seen from +Y
	return m;
}

HRL_id AddMesh(HRL_id scene, const MeshData& d, HRL_id material, float x, float y, float z)
{
	const HRL_id mesh = HRL_CreateMesh3D(scene, d.v.data(), d.v.size(), d.i.data(), d.i.size());
	HRL_SetMeshMaterial(mesh, material);
	HRL_SetMeshLocation(mesh, x, y, z);
	return mesh;
}

void Frames(int count)
{
	for (int i = 0; i < count; ++i) { HRL_BeginFrame(); HRL_EndFrame(); }
}
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(320, 200, nullptr);
	if (g_errors) { std::fprintf(stderr, "Vulkan initialisation failed\n"); return 1; }
	const bool rt = HRL_Vulkan_IsRayTracingSupported() != 0;
	std::printf("GPU: %s, ray tracing: %s\n", HRL_Vulkan_GetDeviceName(), rt ? "yes" : "no");

	const HRL_id scene = HRL_CreateScene(HRL_TRUE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraNearPlane(camera, 0.1f);
	HRL_SetCameraFarPlane(camera, 200.f);
	HRL_SetCameraLocation(camera, 0.f, 2.f, 6.f);
	HRL_SetCameraRotation(camera, -15.f, -90.f, 0.f); // looks towards -Z, slightly down
	const HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	HRL_SetSkySphereEnabled(scene, HRL_TRUE);
	HRL_SetEnvironmentMappingEnabled(scene, HRL_TRUE);

	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -50.f, 30.f, 0.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	HRL_SetLightShadowResolution(sun, 512);
	const HRL_id spot = HRL_CreateLight(scene, HRL_SPOT_LIGHT);
	HRL_SetLightLocation(spot, 2.f, 5.f, 2.f);
	HRL_SetLightRotation(spot, -60.f, -135.f, 0.f);
	HRL_SetLightCastShadows(spot, HRL_TRUE);
	HRL_SetLightShadowResolution(spot, 512);
	const HRL_id point = HRL_CreateLight(scene, HRL_POINT_LIGHT);
	HRL_SetLightLocation(point, -2.f, 1.f, 1.f);
	HRL_SetLightCastShadows(point, HRL_TRUE);

	const HRL_id checker = MakeCheckerTexture(64, 230, 200, 60);
	CHECK(checker != HRL_INVALID_ID);
	CHECK(HRL_IsValidTexture(checker));
	int tw = 0, th = 0;
	HRL_GetTextureSize(checker, &tw, &th);
	CHECK(tw == 64 && th == 64);

	const HRL_id groundMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetTexture(groundMat, HRL_T_ALBEDO, checker);
	const HRL_id redMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(redMat, "TintColor", 0.9f, 0.1f, 0.1f);
	HRL_MaterialSetInt(redMat, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(redMat, "RoughnessValue", 0.3f);
	const HRL_id twoSided = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetBool(twoSided, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
	HRL_MaterialSetInt(twoSided, "OpacityUseValue", 1);
	HRL_MaterialSetFloat(twoSided, "OpacityValue", 1.f);
	HRL_MaterialSetEmissiveColor(twoSided, 1.f, 0.5f, 0.f, 2.f);

	const MeshData sphere = MakeSphere(1.f, 24, 32), ground = MakeGround(30.f);
	AddMesh(scene, ground, groundMat, 0.f, 0.f, 0.f);
	const HRL_id center = AddMesh(scene, sphere, redMat, 0.f, 1.f, 0.f);
	HRL_SetMeshLODAutomatic(center, HRL_TRUE);
	std::vector<HRL_id> extra;
	for (int i = 0; i < 12; ++i)
		extra.push_back(AddMesh(scene, sphere, i % 2 ? twoSided : redMat, -6.f + (float)i, 1.f, -4.f));

	// ---- Shadow maps -----------------------------------------------------------
	Frames(3);
	HRL_EMeshType type = HRL_SPRITE;
	// The camera looks at the centre sphere: it covers the middle of the image.
	CHECK(HRL_GetHoveredObject(scene, 160, 100, &type) == center);
	CHECK(type == HRL_3D_MESH);
	CHECK(HRL_GetHoveredObject(scene, 160, 195, &type) == HRL_INVALID_ID); // top of the image: sky
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, "hrl_test_shadow_maps.png");
	HRL_EndFrame();

	// ---- Ray traced shadows ----------------------------------------------------
	HRL_Vulkan_SetRayTracedShadows(HRL_TRUE);
	CHECK((HRL_Vulkan_GetRayTracedShadows() != 0) == rt);
	Frames(2);
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, "hrl_test_ray_traced.png");
	HRL_EndFrame();
	HRL_Vulkan_SetRayTracedShadows(HRL_FALSE);

	// ---- Resource churn: deletions while frames are in flight ------------------
	for (int round = 0; round < 4; ++round)
	{
		for (size_t i = 0; i < extra.size(); i += 2) HRL_DeleteMesh(extra[i]);
		HRL_BeginFrame(); HRL_EndFrame();
		for (size_t i = 0; i < extra.size(); i += 2)
			extra[i] = AddMesh(scene, sphere, redMat, -6.f + (float)i, 1.f + (float)round, -4.f);
		const HRL_id temp = MakeCheckerTexture(32, 10, 200, 250);
		HRL_MaterialSetTexture(groundMat, HRL_T_ALBEDO, temp);
		HRL_SetTextureMinFilter(temp, HRL_FILTER_NEAREST);
		HRL_BeginFrame(); HRL_EndFrame();
		HRL_MaterialSetTexture(groundMat, HRL_T_ALBEDO, checker);
		HRL_DeleteTexture(temp);
		CHECK(!HRL_IsValidTexture(temp));
		HRL_BeginFrame(); HRL_EndFrame();
	}

	// ---- Two viewports, fog, debug primitives, resize --------------------------
	const HRL_id camera2 = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera2, 40.f);
	HRL_SetCameraNearPlane(camera2, 0.1f);
	HRL_SetCameraFarPlane(camera2, 200.f);
	HRL_SetCameraLocation(camera2, 0.f, 12.f, 0.1f);
	HRL_SetCameraRotation(camera2, -89.f, -90.f, 0.f);
	const HRL_id inset = HRL_CreateViewport(scene, camera2, 0.65f, 0.65f, 0.33f, 0.33f);
	HRL_SetFogEnabled(scene, HRL_TRUE);
	HRL_SetFogMode(scene, HRL_FOG_LINEAR);
	HRL_SetFogColor(scene, 0.6f, 0.7f, 0.8f);
	HRL_SetFogLinearRange(scene, 10.f, 60.f);
	HRL_WindowResizeCallback(400, 260);
	for (int i = 0; i < 3; ++i)
	{
		HRL_BeginFrame();
		HRL_DrawDebugSegment(scene, -3.f, 0.1f, 2.f, 3.f, 3.f, 2.f, 1.f, 1.f, 0.f);
		if (i == 2) HRL_TakeScreenshot(scene, "hrl_test_viewports.png");
		HRL_EndFrame();
	}
	HRL_DeleteViewport(inset);
	HRL_SetFogEnabled(scene, HRL_FALSE);

	// ---- Off-screen scene with its own size, then scene deletion ---------------
	const HRL_id offscreen = HRL_CreateScene(HRL_FALSE);
	HRL_ResizeSceneTexture(offscreen, 128, 96);
	const HRL_id cam3 = HRL_CreateCamera(offscreen, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(cam3, 60.f);
	HRL_SetCameraNearPlane(cam3, 0.1f);
	HRL_SetCameraFarPlane(cam3, 50.f);
	HRL_SetCameraLocation(cam3, 0.f, 0.f, 4.f);
	HRL_SetCameraRotation(cam3, 0.f, -90.f, 0.f);
	HRL_CreateViewport(offscreen, cam3, 0.f, 0.f, 1.f, 1.f);
	const HRL_id lonely = AddMesh(offscreen, sphere, redMat, 0.f, 0.f, 0.f);
	Frames(3);
	CHECK(HRL_GetHoveredObject(offscreen, 64, 48, &type) == lonely);
	HRL_DeleteScene(offscreen);
	Frames(3);

	// Features of the deprecated OpenGL backend must fail cleanly, not crash.
	CHECK(HRL_IsGlobalIlluminationMethodSupported(HRL_GI_DDGI) == HRL_FALSE);

	(void)viewport;
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
