// Programmable render pipeline on Vulkan: compute and fullscreen render passes,
// render targets (2D, 3D, history), the scene image (copy and storage), and
// the participating media volume applied by the surfaces and the sky.
// Screenshot pixels are checked (no tone mapping, no anti-aliasing).
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

HRL_id Compute(const char* src) { return HRL_CreateComputeShader(src, std::strlen(src)); }
HRL_id Fullscreen(const char* src) { return HRL_CreateFullscreenShader(src, std::strlen(src)); }

// Writes a constant colour into a 2D target, one invocation per texel.
const char* kFill = R"(
layout(local_size_x = 8, local_size_y = 8) in;
HRL_IMAGE2D(rgba16f, Out);
uniform vec4 FillColor = vec4(1.0, 0.0, 1.0, 1.0);
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, imageSize(Out)))) return;
	imageStore(Out, p, FillColor);
}
)";

// Left half: the target; right half: the scene as it was (copy).
const char* kShow = R"(
HRL_TEXTURE2D(Source);
layout(location = 0) out vec4 outColor;
void main()
{
	vec3 c = vUV.x < 0.5 ? texture(Source, vUV).rgb : texture(hrl_SceneColor, HRL_SceneUV(vUV)).rgb;
	outColor = vec4(c, 1.0);
}
)";

// After the anti-aliasing: the top-left 16 x 16 pixels of the viewport become blue (storage write).
const char* kStamp = R"(
layout(local_size_x = 8, local_size_y = 8) in;
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= 16 || p.y >= 16) return;
	imageStore(hrl_SceneImage, HRL_ScenePixel(p), vec4(0.0, 0.0, 1.0, 1.0));
}
)";

// Participating media: constant light (green) and transmittance (one half) in every froxel.
const char* kMedia = R"(
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
HRL_IMAGE3D(rgba16f, Media);
uniform vec4 MediaValue = vec4(0.0, 0.5, 0.0, 0.5);
void main()
{
	ivec3 p = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(p, imageSize(Media)))) return;
	imageStore(Media, p, MediaValue);
}
)";

// History: red is 1 once the previous frame of the target exists.
const char* kHistory = R"(
layout(local_size_x = 8, local_size_y = 8) in;
HRL_IMAGE2D(r16f, Current);
HRL_TEXTURE2D(Previous);
void main()
{
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, imageSize(Current)))) return;
	float previous = texelFetch(Previous, p, 0).r;
	imageStore(Current, p, vec4(hrlPass.frame.w > 0.5 ? previous + 1.0 : 0.0));
}
)";
const char* kShowHistory = R"(
HRL_TEXTURE2D(Counter);
uniform float Scale = 0.1;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(min(texture(Counter, vUV).r * Scale, 1.0), 0.0, 0.0, 1.0); }
)";

