// Screen space effects on Vulkan (the OpenGL effect shaders compiled at run
// time): decals, screen space reflections, ambient occlusion, local and global
// volumetric fog, god rays and volumetric clouds. Each effect must compile,
// pass validation and change the image the expected way.
#include "test_common.h"

#include <hrl_vulkan.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace
{
struct Image
{
	int w = 0, h = 0;
	std::vector<unsigned char> px;
	const unsigned char* At(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
	double Mean(int x0, int y0, int x1, int y1, int channel = -1) const
	{
		double sum = 0.0;
		int n = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x, ++n)
			{
				const unsigned char* p = At(x, y);
				sum += channel < 0 ? (p[0] + p[1] + p[2]) / 3.0 : p[channel];
			}
		return n ? sum / n : 0.0;
	}
};

Image Shot(HRL_id scene, const char* path)
{
	Frames(2);
	HRL_BeginFrame();
	HRL_TakeScreenshot(scene, path);
	HRL_EndFrame();
	Frames(3);
	Image img;
	int c = 0;
	unsigned char* data = stbi_load(path, &img.w, &img.h, &c, 4);
	if (data) img.px.assign(data, data + (size_t)img.w * img.h * 4);
	stbi_image_free(data);
	CHECK(!img.px.empty());
	return img;
}
} // namespace

