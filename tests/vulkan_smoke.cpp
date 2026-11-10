// Off-screen integration test of the native Vulkan backend.
//
// Exercises resource creation / deletion, textures, automatic LODs, several
// viewports, shadows (maps and ray traced when available), fog, debug
// primitives, window resize, picking and screenshots. Any HRL error (which
// includes Vulkan validation messages when HRL_VULKAN_VALIDATION=1) fails the test.
#include "test_common.h"

#include <hrl_vulkan.h>

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

	// ---- Scene data cache: only what changed is recomputed ----------------------
	HRL_VulkanSceneStats stats{};
	Frames(1);
	CHECK(HRL_Vulkan_GetSceneStats(scene, &stats) == HRL_TRUE);
	CHECK(stats.instance_count == 14 && stats.material_count == 3);
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
	CHECK(stats.scene_rebuilds == 1 && stats.instance_count == 15);
	HRL_DeleteMesh(temporary);
	Frames(1);
	HRL_Vulkan_GetSceneStats(scene, &stats);
	CHECK(stats.scene_rebuilds == 1 && stats.instance_count == 14);
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

	// Features of the deprecated OpenGL backend must fail cleanly, not crash.
	CHECK(HRL_IsGlobalIlluminationMethodSupported(HRL_GI_DDGI) == HRL_FALSE);

	(void)viewport;
	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
