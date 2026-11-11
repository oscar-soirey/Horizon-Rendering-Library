// Custom shaders on Vulkan: surface functions (meshes), vertex functions,
// OpenGL post-process shaders and compilation errors. Pixels are checked.
#include "test_common.h"

#include <hrl_vulkan.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <cstring>

namespace
{
bool g_expectError = false;
int g_expected = 0;
void OnErrorExpected(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	if (g_expectError && severity == HRL_SEVERITY_ERROR) { ++g_expected; return; }
	OnError(code, severity, detail);
}

std::vector<unsigned char> Shot(HRL_id scene, const char* path, int& w, int& h)
{
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, path);
	HRL_EndFrame();
	Frames(3);
	int c = 0;
	unsigned char* data = stbi_load(path, &w, &h, &c, 4);
	std::vector<unsigned char> out;
	if (data) out.assign(data, data + (size_t)w * h * 4);
	stbi_image_free(data);
	return out;
}

const unsigned char* Px(const std::vector<unsigned char>& img, int w, int x, int y) { return &img[((size_t)y * w + x) * 4]; }

HRL_id Shader(const char* vert, const char* frag)
{
	return HRL_CreateShader(vert, vert ? std::strlen(vert) : 0, frag, std::strlen(frag));
}

// Unlit green, emissive: the colour comes only from the surface function.
const char* kSurface = R"(
uniform vec3 Glow = vec3(0.0, 1.0, 0.0);
uniform float Strength;
uniform sampler2D Pattern;
void Surface(HRL_SurfaceInput i, inout HRL_Surface s)
{
	s.baseColor = vec3(0.0);
	s.emissive = Glow * Strength * texture(Pattern, i.uv).r;
}
)";

// Moves the mesh 3 m up.
const char* kVertex = R"(
uniform float Lift = 3.0;
void Vertex(inout HRL_VertexData v) { v.worldPosition.y += Lift; }
)";

