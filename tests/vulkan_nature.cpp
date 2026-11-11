// Off-screen test of the Vulkan landscapes, foliage and grass, and of the GPU
// culling (frustum, occlusion, LOD) of meshes and foliage.
//
//   * landscape heights: creation, queries, partial update, picking;
//   * foliage: parts, scattering, painting with a minimal spacing, erasing,
//     instance read back, rendering with LODs, impostors and shadows;
//   * grass on the landscape (with its shadows);
//   * occlusion culling is conservative: the object visible at each pixel is
//     the same with and without it, and it removes hidden instances;
//   * deletions (foliage, landscape with its grass, scene).
// Any HRL error, including Vulkan validation messages, fails the test.
#include "test_common.h"

#include <hrl_vulkan.h>

#include <algorithm>
#include <cstring>

namespace
{
constexpr int kWidth = 320, kHeight = 200;

// Object id at a grid of pixels (picking) after a few frames.
std::vector<HRL_id> PickGrid(HRL_id scene)
{
	Frames(3);
	std::vector<HRL_id> ids;
	for (int y = 5; y < kHeight; y += 15)
		for (int x = 5; x < kWidth; x += 15)
		{
			HRL_EMeshType type = HRL_3D_MESH;
			ids.push_back(HRL_GetHoveredObject(scene, x, y, &type));
		}
	return ids;
}

MeshData MakeBox(float hx, float hy, float hz)
{
	MeshData m = MakeGround(1.f); // top face, then the other faces
	m.v.clear();
	m.i.clear();
	const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const float t[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
	for (int f = 0; f < 6; ++f)
	{
		const float bx = n[f][1] * t[f][2] - n[f][2] * t[f][1];
		const float by = n[f][2] * t[f][0] - n[f][0] * t[f][2];
		const float bz = n[f][0] * t[f][1] - n[f][1] * t[f][0];
		const HRL_uint base = (HRL_uint)m.v.size();
		for (int c = 0; c < 4; ++c)
		{
			const float su = (c == 1 || c == 2) ? 1.f : -1.f, sv = (c >= 2) ? 1.f : -1.f;
			HRL_Vertex3D v{};
			v.position[0] = (n[f][0] + t[f][0] * su + bx * sv) * hx;
			v.position[1] = (n[f][1] + t[f][1] * su + by * sv) * hy;
			v.position[2] = (n[f][2] + t[f][2] * su + bz * sv) * hz;
			for (int k = 0; k < 3; ++k) { v.normal[k] = n[f][k]; v.tangent[k] = t[f][k]; }
			v.bitangent[0] = bx; v.bitangent[1] = by; v.bitangent[2] = bz;
			v.uv[0] = su * 0.5f + 0.5f;
			v.uv[1] = sv * 0.5f + 0.5f;
			m.v.push_back(v);
		}
		m.i.insert(m.i.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
	}
	return m;
}

HRL_id MakeMaterial(float r, float g, float b)
{
	const HRL_id m = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(m, "TintColor", r, g, b);
	HRL_MaterialSetInt(m, "RoughnessUseValue", 1);
	HRL_MaterialSetFloat(m, "RoughnessValue", 0.8f);
	return m;
}
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(kWidth, kHeight, nullptr);
	if (g_errors) { std::fprintf(stderr, "Vulkan initialisation failed\n"); return 1; }
	const bool rt = HRL_Vulkan_IsRayTracingSupported() != 0;
	std::printf("GPU: %s, ray tracing: %s\n", HRL_Vulkan_GetDeviceName(), rt ? "yes" : "no");

	const HRL_id scene = HRL_CreateScene(HRL_TRUE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraNearPlane(camera, 0.1f);
	HRL_SetCameraFarPlane(camera, 500.f);
	HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	HRL_SetSkySphereEnabled(scene, HRL_TRUE);
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -45.f, 30.f, 0.f);
	HRL_SetLightIntensity(sun, 3.f);
	HRL_SetLightCastShadows(sun, HRL_TRUE);
	HRL_SetLightShadowResolution(sun, 1024);

	// ---- Landscape: a tilted plane y = 0.1 x over 129 x 129 samples, 128 x 128 units ----
	const HRL_uint W = 129;
	std::vector<float> heights(W * W);
	for (HRL_uint z = 0; z < W; ++z)
		for (HRL_uint x = 0; x < W; ++x) heights[z * W + x] = 0.1f * ((float)x - 64.f);
	const HRL_id land = HRL_CreateLandscapeFromHeights(scene, heights.data(), W, W);
	CHECK(land != HRL_INVALID_ID);
	HRL_SetLandscapeSize(land, 128.f, 128.f);
	HRL_SetLandscapeMaterial(land, MakeMaterial(0.3f, 0.4f, 0.2f));
	float y = -1.f, nx = 0.f, ny = 0.f, nz = 0.f;
	CHECK(HRL_GetLandscapeHeightAt(land, 10.f, 3.f, &y) && std::abs(y - 1.f) < 1e-3f);
	CHECK(HRL_GetLandscapeNormalAt(land, 10.f, 3.f, &nx, &ny, &nz) && std::abs(nx + 0.0995f) < 2e-3f && std::abs(ny - 0.995f) < 2e-3f);
	CHECK(!HRL_GetLandscapeHeightAt(land, 100.f, 0.f, &y)); // outside
	HRL_uint hw = 0, hd = 0;
	HRL_GetLandscapeHeightsSize(land, &hw, &hd);
	CHECK(hw == W && hd == W);
	// Sculpt a 3 x 3 area around the centre (sample 64, 64 = world 0, 0).
	const float bump[9] = {5, 5, 5, 5, 5, 5, 5, 5, 5};
	HRL_UpdateLandscapeHeights(land, 63, 63, 3, 3, bump);
	CHECK(HRL_GetLandscapeHeightAt(land, 0.f, 0.f, &y) && std::abs(y - 5.f) < 1e-3f);
	HRL_UpdateLandscapeHeights(land, 128, 128, 3, 3, bump); // outside the samples
	CHECK(g_errors == 1);
	g_errors = 0;
	const float flat[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
	HRL_UpdateLandscapeHeights(land, 63, 63, 3, 3, flat);

	// ---- Foliage: two parts (trunk, crown), scattering, painting, erasing ----------
	const MeshData trunk = MakeBox(0.2f, 1.5f, 0.2f);
	MeshData crown = MakeSphere(1.2f, 10, 14);
	for (auto& v : crown.v) v.position[1] += 3.5f;
	const HRL_id tree = HRL_CreateFoliage(scene);
	CHECK(HRL_AddFoliageMesh(tree, trunk.v.data(), trunk.v.size(), trunk.i.data(), trunk.i.size(), MakeMaterial(0.35f, 0.2f, 0.1f)) == 0);
	CHECK(HRL_AddFoliageMesh(tree, crown.v.data(), crown.v.size(), crown.i.data(), crown.i.size(), MakeMaterial(0.1f, 0.4f, 0.1f)) == 1);
	HRL_FoliageScatter scatter = HRL_DefaultFoliageScatter();
	scatter.density = 0.01f;
	scatter.min_spacing = 4.f;
	const size_t scattered = HRL_ScatterFoliage(tree, land, &scatter);
	std::printf("scattered %zu trees\n", scattered);
	CHECK(scattered > 50 && scattered < 200 && HRL_GetFoliageInstanceCount(tree) == scattered);
	// Every instance stands on the ground and respects the spacing.
	std::vector<HRL_FoliageInstance> all(scattered);
	CHECK(HRL_GetFoliageInstances(tree, all.data(), all.size()) == scattered);
	bool onGround = true, spaced = true;
	for (size_t i = 0; i < all.size(); ++i)
	{
		float ground = 0.f;
		HRL_GetLandscapeHeightAt(land, all[i].position[0], all[i].position[2], &ground);
		onGround = onGround && std::abs(ground - all[i].position[1]) < 1e-2f;
		for (size_t j = i + 1; j < all.size(); ++j)
		{
			const float dx = all[i].position[0] - all[j].position[0], dz = all[i].position[2] - all[j].position[2];
			spaced = spaced && dx * dx + dz * dz >= 16.f - 1e-3f;
		}
	}
	CHECK(onGround && spaced);
	// Erase a circle, then paint it again.
	const size_t erased = HRL_EraseFoliage(tree, 0.f, 0.f, 20.f);
	CHECK(erased > 0 && HRL_GetFoliageInstanceCount(tree) == scattered - erased);
	scatter.seed = 3;
	const size_t painted = HRL_PaintFoliage(tree, land, 0.f, 0.f, 20.f, &scatter);
	CHECK(painted > 0 && HRL_GetFoliageInstanceCount(tree) == scattered - erased + painted);
	// Painting again at the same place keeps the spacing with the instances already there.
	HRL_PaintFoliage(tree, land, 0.f, 0.f, 20.f, &scatter);
	{
		std::vector<HRL_FoliageInstance> near(HRL_GetFoliageInstanceCount(tree));
		HRL_GetFoliageInstances(tree, near.data(), near.size());
		bool kept = true;
		for (size_t i = 0; i < near.size(); ++i)
			for (size_t j = i + 1; j < near.size(); ++j)
			{
				const float dx = near[i].position[0] - near[j].position[0], dz = near[i].position[2] - near[j].position[2];
				kept = kept && dx * dx + dz * dz >= 16.f - 1e-3f;
			}
		CHECK(kept);
	}
	// An instance given explicitly comes back with its rotation.
	const HRL_FoliageInstance one{{30.f, 3.f, 30.f}, {10.f, 30.f, 5.f}, 1.5f};
	HRL_ClearFoliage(tree);
	CHECK(HRL_GetFoliageInstanceCount(tree) == 0);
	CHECK(HRL_AddFoliageInstances(tree, &one, 1) == 1);
	HRL_FoliageInstance back{};
	CHECK(HRL_GetFoliageInstances(tree, &back, 1) == 1);
	CHECK(std::abs(back.rotation[0] - 10.f) < 0.01f && std::abs(back.rotation[1] - 30.f) < 0.01f &&
		std::abs(back.rotation[2] - 5.f) < 0.01f && back.scale == 1.5f);
	HRL_ClearFoliage(tree);
	HRL_ScatterFoliage(tree, land, &scatter);

	// ---- Rendering: LODs, shadows, impostors, grass, wind and interactors -------------
	HRL_SetCameraLocation(camera, -50.f, 15.f, -50.f);
	HRL_SetCameraRotation(camera, -12.f, 45.f, 0.f);
	HRL_SetFoliageLODDistance(tree, 25.f);
	HRL_SetFoliageImpostorDistance(tree, 70.f);
	HRL_SetWind(scene, 1.f, 0.5f, 0.7f, 0.5f);
	HRL_SetFoliageInteractor(scene, 0, -40.f, 0.f, -40.f, 3.f);
	const HRL_id grass = HRL_CreateGrass(land);
	CHECK(grass != HRL_INVALID_ID);
	HRL_SetGrassDensity(grass, 20.f);
	HRL_SetGrassDistance(grass, 40.f);
	HRL_SetGrassCastShadows(grass, HRL_TRUE);
	Frames(4);
	HRL_VulkanFoliageStats fs{};
	CHECK(HRL_Vulkan_GetFoliageStats(tree, &fs));
	std::printf("foliage drawn: LOD %u / %u / %u, impostors %u\n", fs.lod_instances[0], fs.lod_instances[1], fs.lod_instances[2], fs.impostor_instances);
	CHECK(fs.lod_instances[0] + fs.lod_instances[1] + fs.lod_instances[2] > 0 && fs.impostor_instances > 0);
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, "hrl_test_nature.png");
	HRL_EndFrame();
	if (rt)
	{
		HRL_Vulkan_SetRayTracedShadows(HRL_TRUE);
		Frames(2);
		HRL_BeginFrame();
		HRL_TakeScreenshot(scene, "hrl_test_nature_rt.png");
		HRL_EndFrame();
		HRL_Vulkan_SetRayTracedShadows(HRL_FALSE);
	}

	// ---- Picking: a tree right in front of the camera, the ground below ---------------
	HRL_ClearFoliage(tree);
	const HRL_FoliageInstance front{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 1.f};
	HRL_AddFoliageInstances(tree, &front, 1);
	HRL_SetCameraLocation(camera, 0.f, 3.5f, 8.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f);
	Frames(3);
	HRL_EMeshType type = HRL_3D_MESH;
	CHECK(HRL_GetHoveredObject(scene, kWidth / 2, kHeight / 2, &type) == tree && type == HRL_FOLIAGE);
	CHECK(HRL_GetHoveredObject(scene, kWidth / 2, 5, &type) == land && type == HRL_LANDSCAPE);

	// ---- Occlusion culling: same image, fewer instances --------------------------------
	// A wall in front of the camera hides a field of trees and spheres.
	HRL_ClearFoliage(tree);
	HRL_SetFoliageImpostorDistance(tree, 0.f);
	HRL_SetFoliageDrawDistance(tree, 400.f);
	for (int z = 0; z < 10; ++z)
		for (int x = 0; x < 10; ++x)
		{
			const HRL_FoliageInstance f{{-20.f + 4.f * (float)x, 0.f, -10.f - 4.f * (float)z}, {0.f, 0.f, 0.f}, 1.f};
			HRL_AddFoliageInstances(tree, &f, 1);
		}
	const MeshData sphere = MakeSphere(0.5f, 8, 12);
	const HRL_id sphereMat = MakeMaterial(0.8f, 0.2f, 0.2f);
	for (int i = 0; i < 40; ++i) AddMesh(scene, sphere, sphereMat, -15.f + (float)(i % 10) * 3.f, 1.f, -12.f - 3.f * (float)(i / 10));
	const MeshData wall = MakeBox(30.f, 15.f, 0.5f);
	AddMesh(scene, wall, MakeMaterial(0.6f, 0.6f, 0.6f), 0.f, 0.f, 0.f);
	HRL_DeleteGrass(grass);
	HRL_SetWind(scene, 1.f, 0.f, 0.f, 0.f); // still: the images are compared
	HRL_SetCameraLocation(camera, 0.f, 3.f, 12.f);
	HRL_SetCameraRotation(camera, 0.f, -90.f, 0.f);
	HRL_Vulkan_SetOcclusionCulling(HRL_TRUE);
	const std::vector<HRL_id> withOcclusion = PickGrid(scene);
	Frames(3);
	HRL_Vulkan_GetFoliageStats(tree, &fs);
	const uint32_t hiddenDrawn = fs.lod_instances[0] + fs.lod_instances[1] + fs.lod_instances[2];
	HRL_Vulkan_SetOcclusionCulling(HRL_FALSE);
	const std::vector<HRL_id> withoutOcclusion = PickGrid(scene);
	Frames(3);
	HRL_Vulkan_GetFoliageStats(tree, &fs);
	const uint32_t allDrawn = fs.lod_instances[0] + fs.lod_instances[1] + fs.lod_instances[2];
	std::printf("trees behind the wall drawn: %u with occlusion culling, %u without\n", hiddenDrawn, allDrawn);
	CHECK(withOcclusion == withoutOcclusion);
	CHECK(allDrawn > 0 && hiddenDrawn < allDrawn / 4);
	// Looking over the wall: the trees appear in the frame where they become visible.
	HRL_Vulkan_SetOcclusionCulling(HRL_TRUE);
	HRL_SetCameraLocation(camera, 0.f, 40.f, 12.f);
	HRL_SetCameraRotation(camera, -40.f, -90.f, 0.f);
	HRL_BeginFrame();
	HRL_EndFrame();
	HRL_BeginFrame();
	HRL_EndFrame();
	const std::vector<HRL_id> over = PickGrid(scene);
	HRL_Vulkan_SetOcclusionCulling(HRL_FALSE);
	const std::vector<HRL_id> overReference = PickGrid(scene);
	CHECK(over == overReference);
	CHECK(std::count(over.begin(), over.end(), tree) > 0);
	HRL_Vulkan_SetOcclusionCulling(HRL_TRUE);

	// ---- Deletions ------------------------------------------------------------------------
	const HRL_id grass2 = HRL_CreateGrass(land);
	Frames(2);
	HRL_DeleteLandscape(land); // its grass goes with it
	CHECK(!HRL_IsValidLandscape(land));
	HRL_SetGrassDensity(grass2, 10.f);
	CHECK(g_errors == 1); // the grass no longer exists
	g_errors = 0;
	HRL_DeleteFoliage(tree);
	CHECK(!HRL_IsValidFoliage(tree));
	Frames(3);
	const HRL_id other = HRL_CreateScene(HRL_FALSE);
	const HRL_id f2 = HRL_CreateFoliage(other);
	HRL_AddFoliageMesh(f2, crown.v.data(), crown.v.size(), crown.i.data(), crown.i.size(), sphereMat);
	HRL_AddFoliageInstances(f2, &one, 1);
	Frames(2);
	HRL_DeleteScene(other);
	CHECK(!HRL_IsValidFoliage(f2));
	Frames(3);

	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
