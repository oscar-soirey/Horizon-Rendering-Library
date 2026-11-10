// Measures the CPU time HRL spends per frame on a large static scene.
//
// The meshes are placed behind the camera: they are culled, so the GPU has
// almost nothing to draw and the frame time is the cost of preparing the scene
// (instances, materials, LOD selection, culling) on the CPU.
//   hrl_bench_scene [mesh count] [material count] [frames]
#include "test_common.h"

#include <hrl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

int main(int argc, char** argv)
{
	const int meshCount = argc > 1 ? std::atoi(argv[1]) : 20000;
	const int materialCount = argc > 2 ? std::atoi(argv[2]) : 500;
	const int frames = argc > 3 ? std::atoi(argv[3]) : 200;

	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(64, 64, nullptr);
	if (g_errors) return 1;

	const HRL_id scene = HRL_CreateScene(HRL_TRUE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraNearPlane(camera, 0.1f);
	HRL_SetCameraFarPlane(camera, 100.f);
	HRL_SetCameraLocation(camera, 0.f, 0.f, 0.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f); // looks towards -Z
	HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -50.f, 30.f, 0.f);

	const HRL_id texture = MakeCheckerTexture(16, 200, 200, 200);
	std::vector<HRL_id> materials;
	for (int i = 0; i < materialCount; ++i)
	{
		const HRL_id m = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
		HRL_MaterialSetTexture(m, HRL_T_ALBEDO, texture);
		HRL_MaterialSetVec3(m, "TintColor", 0.5f, 0.5f, 0.5f);
		HRL_MaterialSetInt(m, "RoughnessUseValue", 1);
		HRL_MaterialSetFloat(m, "RoughnessValue", 0.5f);
		HRL_MaterialSetInt(m, "MetallicUseValue", 1);
		HRL_MaterialSetFloat(m, "MetallicValue", 0.f);
		materials.push_back(m);
	}
	const MeshData quad = MakeGround(0.5f);
	std::vector<HRL_id> meshes;
	for (int i = 0; i < meshCount; ++i)
	{
		// Behind the camera (+Z), on a grid.
		const HRL_id mesh = AddMesh(scene, quad, materials[(size_t)i % materials.size()],
			(float)(i % 200) * 2.f - 200.f, (float)((i / 200) % 200) * 2.f - 100.f, 50.f + (float)(i / 40000) * 2.f);
		meshes.push_back(mesh);
	}
	Frames(5); // uploads, warm-up

	auto measure = [&](const char* label, int movingMeshes, int changingMaterials)
	{
		std::vector<double> times;
		for (int f = 0; f < frames; ++f)
		{
			for (int i = 0; i < movingMeshes; ++i)
				HRL_SetMeshLocation(meshes[(size_t)i], (float)i, (float)f * 0.01f, 60.f);
			for (int i = 0; i < changingMaterials; ++i)
				HRL_MaterialSetFloat(materials[(size_t)i], "RoughnessValue", 0.3f + 0.001f * (float)(f % 100));
			HRL_BeginFrame();
			const auto t0 = std::chrono::steady_clock::now();
			HRL_EndFrame();
			times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		}
		std::sort(times.begin(), times.end());
		std::printf("%-34s median %7.3f ms   p90 %7.3f ms\n", label, times[times.size() / 2], times[times.size() * 9 / 10]);
	};
	std::printf("%d meshes, %d materials, %d frames, GPU: %s\n", meshCount, materialCount, frames, HRL_Vulkan_GetDeviceName());
	measure("static scene", 0, 0);
	measure("100 meshes moving", std::min(100, meshCount), 0);
	measure("10 materials changing", 0, std::min(10, materialCount));

	HRL_Shutdown();
	return g_errors ? 1 : 0;
}