// OpenGL post-process: top half (v > 0.5) red, bottom half the scene.
const char* kPost = R"(#version 330 core
in vec2 uv;
out vec4 FragColor;
uniform sampler2D uScene;
uniform vec2 uScreenSize;
uniform float uTime;
uniform vec3 topColor = vec3(1.0, 0.0, 0.0);
uniform bool enabled = true;
void main()
{
	vec3 c = texture(uScene, uv).rgb;
	if (enabled && uv.y > 0.5) c = topColor;
	FragColor = vec4(c, 1.0);
}
)";
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnErrorExpected);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(160, 120, nullptr);
	if (g_errors) { std::fprintf(stderr, "Vulkan initialisation failed\n"); return 1; }
	HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_NONE);

	const HRL_id scene = HRL_CreateScene(HRL_FALSE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraLocation(camera, 0.f, 0.f, 8.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f); // towards -Z
	const HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -60.f, 20.f, 0.f);
	// No tone mapping: the emissive green comes out as is.
	const HRL_id defaultPost = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetInt(defaultPost, "toneMappingEnabled", 0);
	HRL_MaterialSetFloat(defaultPost, "bloomStrength", 0.f);
	HRL_CreatePostProcess(viewport, defaultPost, 0);

	// ---- Surface function --------------------------------------------------------
	const HRL_id surface = Shader(nullptr, kSurface);
	CHECK(surface != HRL_INVALID_ID);
	CHECK(HRL_IsValidShader(surface));
	const HRL_id custom = HRL_CreateMaterial(surface);
	HRL_MaterialSetFloat(custom, "Strength", 1.f);
	std::vector<unsigned char> white(4 * 4 * 4, 255);
	HRL_MaterialSetTexture(custom, "Pattern", HRL_CreateTextureFromPixels(white.data(), 4, 4));
	const MeshData sphere = MakeSphere(1.f, 16, 24);
	const HRL_id mesh = AddMesh(scene, sphere, custom, 0.f, 0.f, 0.f);
	Frames(2);
	int w = 0, h = 0;
	std::vector<unsigned char> img = Shot(scene, "hrl_test_custom_surface.png", w, h);
	if (!img.empty())
	{
		const unsigned char* p = Px(img, w, 80, 60);
		std::printf("surface: %d %d %d\n", p[0], p[1], p[2]);
		CHECK(p[1] > 200 && p[0] < 40 && p[2] < 40);
	}
	HRL_EMeshType type;
	CHECK(HRL_GetHoveredObject(scene, 80, 60, &type) == mesh);

	// Parameters change with the material.
	HRL_MaterialSetVec3(custom, "Glow", 0.f, 0.f, 1.f);
	Frames(1);
	img = Shot(scene, "hrl_test_custom_param.png", w, h);
	if (!img.empty())
	{
		const unsigned char* p = Px(img, w, 80, 60);
		CHECK(p[2] > 200 && p[1] < 40);
	}

	// ---- Vertex function: the sphere moves up 3 m (picking follows) --------------------
	const HRL_id lifted = Shader(kVertex, kSurface);
	CHECK(lifted != HRL_INVALID_ID);
	const HRL_id liftedMat = HRL_CreateMaterial(lifted);
	HRL_MaterialSetFloat(liftedMat, "Strength", 1.f);
	HRL_SetMeshMaterial(mesh, liftedMat);
	Frames(2);
	// 8 m away, 60 degrees: 3 m = 3 / (2 * 8 * tan 30) * 120 = 39 px above the centre.
	CHECK(HRL_GetHoveredObject(scene, 80, 60, &type) == HRL_INVALID_ID);
	CHECK(HRL_GetHoveredObject(scene, 80, 60 + 39, &type) == mesh);

	// Shadows of custom materials: a ground below, the sphere casts a shadow.
	const HRL_id groundMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	AddMesh(scene, MakeGround(20.f), groundMat, 0.f, -1.5f, 0.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	HRL_SetMeshMaterial(mesh, custom);
	Frames(2);
	img = Shot(scene, "hrl_test_custom_shadow.png", w, h);
	CHECK(g_errors == 0);

	// ---- OpenGL post-process shader ---------------------------------------------------
	const HRL_id post = Shader(nullptr, kPost);
	CHECK(post != HRL_INVALID_ID);
	const HRL_id postMat = HRL_CreateMaterial(post);
	HRL_CreatePostProcess(viewport, postMat, 1);
	Frames(1);
	img = Shot(scene, "hrl_test_custom_post.png", w, h);
	if (!img.empty())
	{
		const unsigned char* top = Px(img, w, 80, 10);
		const unsigned char* centre = Px(img, w, 80, 90);
		std::printf("post: top %d %d %d, bottom %d %d %d\n", top[0], top[1], top[2], centre[0], centre[1], centre[2]);
		CHECK(top[0] > 240 && top[1] < 10 && top[2] < 10); // v > 0.5 is the top of the image
		CHECK(!(centre[0] > 240 && centre[1] < 10));        // the scene below
	}
	HRL_MaterialSetBool(postMat, "enabled", HRL_FALSE);
	Frames(1);
	img = Shot(scene, "hrl_test_custom_post_off.png", w, h);
	if (!img.empty())
	{
		const unsigned char* top = Px(img, w, 80, 10);
		CHECK(!(top[0] > 240 && top[1] < 10));
	}

	// Colour controls of the built-in post-process material (after the custom ones).
	HRL_MaterialSetBool(postMat, "enabled", HRL_TRUE);
	HRL_MaterialSetBool(defaultPost, "invertColor", HRL_TRUE);
	Frames(1);
	img = Shot(scene, "hrl_test_custom_invert.png", w, h);
	if (!img.empty())
	{
		const unsigned char* top = Px(img, w, 80, 10);
		CHECK(top[0] < 10 && top[1] > 240 && top[2] > 240); // red inverted: cyan
	}
	HRL_MaterialSetBool(defaultPost, "invertColor", HRL_FALSE);

	// ---- Errors are reported at creation --------------------------------------------
	g_expectError = true;
	CHECK(Shader(nullptr, "void Surface(HRL_SurfaceInput i, inout HRL_Surface s) { s.baseColor = undefinedName; }") == HRL_INVALID_ID);
	CHECK(Shader(nullptr, "#version 330 core\nout vec4 c; void main() { c = vec4(1) }") == HRL_INVALID_ID);
	g_expectError = false;
	CHECK(g_expected == 2);

	HRL_DeleteShader(lifted);
	Frames(2);
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