int main()
{
	HRL_RegisterErrorCallback(OnError);
	HRL_Init(HRL_VULKAN);
	HRL_InitContext(200, 150, nullptr);
	if (g_errors) { std::fprintf(stderr, "Vulkan initialisation failed\n"); return 1; }
	HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_NONE);

	const HRL_id scene = HRL_CreateScene(HRL_FALSE);
	const HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(camera, 60.f);
	HRL_SetCameraFarPlane(camera, 500.f);
	HRL_SetCameraLocation(camera, 0.f, 3.f, 8.f);
	HRL_SetCameraRotation(camera, -20.f, -90.f, 0.f);
	HRL_CreateViewport(scene, camera, 0.f, 0.f, 1.f, 1.f);
	HRL_SetSkySphereEnabled(scene, HRL_TRUE);
	const HRL_id sun = HRL_CreateLight(scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(sun, -50.f, 30.f, 0.f);
	const HRL_id sky = HRL_CreateLight(scene, HRL_SKY_LIGHT);
	HRL_SetLightIntensity(sky, 0.6f);
	const HRL_id white = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	AddMesh(scene, MakeGround(30.f), white, 0.f, 0.f, 0.f);
	const MeshData sphere = MakeSphere(1.f, 24, 32);
	for (int i = -2; i <= 2; ++i) AddMesh(scene, sphere, white, (float)i * 2.2f, 1.f, 0.f);

	const Image base = Shot(scene, "hrl_test_fx_base.png");
	// Rows of the image: ground in the lower part, contact of the spheres around y = 95.
	const int gx0 = 20, gx1 = 180, gy0 = 110, gy1 = 145;

	// ---- Ambient occlusion: darker where the spheres meet the ground ------------------
	HRL_SetAmbientOcclusionEnabled(scene, HRL_TRUE);
	HRL_SetAmbientOcclusionStrength(scene, 1.f);
	const Image ao = Shot(scene, "hrl_test_fx_ao.png");
	// Same shader as OpenGL: it darkens the creases between the spheres and the ground.
	std::printf("ambient occlusion: %.3f -> %.3f\n", base.Mean(0, 30, 200, 90), ao.Mean(0, 30, 200, 90));
	CHECK(ao.Mean(0, 30, 200, 90) < base.Mean(0, 30, 200, 90) - 0.01);
	HRL_SetAmbientOcclusionEnabled(scene, HRL_FALSE);

	// ---- Decal: a red square on the ground in front of the spheres -------------------
	std::vector<unsigned char> red(16 * 16 * 4, 255);
	for (size_t i = 0; i < red.size(); i += 4) red[i + 1] = red[i + 2] = 0;
	const HRL_id decal = HRL_CreateDecal(scene);
	HRL_SetDecalTexture(decal, HRL_CreateTextureFromPixels(red.data(), 16, 16));
	HRL_SetDecalPosition(decal, 0.f, 0.f, 3.f);
	HRL_SetDecalSize(decal, 3.f, 1.f, 2.f);
	const Image decalShot = Shot(scene, "hrl_test_fx_decal.png");
	{
		// Red dominates in the middle of the ground below the spheres.
		const double r = decalShot.Mean(85, 95, 115, 110, 0), g = decalShot.Mean(85, 95, 115, 110, 1);
		std::printf("decal: red %.1f green %.1f (before: %.1f %.1f)\n", r, g, base.Mean(85, 95, 115, 110, 0), base.Mean(85, 95, 115, 110, 1));
		CHECK(r > g + 40.0);
	}
	HRL_SetDecalEnabled(decal, HRL_FALSE);

	// ---- Screen space reflections: the spheres appear in a shiny ground ---------------
	HRL_SetScreenSpaceReflectionsEnabled(scene, HRL_TRUE);
	HRL_SetScreenSpaceReflectionsStrength(scene, 1.f);
	const Image ssr = Shot(scene, "hrl_test_fx_ssr.png");
	double diff = 0.0;
	for (size_t i = 0; i < ssr.px.size() && i < base.px.size(); ++i) diff += std::abs((int)ssr.px[i] - (int)base.px[i]);
	std::printf("reflections: mean change %.2f\n", diff / (double)std::max<size_t>(base.px.size(), 1));
	CHECK(diff / (double)std::max<size_t>(base.px.size(), 1) > 0.005);
	HRL_SetScreenSpaceReflectionsEnabled(scene, HRL_FALSE);

	// ---- Volumetric fog (local, then global): towards the fog colour -----------------
	const HRL_id fog = HRL_CreateVolumetricFog(scene);
	HRL_SetVolumetricFogEnabled(fog, HRL_TRUE);
	HRL_SetVolumetricFogPosition(fog, 0.f, 1.f, 0.f);
	HRL_SetVolumetricFogRadius(fog, 6.f);
	HRL_SetVolumetricFogDensity(fog, 2.f);
	HRL_SetVolumetricFogColor(fog, 0.f, 1.f, 0.f);
	const Image fogShot = Shot(scene, "hrl_test_fx_fog.png");
	std::printf("volumetric fog: green %.1f -> %.1f\n", base.Mean(60, 60, 140, 110, 1) - base.Mean(60, 60, 140, 110, 0),
		fogShot.Mean(60, 60, 140, 110, 1) - fogShot.Mean(60, 60, 140, 110, 0));
	CHECK(fogShot.Mean(60, 60, 140, 110, 1) - fogShot.Mean(60, 60, 140, 110, 0) >
		base.Mean(60, 60, 140, 110, 1) - base.Mean(60, 60, 140, 110, 0) + 10.0);
	HRL_SetVolumetricFogEnabled(fog, HRL_FALSE);
	HRL_SetGlobalVolumetricFogEnabled(scene, HRL_TRUE);
	HRL_SetGlobalVolumetricFogDensity(scene, 0.3f);
	HRL_SetGlobalVolumetricFogColor(scene, 1.f, 0.f, 1.f);
	const Image global = Shot(scene, "hrl_test_fx_global_fog.png");
	CHECK(global.Mean(gx0, gy0, gx1, gy1, 1) < global.Mean(gx0, gy0, gx1, gy1, 0)); // magenta tint
	HRL_SetGlobalVolumetricFogEnabled(scene, HRL_FALSE);

	// ---- God rays: brighter around the light ------------------------------------------
	HRL_SetGodRaysEnabled(scene, HRL_TRUE);
	HRL_SetGodRaysPosition(scene, 0.f, 6.f, -20.f);
	HRL_SetGodRaysColor(scene, 1.f, 1.f, 0.6f);
	HRL_SetGodRaysWeight(scene, 0.6f);
	const Image rays = Shot(scene, "hrl_test_fx_god_rays.png");
	std::printf("god rays: %.1f -> %.1f\n", base.Mean(0, 0, 200, 150), rays.Mean(0, 0, 200, 150));
	CHECK(rays.Mean(0, 0, 200, 150) > base.Mean(0, 0, 200, 150) + 1.0);
	HRL_SetGodRaysEnabled(scene, HRL_FALSE);

	// ---- Particles: red billboards (alpha), then mesh particles (additive) ------------
	{
		const HRL_id system = HRL_CreateVFXSystem(scene);
		HRL_SetVFXSystemAutoUpdate(system, HRL_FALSE);
		HRL_SetVFXSystemPosition(system, 0.f, 3.f, 3.f);
		const HRL_id emitter = HRL_CreateVFXEmitter(system);
		HRL_SetVFXEmitterSpawnRate(emitter, 0.f);
		HRL_AddVFXBurst(emitter, 0.f, 40);
		HRL_SetVFXEmitterLifetime(emitter, 100.f, 100.f);
		HRL_SetVFXEmitterSpawnShape(emitter, HRL_VFX_SHAPE_SPHERE);
		HRL_SetVFXEmitterShapeRadius(emitter, 0.3f);
		HRL_SetVFXEmitterParticleSize(emitter, 0.6f, 0.6f);
		const HRL_id colors = HRL_CreateVFXColorCurve(emitter);
		HRL_AddVFXColorKey(colors, 0.f, 1.f, 0.f, 0.f, 1.f);
		HRL_AddVFXColorKey(colors, 1.f, 1.f, 0.f, 0.f, 1.f);
		HRL_SetVFXEmitterColorCurve(emitter, colors);
		HRL_PlayVFXSystem(system);
		HRL_UpdateVFX(0.05f);
		const Image particles = Shot(scene, "hrl_test_fx_particles.png");
		const double r = particles.Mean(85, 30, 115, 60, 0), g = particles.Mean(85, 30, 115, 60, 1);
		std::printf("particles: red %.1f green %.1f\n", r, g);
		CHECK(r > g + 60.0);

		HRL_SetVFXEmitterRenderMode(emitter, HRL_VFX_RENDER_MESH);
		const HRL_id templateMesh = AddMesh(scene, sphere, white, 0.f, -50.f, 0.f); // out of sight
		HRL_SetVFXEmitterMesh(emitter, templateMesh);
		HRL_SetVFXEmitterMeshScale(emitter, 0.3f, 0.3f, 0.3f);
		HRL_SetVFXEmitterBlendMode(emitter, HRL_VFX_BLEND_ADDITIVE);
		const Image meshes = Shot(scene, "hrl_test_fx_mesh_particles.png");
		diff = 0.0;
		for (size_t i = 0; i < meshes.px.size() && i < base.px.size(); ++i) diff += std::abs((int)meshes.px[i] - (int)base.px[i]);
		std::printf("mesh particles: mean change %.2f\n", diff / (double)std::max<size_t>(base.px.size(), 1));
		CHECK(diff / (double)std::max<size_t>(base.px.size(), 1) > 0.2);
		HRL_DeleteVFXSystem(system);
	}

	// ---- Screen space displacement: a quad shows the scene behind it, shifted --------
	{
		std::vector<unsigned char> shift(8 * 8 * 4, 0);
		for (size_t i = 0; i < shift.size(); i += 4) { shift[i] = 255; shift[i + 1] = 128; shift[i + 3] = 255; } // +X
		const HRL_id displacementMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
		HRL_MaterialSetTexture(displacementMat, HRL_MATERIAL_TEXTURE_SS_DISPLACEMENT_MAPPING, HRL_CreateTextureFromPixels(shift.data(), 8, 8));
		HRL_MaterialSetBool(displacementMat, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_ENABLED, HRL_TRUE);
		HRL_MaterialSetFloat(displacementMat, HRL_MATERIAL_PARAM_SS_DISPLACEMENT_STRENGTH, 20.f);
		const HRL_id pane = AddMesh(scene, MakeGround(1.5f), displacementMat, 0.f, 1.f, 3.f);
		HRL_SetMeshRotation(pane, 90.f, 0.f, 0.f); // facing the camera
		const Image displaced = Shot(scene, "hrl_test_fx_displacement.png");
		diff = 0.0;
		for (size_t i = 0; i < displaced.px.size() && i < base.px.size(); ++i) diff += std::abs((int)displaced.px[i] - (int)base.px[i]);
		std::printf("displacement: mean change %.2f\n", diff / (double)std::max<size_t>(base.px.size(), 1));
		CHECK(diff / (double)std::max<size_t>(base.px.size(), 1) > 0.3);
		HRL_EMeshType type;
		CHECK(HRL_GetHoveredObject(scene, 100, 75, &type) == pane); // pickable
		HRL_DeleteMesh(pane);
	}

	// ---- Global illumination (probe volume): light bounced by a red wall -------------
	{
		const HRL_id redMat = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
		HRL_MaterialSetVec3(redMat, "TintColor", 1.f, 0.f, 0.f);
		const HRL_id wall = AddMesh(scene, MakeGround(3.f), redMat, 2.f, 3.f, 1.f);
		HRL_SetMeshRotation(wall, 0.f, 0.f, 90.f); // vertical, facing -X
		const Image before = Shot(scene, "hrl_test_fx_gi_off.png");
		HRL_SetGlobalIlluminationMethod(scene, HRL_GI_DDGI);
		HRL_SetGlobalIlluminationEnabled(scene, HRL_TRUE);
		HRL_SetGlobalIlluminationIntensity(scene, 2.f);
		Frames(80); // the probes converge over a few dozen frames
		const Image after = Shot(scene, "hrl_test_fx_gi_on.png");
		// The ground and the spheres receive the light bounced by the scene and the sky.
		const double redGain = after.Mean(0, 40, 200, 150, 0) - before.Mean(0, 40, 200, 150, 0);
		const double greenGain = after.Mean(0, 40, 200, 150, 1) - before.Mean(0, 40, 200, 150, 1);
		std::printf("global illumination: red +%.2f, green +%.2f\n", redGain, greenGain);
		CHECK(redGain > 3.0 && greenGain > 3.0);
		HRL_SetGlobalIlluminationEnabled(scene, HRL_FALSE);
		HRL_DeleteMesh(wall);
	}

	// ---- Volumetric clouds: the sky changes (camera looking up) -----------------------
	HRL_SetCameraRotation(camera, 30.f, -90.f, 0.f);
	const Image skyBase = Shot(scene, "hrl_test_fx_sky.png");
	HRL_SetVolumetricCloudEnabled(scene, HRL_TRUE);
	HRL_SetVolumetricCloudCoverage(scene, 1.f);
	HRL_SetVolumetricCloudWind(scene, 0.f, 0.f, 0.f); // the clouds stay where they are
	const Image clouds = Shot(scene, "hrl_test_fx_clouds.png");
	diff = 0.0;
	for (size_t i = 0; i < clouds.px.size() && i < skyBase.px.size(); ++i) diff += std::abs((int)clouds.px[i] - (int)skyBase.px[i]);
	std::printf("clouds: mean change %.2f\n", diff / (double)std::max<size_t>(skyBase.px.size(), 1));
	CHECK(diff / (double)std::max<size_t>(skyBase.px.size(), 1) > 1.0);

	HRL_Shutdown();
	std::printf("%d check failure(s), %d HRL error(s)\n", g_failures, g_errors);
	return (g_failures || g_errors) ? 1 : 0;
}
