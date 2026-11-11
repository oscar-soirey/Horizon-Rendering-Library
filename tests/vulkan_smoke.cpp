// Off-screen integration test of the native Vulkan backend.
//
// Exercises resource creation / deletion, textures, automatic LODs, several
// viewports, shadows (maps and ray traced when available), fog, debug
// primitives, window resize, picking and screenshots. Any HRL error (which
// includes Vulkan validation messages when HRL_VULKAN_VALIDATION=1) fails the test.
#include "test_common.h"

#include <hrl_vulkan.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace
{
std::vector<unsigned char> Screenshot(HRL_id scene, const char* path, int& w, int& h)
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

// Mean absolute difference of two images, per channel.
double Difference(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b)
{
	if (a.size() != b.size() || a.empty()) return 1e9;
	double sum = 0.0;
	for (size_t i = 0; i < a.size(); ++i) sum += std::abs((int)a[i] - (int)b[i]);
	return sum / (double)a.size();
}
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnError);
	std::remove("hrl_test_pipeline_cache.bin");
	HRL_Vulkan_SetPipelineCachePath("hrl_test_pipeline_cache.bin");
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

	// Alpha tested fence (its holes must not cast a shadow) and a translucent sphere.
	std::vector<unsigned char> holes(32 * 32 * 4, 255);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x)
			if (((x / 8) + (y / 8)) % 2) holes[(size_t)(y * 32 + x) * 4] = holes[(size_t)(y * 32 + x) * 4 + 1] = holes[(size_t)(y * 32 + x) * 4 + 2] = 0;
	const std::vector<unsigned char> holesPng = EncodePNG(32, 32, holes);
	const HRL_id holesTexture = HRL_CreateTexture(reinterpret_cast<const char*>(holesPng.data()), holesPng.size());
	const HRL_id fenceMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetTexture(fenceMat, HRL_T_ALPHA, holesTexture);
	HRL_MaterialSetBool(fenceMat, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
	const MeshData fence = MakeGround(1.5f);
	const HRL_id fenceMesh = AddMesh(scene, fence, fenceMat, 3.f, 2.f, 1.f);
	HRL_SetMeshRotation(fenceMesh, 90.f, 0.f, 0.f);
	const HRL_id glassMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetBool(glassMat, HRL_MATERIAL_PARAM_TRANSLUCENT, HRL_TRUE);
	HRL_MaterialSetVec3(glassMat, "TintColor", 0.2f, 0.5f, 1.f);
	HRL_MaterialSetInt(glassMat, "OpacityUseValue", 1);
	HRL_MaterialSetFloat(glassMat, "OpacityValue", 0.4f);
	AddMesh(scene, sphere, glassMat, -2.5f, 1.f, 2.f);
	// Post-process material: exposure and bloom.
	const HRL_id postMat = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);
	HRL_MaterialSetFloat(postMat, "bloomStrength", 1.f);
	const HRL_id post = HRL_CreatePostProcess(viewport, postMat, 0);
	CHECK(post != HRL_INVALID_ID);

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

	// ---- Scene data cache: only what changed is recomputed ----------------------
	HRL_VulkanSceneStats stats{};
	Frames(1);
	CHECK(HRL_Vulkan_GetSceneStats(scene, &stats) == HRL_TRUE);
	CHECK(stats.instance_count == 16 && stats.material_count == 5);
	CHECK(stats.drawn_instance_count > 0 && stats.render_pass_count >= 3);
	CHECK(stats.scene_rebuilds == 0 && stats.transform_updates == 0 && stats.material_updates == 0);
	HRL_SetMeshLocation(center, 40.f, 1.f, 0.f);
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.scene_rebuilds == 0 && stats.transform_updates == 1 && stats.material_updates == 0);
	CHECK(HRL_GetHoveredObject(scene, 160, 100, &type) != center); // the moved mesh left the image centre
	HRL_SetMeshLocation(center, 0.f, 1.f, 0.f);
	HRL_MaterialSetVec3(redMat, "TintColor", 0.1f, 0.9f, 0.1f);
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.scene_rebuilds == 0 && stats.transform_updates == 1 && stats.material_updates == 1);
	HRL_MaterialSetVec3(redMat, "TintColor", 0.1f, 0.9f, 0.1f); // same value: nothing to do
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.transform_updates == 0 && stats.material_updates == 0);
	HRL_MaterialSetVec3(redMat, "TintColor", 0.9f, 0.1f, 0.1f);
	const HRL_id temporary = AddMesh(scene, sphere, redMat, 3.f, 1.f, 3.f);
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.scene_rebuilds == 1 && stats.instance_count == 17);
	HRL_DeleteMesh(temporary);
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.scene_rebuilds == 1 && stats.instance_count == 16);
	CHECK(HRL_GetHoveredObject(scene, 160, 100, &type) == center);

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

	// ---- Skeletal mesh: skinned on the GPU by a compute pass ----------------------
	{
		const HRL_id skScene = HRL_CreateScene(HRL_FALSE);
		HRL_ResizeSceneTexture(skScene, 128, 96);
		const HRL_id skCamera = HRL_CreateCamera(skScene, HRL_PERSPECTIVE);
		HRL_SetCameraPerspectiveFov(skCamera, 60.f);
		HRL_SetCameraNearPlane(skCamera, 0.1f);
		HRL_SetCameraFarPlane(skCamera, 100.f);
		HRL_SetCameraLocation(skCamera, 0.f, 0.f, 4.f);
		HRL_SetCameraRotation(skCamera, 0.f, -90.f, 0.f);
		HRL_CreateViewport(skScene, skCamera, 0.f, 0.f, 1.f, 1.f);
		const HRL_id skLight = HRL_CreateLight(skScene, HRL_DIRECTIONAL_LIGHT);
		HRL_SetLightRotation(skLight, -50.f, 30.f, 0.f);
		HRL_SetLightCastShadows(skLight, HRL_TRUE);
		HRL_SetLightShadowResolution(skLight, 512);

		// A sphere bound to bone 1. Animation: frame 0 in place, frame 1 moved 30 units away.
		std::vector<HRL_SkeletalVertex> vertices(sphere.v.size());
		for (size_t i = 0; i < vertices.size(); ++i)
		{
			vertices[i] = HRL_SkeletalVertex{};
			vertices[i].vertex = sphere.v[i];
			vertices[i].boneIndices[0] = 1;
			vertices[i].boneWeights[0] = 1.f;
		}
		std::vector<HRL_uint> indices = sphere.i;
		const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		HRL_SkeletalBoneTransform rest{};
		rest.rotation[3] = 1.f;
		rest.scale[0] = rest.scale[1] = rest.scale[2] = 1.f;
		HRL_SkeletalBone bones[2]{};
		const char* boneNames[2] = {"root", "body"};
		for (int b = 0; b < 2; ++b)
		{
			bones[b].name = boneNames[b];
			bones[b].parentIndex = 0;
			bones[b].bindTransform = rest;
			for (int e = 0; e < 16; ++e) bones[b].bindWorldMatrix[e] = bones[b].inverseBindMatrix[e] = identity[e];
		}
		HRL_SkeletalBoneTransform frames[4] = {rest, rest, rest, rest}; // 2 frames x 2 bones
		float poses[4 * 16];
		for (int m = 0; m < 4; ++m)
			for (int e = 0; e < 16; ++e) poses[m * 16 + e] = identity[e];
		poses[3 * 16 + 12] = 30.f; // frame 1, bone 1 : translation x
		HRL_SkeletalAnimation animation{};
		animation.name = "move";
		animation.duration = 1.f;
		animation.frameRate = 1.f;
		animation.frameCount = 2;
		animation.frames = frames;
		animation.poseMatrices = poses;
		HRL_SkeletalMeshData data{};
		data.vertices = vertices.data();
		data.vertexCount = vertices.size();
		data.indices = indices.data();
		data.indexCount = indices.size();
		data.bones = bones;
		data.boneCount = 2;
		data.animations = &animation;
		data.animationCount = 1;
		for (int e = 0; e < 16; ++e) data.geometryToWorldMatrix[e] = identity[e];

		const HRL_id skeletal = HRL_CreateSkeletalMesh(skScene, &data);
		CHECK(skeletal != HRL_INVALID_ID);
		HRL_SetMeshMaterial(skeletal, redMat);
		HRL_PlaySkeletalAnimation(skeletal, 0);
		HRL_SetSkeletalAnimationLoop(skeletal, HRL_FALSE);
		HRL_SetSkeletalAnimationTime(skeletal, 0.f);
		Frames(3);
		CHECK(HRL_GetHoveredObject(skScene, 64, 48, &type) == skeletal);
		HRL_VulkanSceneStats skStats{};
		HRL_Vulkan_GetSceneStats(skScene, &skStats);
		CHECK(skStats.instance_count == 1 && skStats.skinned_mesh_count == 0); // pose unchanged: not skinned again
		HRL_SetSkeletalAnimationTime(skeletal, 1.f);
		Frames(1);
		HRL_Vulkan_GetSceneStats(skScene, &skStats);
		CHECK(skStats.skinned_mesh_count == 1);
		HRL_BeginFrame();
		HRL_TakeScreenshot(skScene, "hrl_test_skeletal_moved.png");
		HRL_EndFrame();
		Frames(2);
		CHECK(HRL_GetHoveredObject(skScene, 64, 48, &type) == HRL_INVALID_ID); // the bone carried the mesh away
		HRL_SetSkeletalAnimationTime(skeletal, 0.f);
		Frames(3);
		CHECK(HRL_GetHoveredObject(skScene, 64, 48, &type) == skeletal);
		HRL_DeleteMesh(skeletal);
		Frames(2);
		HRL_DeleteScene(skScene);
		Frames(2);
	}

	// ---- Point light shadows (shadow maps, or ray traced when available) -----------
	{
		const HRL_id pScene = HRL_CreateScene(HRL_FALSE);
		HRL_ResizeSceneTexture(pScene, 160, 160);
		const HRL_id pCam = HRL_CreateCamera(pScene, HRL_PERSPECTIVE);
		HRL_SetCameraPerspectiveFov(pCam, 60.f);
		HRL_SetCameraLocation(pCam, 0.f, 10.f, 0.01f);
		HRL_SetCameraRotation(pCam, -89.9f, -90.f, 0.f); // looking down
		HRL_CreateViewport(pScene, pCam, 0.f, 0.f, 1.f, 1.f);
		const HRL_id pLight = HRL_CreateLight(pScene, HRL_POINT_LIGHT);
		HRL_SetLightLocation(pLight, 0.f, 4.f, 0.f);
		HRL_SetLightIntensity(pLight, 20.f);
		HRL_SetLightCastShadows(pLight, HRL_TRUE);
		HRL_SetLightShadowResolution(pLight, 512);
		const HRL_id white = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
		AddMesh(pScene, ground, white, 0.f, 0.f, 0.f);
		// A sphere between the light and the ground at +X: its shadow lands further out.
		AddMesh(pScene, sphere, white, 1.5f, 2.f, 0.f);
		for (int rayTraced = 0; rayTraced < (rt ? 2 : 1); ++rayTraced)
		{
			HRL_Vulkan_SetRayTracedShadows(rayTraced);
			Frames(3);
			int w = 0, h = 0;
			const std::vector<unsigned char> img = Screenshot(pScene, rayTraced ? "hrl_test_point_shadow_rt.png" : "hrl_test_point_shadow.png", w, h);
			CHECK(w == 160 && h == 160);
			if (w != 160 || h != 160) continue;
			// Ground point at x = 3 (behind the sphere seen from the light) and at x = -3.
			// 10 m high camera, 60 degrees: 1 m = 160 / (2 * 10 * tan 30) = 13.9 px.
			auto luma = [&](int x, int y) { const unsigned char* p = &img[((size_t)y * w + x) * 4]; return p[0] + p[1] + p[2]; };
			const int shadowed = luma(80 + 42, 80), lit = luma(80 - 42, 80);
			std::printf("point light shadow (%s): lit %d, shadowed %d\n", rayTraced ? "ray traced" : "maps", lit, shadowed);
			CHECK(lit > 60 && shadowed < lit / 2);
		}
		HRL_Vulkan_SetRayTracedShadows(HRL_FALSE);
		HRL_DeleteScene(pScene);
		Frames(2);
	}

	// ---- Anti-aliasing ------------------------------------------------------------
	{
		HRL_SetMeshLocation(center, 0.f, 1.f, 0.f);
		CHECK(HRL_Vulkan_GetAntiAliasing() == HRL_VULKAN_AA_FXAA); // default
		int w = 0, h = 0;
		HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_NONE);
		Frames(2);
		const std::vector<unsigned char> none = Screenshot(scene, "hrl_test_aa_none.png", w, h);
		HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_FXAA);
		Frames(2);
		const std::vector<unsigned char> fxaa = Screenshot(scene, "hrl_test_aa_fxaa.png", w, h);
		HRL_SetAntialiasingMode(4); // MSAA request: temporal anti-aliasing on Vulkan
		CHECK(HRL_Vulkan_GetAntiAliasing() == HRL_VULKAN_AA_TAA);
		Frames(16);
		const std::vector<unsigned char> taa = Screenshot(scene, "hrl_test_aa_taa.png", w, h);
		// Anti-aliasing only changes the edges: the images stay close to each other.
		const double dFxaa = Difference(none, fxaa), dTaa = Difference(none, taa);
		std::printf("anti-aliasing: FXAA %.2f, TAA %.2f (mean difference with no AA)\n", dFxaa, dTaa);
		CHECK(dFxaa > 0.01 && dFxaa < 6.0);
		CHECK(dTaa > 0.01 && dTaa < 8.0);
		HRL_Vulkan_SetVSync(HRL_FALSE); // off-screen: no swapchain, only kept
		CHECK(HRL_Vulkan_GetVSync() == HRL_FALSE);
		HRL_Vulkan_SetVSync(HRL_TRUE);
		HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_FXAA);
	}

	// Global illumination: the probe volume (DDGI) only.
	CHECK(HRL_IsGlobalIlluminationMethodSupported(HRL_GI_DDGI) == HRL_TRUE);
	CHECK(HRL_IsGlobalIlluminationMethodSupported(HRL_GI_SSGI) == HRL_FALSE);

	uint64_t usedMemory = 0, memoryBudget = 0;
	CHECK(HRL_Vulkan_GetMemoryUsage(&usedMemory, &memoryBudget) == HRL_TRUE);
	CHECK(usedMemory > 0 && memoryBudget >= usedMemory);

	HRL_Shutdown();

	// The pipeline cache was written by the shutdown and is accepted by a second
	// initialisation (same GPU, same driver).
	FILE* cache = std::fopen("hrl_test_pipeline_cache.bin", "rb");
	CHECK(cache != nullptr);
	if (cache)
	{
		std::fseek(cache, 0, SEEK_END);
		CHECK(std::ftell(cache) >= 32);
		std::fclose(cache);
	}
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(64, 64, nullptr);
	const HRL_id again = HRL_CreateScene(HRL_FALSE);
	Frames(2);
	CHECK(HRL_IsValidScene(again));
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