// The lights and their shadows seen from a compute pass: x = 0 is under the
// occluder, x = 1 in the open (red: light reaching the point).
const char* kLights = R"(
layout(local_size_x = 2) in;
HRL_IMAGE2D(rgba16f, Out);
uniform vec3 Shadowed = vec3(0.0, 0.5, 0.0);
uniform vec3 Lit = vec3(6.0, 0.5, 0.0);
void main()
{
	uint x = gl_GlobalInvocationID.x;
	vec3 p = x == 0u ? Shadowed : Lit;
	vec3 total = vec3(0.0);
	for (uint i = 0u; i < HRL_LightCount(); ++i)
	{
		vec3 L;
		total += HRL_LightRadiance(i, p, L);
	}
	imageStore(Out, ivec2(x, 0), vec4(total, 1.0));
}
)";
const char* kShowLights = R"(
HRL_TEXTURE2D(LightValues);
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(texelFetch(LightValues, ivec2(vUV.x < 0.5 ? 0 : 1, 0), 0).rgb * 0.25, 1.0); }
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
	HRL_SetCameraLocation(camera, 0.f, 1.f, 8.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f); // towards -Z, the horizon in the middle
	const HRL_id viewport = HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	const HRL_id post = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetInt(post, "toneMappingEnabled", 0);
	HRL_MaterialSetFloat(post, "bloomStrength", 0.f);
	HRL_CreatePostProcess(viewport, post, 0);
	// A uniform grey sky.
	HRL_SetSkySphereEnabled(scene, HRL_TRUE);
	HRL_SetSkySphereColors(scene, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f);
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -60.f, 20.f, 0.f);
	HRL_SetLightIntensity(sun, 2.f);
	HRL_SetLightColor(sun, 1.f, 1.f, 1.f);

	int w = 0, h = 0;
	std::vector<unsigned char> img = Shot(scene, "hrl_test_passes_reference.png", w, h);
	CHECK(!img.empty());
	if (!img.empty())
	{
		const unsigned char* sky = Px(img, w, 80, 10);
		std::printf("sky: %d %d %d\n", sky[0], sky[1], sky[2]);
		CHECK(std::abs(sky[0] - 170) < 12 && std::abs(sky[1] - 170) < 12); // 0.4 linear -> sRGB 170
	}

	// ---- Compute + fullscreen passes, 2D target, scene colour copy ----------------------
	const HRL_id fill = Compute(kFill);
	const HRL_id show = Fullscreen(kShow);
	CHECK(fill != HRL_INVALID_ID && show != HRL_INVALID_ID);
	CHECK(HRL_IsValidShader(fill));
	const HRL_id half = HRL_CreateViewportRenderTarget(scene, HRL_RENDER_TARGET_RGBA16F, 0.5f, 1);
	CHECK(HRL_IsValidRenderTarget(half));
	const HRL_id fillMat = HRL_CreateMaterial(fill);
	HRL_MaterialSetVec4(fillMat, "FillColor", 1.f, 0.f, 0.f, 1.f);
	const HRL_id fillPass = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_OPAQUE, 0, fillMat);
	HRL_SetRenderPassTarget(fillPass, "Out", half, HRL_PASS_WRITE);
	HRL_SetRenderPassDispatchTarget(fillPass, half);
	const HRL_id showMat = HRL_CreateMaterial(show);
	const HRL_id showPass = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_TRANSLUCENT, 0, showMat);
	HRL_SetRenderPassTarget(showPass, "Source", half, HRL_PASS_READ);
	HRL_SetRenderPassOutput(showPass, 0, HRL_SCENE_COLOR_TARGET, HRL_PASS_BLEND_NONE);
	CHECK(HRL_IsValidRenderPass(fillPass) && HRL_IsValidRenderPass(showPass));
	img = Shot(scene, "hrl_test_passes_fill.png", w, h);
	if (!img.empty())
	{
		const unsigned char* left = Px(img, w, 20, 60);
		const unsigned char* right = Px(img, w, 140, 10);
		std::printf("fill: left %d %d %d, right %d %d %d\n", left[0], left[1], left[2], right[0], right[1], right[2]);
		CHECK(left[0] > 245 && left[1] < 10 && left[2] < 10);
		CHECK(std::abs(right[0] - 170) < 12 && std::abs(right[2] - 170) < 12);
	}
	// Parameters follow the material.
	HRL_MaterialSetVec4(fillMat, "FillColor", 0.f, 1.f, 0.f, 1.f);
	img = Shot(scene, "hrl_test_passes_param.png", w, h);
	if (!img.empty())
	{
		const unsigned char* left = Px(img, w, 20, 60);
		CHECK(left[1] > 245 && left[0] < 10);
	}
	HRL_SetRenderPassEnabled(fillPass, HRL_FALSE);
	HRL_SetRenderPassEnabled(showPass, HRL_FALSE);

	// ---- Storage write of the scene image after the anti-aliasing ---------------------
	const HRL_id stamp = Compute(kStamp);
	const HRL_id stampPass = HRL_CreateRenderPass(viewport, HRL_STAGE_BEFORE_TONEMAP, 0, HRL_CreateMaterial(stamp));
	img = Shot(scene, "hrl_test_passes_stamp.png", w, h);
	if (!img.empty())
	{
		const unsigned char* in = Px(img, w, 4, 4);
		const unsigned char* out = Px(img, w, 40, 4);
		std::printf("stamp: in %d %d %d, out %d %d %d\n", in[0], in[1], in[2], out[0], out[1], out[2]);
		CHECK(in[2] > 245 && in[0] < 10);
		CHECK(std::abs(out[0] - 170) < 12);
	}
	HRL_DeleteRenderPass(stampPass);
	CHECK(!HRL_IsValidRenderPass(stampPass));

	// ---- Participating media: sky * 0.5 + green 0.5 -------------------------------------
	const HRL_id media = Compute(kMedia);
	const HRL_id froxels = HRL_CreateViewportRenderTarget(scene, HRL_RENDER_TARGET_RGBA16F, 1.f / 8.f, 32);
	const HRL_id mediaPass = HRL_CreateRenderPass(viewport, HRL_STAGE_BEFORE_OPAQUE, 0, HRL_CreateMaterial(media));
	HRL_SetRenderPassTarget(mediaPass, "Media", froxels, HRL_PASS_WRITE);
	HRL_SetRenderPassDispatchTarget(mediaPass, froxels);
	HRL_SetViewportMediaVolume(viewport, froxels, HRL_INVALID_ID, 50.f, 2.f);
	img = Shot(scene, "hrl_test_passes_media.png", w, h);
	if (!img.empty())
	{
		const unsigned char* sky = Px(img, w, 80, 10);
		std::printf("media: sky %d %d %d\n", sky[0], sky[1], sky[2]);
		// Linear: r = 0.2, g = 0.7 -> sRGB 124, 218.
		CHECK(std::abs(sky[0] - 124) < 14 && std::abs(sky[1] - 218) < 14);
	}
	HRL_SetViewportMediaVolume(viewport, HRL_INVALID_ID, HRL_INVALID_ID, 50.f, 2.f);
	HRL_DeleteRenderPass(mediaPass);

	// ---- History: counts the frames once the previous one is valid ----------------------
	const HRL_id counter = HRL_CreateRenderTarget(scene, HRL_RENDER_TARGET_R16F, 4, 4, 1);
	HRL_SetRenderTargetHistory(counter, HRL_TRUE);
	const HRL_id historyPass = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_OPAQUE, 0, HRL_CreateMaterial(Compute(kHistory)));
	HRL_SetRenderPassTarget(historyPass, "Current", counter, HRL_PASS_WRITE);
	HRL_SetRenderPassTarget(historyPass, "Previous", counter, HRL_PASS_READ_PREVIOUS);
	HRL_SetRenderPassDispatchTarget(historyPass, counter);
	const HRL_id showCounter = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_TRANSLUCENT, 1, HRL_CreateMaterial(Fullscreen(kShowHistory)));
	HRL_SetRenderPassTarget(showCounter, "Counter", counter, HRL_PASS_READ);
	HRL_SetRenderPassOutput(showCounter, 0, HRL_SCENE_COLOR_TARGET, HRL_PASS_BLEND_NONE);
	Frames(20);
	img = Shot(scene, "hrl_test_passes_history.png", w, h);
	if (!img.empty())
	{
		const unsigned char* p = Px(img, w, 80, 60);
		std::printf("history: %d %d %d\n", p[0], p[1], p[2]);
		CHECK(p[0] > 245); // counter >= 10 frames
	}
	HRL_ResetRenderTargetHistory(counter);
	img = Shot(scene, "hrl_test_passes_history_reset.png", w, h);
	if (!img.empty())
	{
		const unsigned char* p = Px(img, w, 80, 60);
		std::printf("history after reset: %d\n", p[0]);
		CHECK(p[0] < 245); // restarted 4 frames ago (Shot): 0.4 -> 170
	}
	HRL_DeleteRenderPass(historyPass);
	HRL_DeleteRenderPass(showCounter);

	// ---- Lights and shadows from a compute pass -------------------------------------------
	// A sun from straight above, a slab 3 m over the shadowed point.
	HRL_SetLightRotation(sun, -90.f, 0.f, 0.f);
	HRL_SetLightIntensity(sun, 2.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	const MeshData slab = MakeGround(1.5f);
	const HRL_id slabMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetBool(slabMat, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
	const HRL_id occluder = AddMesh(scene, slab, slabMat, 0.f, 3.f, 0.f);
	const HRL_id lightTarget = HRL_CreateRenderTarget(scene, HRL_RENDER_TARGET_RGBA16F, 2, 1, 1);
	const HRL_id lightPass = HRL_CreateRenderPass(viewport, HRL_STAGE_BEFORE_OPAQUE, 0, HRL_CreateMaterial(Compute(kLights)));
	HRL_SetRenderPassTarget(lightPass, "Out", lightTarget, HRL_PASS_WRITE);
	HRL_SetRenderPassDispatchSize(lightPass, 2, 1, 1);
	const HRL_id showLights = HRL_CreateRenderPass(viewport, HRL_STAGE_BEFORE_TONEMAP, 0, HRL_CreateMaterial(Fullscreen(kShowLights)));
	HRL_SetRenderPassTarget(showLights, "LightValues", lightTarget, HRL_PASS_READ);
	HRL_SetRenderPassOutput(showLights, 0, HRL_SCENE_COLOR_TARGET, HRL_PASS_BLEND_NONE);
	img = Shot(scene, "hrl_test_passes_lights.png", w, h);
	if (!img.empty())
	{
		const unsigned char* shadowed = Px(img, w, 40, 60);
		const unsigned char* lit = Px(img, w, 120, 60);
		std::printf("lights: shadowed %d, lit %d\n", shadowed[0], lit[0]);
		CHECK(lit[0] > 180);       // 2 * 0.25 = 0.5 linear -> sRGB 188
		CHECK(shadowed[0] < 40);   // under the slab
	}
	HRL_DeleteRenderPass(lightPass);
	HRL_DeleteRenderPass(showLights);
	HRL_DeleteMesh(occluder);

	// ---- Errors -------------------------------------------------------------------------
	g_expectError = true;
	CHECK(Compute("layout(local_size_x = 1) in; void main() { undefinedName = 1; }") == HRL_INVALID_ID);
	CHECK(Compute("HRL_IMAGE2D(badformat, X); layout(local_size_x = 1) in; void main() {}") == HRL_INVALID_ID);
	CHECK(Fullscreen("HRL_IMAGE2D(rgba16f, X); layout(location = 0) out vec4 c; void main() { c = vec4(1); }") == HRL_INVALID_ID);
	// A pass whose resource is not bound is skipped (reported once).
	const HRL_id unbound = HRL_CreateRenderPass(viewport, HRL_STAGE_AFTER_OPAQUE, 0, HRL_CreateMaterial(fill));
	Frames(2);
	HRL_DeleteRenderPass(unbound);
	CHECK(HRL_CreateRenderPass(9999999, HRL_STAGE_AFTER_OPAQUE, 0, fillMat) == HRL_INVALID_ID);
	g_expectError = false;
	std::printf("expected errors: %d\n", g_expected);
	CHECK(g_expected == 5);

	// Deleting the scene releases its targets; shutdown releases the rest.
	HRL_DeleteRenderTarget(half);
	CHECK(!HRL_IsValidRenderTarget(half));
	Frames(2);
	HRL_DeleteScene(scene);
	Frames(1);
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
