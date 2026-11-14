// HRL example "Heaven": a dark, heavy, smoke filled forest rendered with the
// programmable render pipeline of the Vulkan backend (hrl::heaven::HeavenStyle,
// built on hrl::CustomShaderRenderer, src/extras).
//
//   hrl_vulkan_heaven                       opens a window (cinematic camera)
//   hrl_vulkan_heaven --screenshot out.png  renders off-screen and writes a PNG
//   options: --size W H   --frames N (frames rendered before the screenshot / before quitting)
//            --time T (seconds into the camera path)   --plain (HRL without the Heaven style)
//            --rt (ray traced shadows)   --set name=value (a Heaven setting or a light, see Overrides())
//
// Controls: C toggles the cinematic camera; free camera: right mouse button +
// mouse to look, W A S D to move, Q / E down / up, Shift faster. H toggles the
// Heaven style, P takes a screenshot, Escape quits.
#include <hrl.h>
#include <hrl_vulkan.h>
#include <extras/heaven_style.hpp>

#ifdef HRL_EXAMPLE_HAS_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include "procedural.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{
using geo::V3;
int g_errors = 0;

void OnError(HRL_EError code, HRL_ESeverity severity, const char* detail)
{
	std::fprintf(stderr, "[HRL %s] %s: %s\n", HRL_SeverityEnumToString(severity), HRL_ErrorEnumToString(code), detail ? detail : "");
	if (severity == HRL_SEVERITY_ERROR || severity == HRL_SEVERITY_FATAL) ++g_errors;
}

struct Options
{
	int width = 1600, height = 900;
	std::string screenshot;
	int frames = 0;
	float time = 18.f;
	bool plain = false;
	bool rayTracedShadows = false;
	std::vector<std::pair<std::string, float>> overrides; // --set name=value
	std::string debug;                                    // --debug noise | media | smoke
	bool fixedCamera = false;                             // --camera x y z pitch yaw
	float camera[5] = {0.f, 1.6f, 0.f, 0.f, -90.f};
};

// Lights of the scene that --set can change (tuning without recompiling).
struct LightSettings
{
	float sunIntensity = 1.7f, sunPitch = -11.f;
	float fireIntensity = 60.f, fireAttenuation = 0.03f, fireCore = 40.f;
	float ambient = 0.05f;
};

bool ApplyOverride(const std::string& name, float v, hrl::heaven::Settings& s, LightSettings& l)
{
	struct Entry { const char* name; float* value; };
	const Entry entries[] = {
		{"mediaDistance", &s.mediaDistance}, {"fogDensity", &s.fogDensity}, {"fogHeightFalloff", &s.fogHeightFalloff},
		{"fogBaseHeight", &s.fogBaseHeight}, {"fogNoiseFrequency", &s.fogNoiseFrequency}, {"fogNoiseAmount", &s.fogNoiseAmount},
		{"fogAnisotropy", &s.fogAnisotropy}, {"fogTemporalBlend", &s.fogTemporalBlend}, {"smokeSteps", &s.smokeSteps},
		{"smokeShadowSteps", &s.smokeShadowSteps}, {"smokeMultipleScattering", &s.smokeMultipleScattering},
		{"smokeTemporalBlend", &s.smokeTemporalBlend}, {"hazeDistortion", &s.hazeDistortion}, {"edgeBlur", &s.edgeBlur},
		{"aberration", &s.aberration}, {"halationStrength", &s.halationStrength}, {"crush", &s.crush},
		{"desaturate", &s.desaturate}, {"exposure", &s.exposure}, {"bloomStrength", &s.bloomStrength},
		{"contrast", &s.contrast}, {"saturation", &s.saturation}, {"vignetteStrength", &s.vignetteStrength},
		{"vignetteRadius", &s.vignetteRadius}, {"vignetteSoftness", &s.vignetteSoftness}, {"filmGrain", &s.filmGrain},
		{"sunIntensity", &l.sunIntensity}, {"sunPitch", &l.sunPitch}, {"fireIntensity", &l.fireIntensity},
		{"fireAttenuation", &l.fireAttenuation}, {"fireCore", &l.fireCore}, {"ambient", &l.ambient}};
	for (const Entry& e : entries)
		if (name == e.name) { *e.value = v; return true; }
	return false;
}

// Where the light comes from: a fire behind the trees.
const V3 kGlow(0.f, 4.5f, -44.f);

float PathX(float z) { return 0.9f * std::sin(z * 0.09f); }

float Ground(float x, float z)
{
	const float hills = (geo::Noise3(x * 0.045f, 0.f, z * 0.045f) - 0.5f) * 2.2f + (geo::Noise3(x * 0.13f, 3.f, z * 0.13f) - 0.5f) * 0.5f;
	const float side = std::fabs(x - PathX(z));
	// A clearing along the path, rising banks on the sides and far away.
	const float banks = std::max(0.f, side - 9.f) * 0.12f + std::max(0.f, -z - 60.f) * 0.18f;
	return hills * std::min(1.f, 0.25f + side * 0.12f) + banks;
}

HRL_id AddMesh(HRL_id scene, const geo::Mesh& m, HRL_id material, const V3& at, float yaw = 0.f)
{
	const HRL_id id = HRL_CreateMesh3D(scene, m.vertices.data(), m.vertices.size(), m.indices.data(), m.indices.size());
	HRL_SetMeshMaterial(id, material);
	HRL_SetMeshLocation(id, at.x, at.y, at.z);
	if (yaw != 0.f) HRL_SetMeshRotation(id, 0.f, yaw, 0.f);
	return id;
}

struct Bird
{
	HRL_id mesh = 0;
	V3 center;
	float radius = 10.f, speed = 0.3f, phase = 0.f, height = 1.f;
};

struct App
{
	HRL_id scene = 0, camera = 0, viewport = 0, sun = 0, fire = 0;
	std::unique_ptr<hrl::heaven::HeavenStyle> heaven;
	HRL_id plainPost = 0, plainPostPass = 0;
	std::vector<Bird> birds;
	hrl::heaven::Settings settings;
	LightSettings lights;
	bool cinematic = true;
	float camX = 0.f, camY = 1.6f, camZ = 6.f, pitch = 3.f, yaw = -90.f;
};

void EnableHeaven(App& app, bool enable)
{
	if (enable && !app.heaven)
	{
		if (app.plainPostPass) { HRL_DeletePostProcess(app.plainPostPass); app.plainPostPass = 0; }
		hrl::heaven::Settings s = app.settings;
		s.glowPosition = {kGlow.x, kGlow.y, kGlow.z};
		app.heaven = std::make_unique<hrl::heaven::HeavenStyle>(app.scene, app.viewport, s);
		if (!app.heaven->Ok()) std::fprintf(stderr, "Heaven: %s\n", app.heaven->LastError().c_str());
	}
	else if (!enable && app.heaven)
	{
		app.heaven.reset();
		app.plainPostPass = HRL_CreatePostProcess(app.viewport, app.plainPost, 0);
	}
}

void BuildScene(App& app, const Options& opt)
{
	for (const auto& [name, value] : opt.overrides)
		if (!ApplyOverride(name, value, app.settings, app.lights))
			std::fprintf(stderr, "--set: unknown setting %s\n", name.c_str());
	const LightSettings& L = app.lights;
	app.scene = HRL_CreateScene(HRL_TRUE);
	app.camera = HRL_CreateCamera(app.scene, HRL_PERSPECTIVE);
	HRL_SetCameraPerspectiveFov(app.camera, 52.f);
	HRL_SetCameraNearPlane(app.camera, 0.1f);
	HRL_SetCameraFarPlane(app.camera, 400.f);
	app.viewport = HRL_CreateViewport(app.scene, app.camera, 0.f, 0.f, 1.f, 1.f);
	app.plainPost = HRL_CreateMaterial(HRL_DEFAULT_POST_PROCESS_SHADER);

	// A dark sky: the haze in front of it makes the light.
	HRL_SetSkySphereEnabled(app.scene, HRL_TRUE);
	HRL_SetSkySphereColors(app.scene, 0.012f, 0.008f, 0.006f, 0.16f, 0.07f, 0.03f, 0.012f, 0.008f, 0.006f);

	// The sun, low and behind the forest: it comes towards the camera through the trees.
	app.sun = HRL_CreateLight(app.scene, HRL_DIRECTIONAL_LIGHT);
	HRL_SetLightRotation(app.sun, L.sunPitch, 90.f, 0.f);
	HRL_SetLightColor(app.sun, 1.f, 0.48f, 0.2f);
	HRL_SetLightIntensity(app.sun, L.sunIntensity);
	HRL_SetLightCastShadows(app.sun, HRL_TRUE);
	HRL_SetLightShadowResolution(app.sun, 2048);
	HRL_Vulkan_SetShadowDistance(110.f);
	// The fire: a point light with its shadows (the watchers cut the light).
	app.fire = HRL_CreateLight(app.scene, HRL_POINT_LIGHT);
	HRL_SetLightLocation(app.fire, kGlow.x, kGlow.y, kGlow.z);
	HRL_SetLightColor(app.fire, 1.f, 0.6f, 0.3f);
	HRL_SetLightIntensity(app.fire, L.fireIntensity);
	HRL_SetLightAttenuation(app.fire, L.fireAttenuation);
	HRL_SetLightCastShadows(app.fire, HRL_TRUE);
	// A little light from everywhere.
	const HRL_id ambient = HRL_CreateLight(app.scene, HRL_SKY_LIGHT);
	HRL_SetLightColor(ambient, 0.9f, 0.6f, 0.42f);
	HRL_SetLightIntensity(ambient, L.ambient);

	EnableHeaven(app, !opt.plain);
	if (!app.heaven) app.plainPostPass = HRL_CreatePostProcess(app.viewport, app.plainPost, 0);

	// Materials: the Heaven forms when the style exists, plain dark ones otherwise.
	auto form = [&](const char* name, const hrl::heaven::Form& f) -> HRL_id
	{
		if (app.heaven) return app.heaven->CreateFormMaterial(name, f);
		const HRL_id m = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
		HRL_MaterialSetVec3(m, "TintColor", f.color.r, f.color.g, f.color.b);
		HRL_MaterialSetInt(m, "RoughnessUseValue", 1);
		HRL_MaterialSetFloat(m, "RoughnessValue", f.roughness);
		HRL_MaterialSetInt(m, "SpecularUseValue", 1);
		HRL_MaterialSetFloat(m, "SpecularValue", 0.1f);
		if (f.twoSided) HRL_MaterialSetBool(m, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
		return m;
	};
	hrl::heaven::Form groundForm;
	groundForm.color = {0.035f, 0.025f, 0.02f};
	groundForm.rimStrength = 0.08f;
	groundForm.breath = 0.f;
	hrl::heaven::Form treeForm;
	treeForm.color = {0.03f, 0.022f, 0.017f};
	treeForm.rimStrength = 0.35f;
	treeForm.sway = 0.015f;
	treeForm.swayBase = 2.f;
	treeForm.breath = 0.012f;
	hrl::heaven::Form figureForm;
	figureForm.color = {0.02f, 0.014f, 0.011f};
	figureForm.roughness = 0.6f;
	figureForm.rimStrength = 0.32f;
	figureForm.rimPower = 2.4f;
	figureForm.breath = 0.01f;
	figureForm.sway = 0.004f;
	figureForm.dissolve = 1.f;
	figureForm.dissolveHeight = 1.3f;
	hrl::heaven::Form birdForm;
	birdForm.color = {0.012f, 0.01f, 0.01f};
	birdForm.rimStrength = 0.45f;
	birdForm.breath = 0.f;
	birdForm.flap = 0.16f;
	birdForm.twoSided = true;
	hrl::heaven::Form perchedForm = birdForm;
	perchedForm.flap = 0.f;
	hrl::heaven::Form grassForm;
	grassForm.color = {0.03f, 0.024f, 0.016f};
	grassForm.rimStrength = 0.35f;
	grassForm.sway = 0.25f;
	grassForm.breath = 0.f;
	grassForm.twoSided = true;
	const HRL_id groundMat = form("ground", groundForm);
	const HRL_id treeMat = form("tree", treeForm);
	const HRL_id figureMat = form("figure", figureForm);
	const HRL_id birdMat = form("bird", birdForm);
	const HRL_id perchedMat = form("perched", perchedForm);
	const HRL_id grassMat = form("grass", grassForm);

	// ---- Ground ----
	AddMesh(app.scene, geo::Terrain(220.f, 220, Ground), groundMat, V3(0.f, 0.f, -30.f + 30.f));

	// ---- The fire: a bright core, the haze does the rest ----
	const HRL_id core = HRL_CreateMaterial(HRL_MESH_3D_SHADER);
	HRL_MaterialSetVec3(core, "TintColor", 0.f, 0.f, 0.f);
	HRL_MaterialSetEmissiveColor(core, 1.f, 0.55f, 0.25f, L.fireCore);
	AddMesh(app.scene, geo::Ellipsoid(V3(0, 0, 0), V3(1.6f, 1.2f, 1.6f), 16, 24), core, V3(kGlow.x, kGlow.y - 1.5f, kGlow.z));

	// ---- Trees: crooked and dead, around the path ----
	geo::Rng rng(7);
	int trees = 0;
	for (int attempt = 0; attempt < 400 && trees < 52; ++attempt)
	{
		const float z = rng.Range(-72.f, 14.f);
		const float x = PathX(z) + rng.Range(-34.f, 34.f);
		const float side = std::fabs(x - PathX(z));
		if (side < 3.2f || (side < 6.f && rng.Uniform() < 0.6f)) continue;
		if (std::fabs(x - kGlow.x) < 7.f && std::fabs(z - kGlow.z) < 7.f) continue;
		std::vector<std::pair<V3, V3>> tips;
		geo::Mesh tree = geo::Tree(rng, rng.Range(7.f, 15.f), &tips);
		for (const auto& tip : tips)
			if (tip.first.y > 3.5f && rng.Uniform() < 0.28f)
				tree.Append(geo::Vine(tip.first, rng.Range(0.8f, 3.2f), rng));
		AddMesh(app.scene, tree, treeMat, V3(x, Ground(x, z), z), rng.Range(0.f, 360.f));
		++trees;
	}
	// A low branch over the path, with a dream catcher hanging from it.
	{
		const V3 base(3.4f, 0.f, -7.f);
		geo::Mesh trunk;
		geo::Rng local(99);
		geo::Branch(trunk, V3(0.f, -0.3f, 0.f), V3(-0.12f, 1.f, 0.05f), 7.f, 0.32f, 0, 2, local);
		std::vector<V3> limb = {V3(0.f, 3.4f, 0.f), V3(-1.2f, 3.55f, 0.1f), V3(-2.6f, 3.45f, 0.15f), V3(-3.8f, 3.2f, 0.1f)};
		trunk.Append(geo::Tube(limb, {0.16f, 0.12f, 0.09f, 0.06f}, 10));
		AddMesh(app.scene, trunk, treeMat, V3(base.x, Ground(base.x, base.z), base.z));
		hrl::heaven::Form charm = treeForm;
		charm.sway = 0.f;
		charm.rimStrength = 0.7f;
		charm.twoSided = true;
		AddMesh(app.scene, geo::DreamCatcher(0.22f), form("charm", charm), V3(base.x - 2.6f, Ground(base.x, base.z) + 3.42f, base.z + 0.15f));
	}

	// ---- A fallen log with crows on it ----
	{
		const V3 at(5.5f, Ground(5.5f, -15.f), -15.f);
		std::vector<V3> pts = {V3(-4.f, 0.35f, 1.2f), V3(-1.5f, 0.55f, 0.4f), V3(1.f, 0.6f, -0.3f), V3(3.6f, 0.45f, -1.f)};
		geo::Mesh log = geo::Tube(pts, {0.42f, 0.38f, 0.34f, 0.26f}, 16);
		geo::Rng local(3);
		geo::Branch(log, V3(2.6f, 0.6f, -0.7f), V3(0.4f, 1.f, -0.2f), 3.2f, 0.12f, 1, 3, local);
		AddMesh(app.scene, log, treeMat, at);
		const geo::Mesh crow = geo::Bird(0.7f);
		const float spots[5] = {-3.f, -1.9f, -0.4f, 0.8f, 2.1f};
		for (int i = 0; i < 5; ++i)
		{
			const float t = (spots[i] + 4.f) / 7.6f;
			const V3 p = geo::Lerp(pts.front(), pts.back(), t) + V3(0.f, 0.48f, 0.f);
			const HRL_id b = AddMesh(app.scene, crow, perchedMat, at + p, rng.Range(-60.f, 60.f) + 180.f);
			HRL_SetMeshScale(b, 1.15f, 1.15f, 1.15f);
		}
	}

	// ---- The watchers ----
	struct Watcher { float x, z, h, yaw; bool spear; };
	const Watcher watchers[] = {{-2.6f, -27.f, 2.9f, 10.f, true}, {3.4f, -31.f, 2.6f, -15.f, false}, {-7.f, -35.f, 3.1f, 25.f, false},
		{8.f, -38.f, 2.7f, -30.f, false}, {0.8f, -22.f, 2.5f, 5.f, false}, {-12.f, -24.f, 2.8f, 35.f, false}};
	int watcherIndex = 0;
	for (const Watcher& w : watchers)
	{
		geo::Rng local((uint32_t)(int)(w.x * 100.f + w.z * 7.f) + 1000u);
		// Each one dissolves from its own feet: the threshold follows the ground under it.
		const float ground = Ground(w.x, w.z);
		hrl::heaven::Form f = figureForm;
		f.dissolveHeight = ground + 1.1f;
		const HRL_id mat = app.heaven ? form(("figure" + std::to_string(watcherIndex++)).c_str(), f) : figureMat;
		AddMesh(app.scene, geo::Figure(w.h, local, w.spear), mat, V3(w.x, ground - 0.1f, w.z), w.yaw);
	}

	// ---- Crows circling over the clearing ----
	const geo::Mesh crow = geo::Bird(0.9f);
	for (int i = 0; i < 11; ++i)
	{
		Bird b;
		b.center = V3(rng.Range(-6.f, 6.f), rng.Range(6.f, 13.f), rng.Range(-34.f, -18.f));
		b.radius = rng.Range(5.f, 14.f);
		b.speed = rng.Range(0.12f, 0.25f) * (rng.Uniform() < 0.5f ? -1.f : 1.f);
		b.phase = rng.Range(0.f, 6.28f);
		b.height = rng.Range(0.5f, 2.f);
		b.mesh = AddMesh(app.scene, crow, birdMat, b.center);
		app.birds.push_back(b);
	}

	// ---- Tall grass in front ----
	for (int g = 0; g < 8; ++g)
	{
		geo::Mesh tufts;
		const V3 origin(PathX(-4.f * g) + rng.Range(-6.f, 6.f), 0.f, rng.Range(-30.f, 6.f));
		for (int t = 0; t < 14; ++t)
		{
			const float x = origin.x + rng.Range(-7.f, 7.f), z = origin.z + rng.Range(-6.f, 6.f);
			if (std::fabs(x - PathX(z)) < 1.2f) continue;
			geo::Mesh tuft = geo::GrassTuft(rng, 26, 0.35f, rng.Range(0.5f, 1.3f));
			for (HRL_Vertex3D& v : tuft.vertices)
			{
				v.position[0] += x - origin.x;
				v.position[2] += z - origin.z;
				v.position[1] += Ground(x, z);
			}
			tufts.Append(tuft);
		}
		AddMesh(app.scene, tufts, grassMat, origin);
	}

	if (!app.heaven) return;
	hrl::heaven::HeavenStyle& h = *app.heaven;
	// ---- Smoke: banks on the ground, columns rising from the fire, wisps ----
	auto smoke = [&](V3 c, V3 e, float density, float frequency, float erosion, float rise, float softness,
		hrl::heaven::Color emission, float thinning, hrl::heaven::Color albedo)
	{
		hrl::heaven::Smoke s;
		s.center = {c.x, c.y, c.z};
		s.halfExtents = {e.x, e.y, e.z};
		s.density = density;
		s.frequency = frequency;
		s.erosion = erosion;
		s.riseSpeed = rise;
		s.softness = softness;
		s.emission = emission;
		s.thinning = thinning;
		s.albedo = albedo;
		h.AddSmoke(s);
	};
	const hrl::heaven::Color grey{0.72f, 0.64f, 0.58f}, dark{0.45f, 0.4f, 0.37f}, none{0.f, 0.f, 0.f};
	smoke(V3(0.f, 0.4f, -26.f), V3(26.f, 1.6f, 24.f), 0.35f, 0.16f, 0.55f, 0.12f, 0.85f, none, 0.7f, grey);  // ground bank
	smoke(V3(0.f, 0.6f, -30.f), V3(10.f, 1.3f, 5.f), 0.7f, 0.3f, 0.6f, 0.2f, 0.7f, none, 0.6f, grey);       // around the watchers
	smoke(V3(1.5f, 7.f, -41.f), V3(5.f, 9.f, 5.f), 0.8f, 0.22f, 0.5f, 0.7f, 0.6f, {0.9f, 0.32f, 0.1f}, 0.55f, grey); // fire column
	smoke(V3(-9.f, 5.f, -36.f), V3(4.f, 7.f, 4.f), 0.55f, 0.25f, 0.6f, 0.45f, 0.6f, none, 0.6f, grey);     // second column
	smoke(V3(-15.f, 13.f, -50.f), V3(16.f, 6.f, 10.f), 0.25f, 0.12f, 0.5f, 0.15f, 0.8f, none, 0.3f, dark);  // cloud above
	smoke(V3(-3.f, 1.3f, -5.f), V3(2.6f, 1.4f, 2.2f), 0.5f, 0.45f, 0.75f, 0.25f, 0.7f, none, 0.6f, grey);    // wisps near the camera
	smoke(V3(7.f, 2.5f, -17.f), V3(3.f, 3.f, 3.f), 0.5f, 0.35f, 0.65f, 0.35f, 0.65f, none, 0.6f, grey);      // behind the log
	h.Update();

	// Diagnostics: one of the inputs of the style on screen.
	if (!opt.debug.empty())
	{
		hrl::CustomShaderRenderer& r = h.Renderer();
		std::string body;
		if (opt.debug == "noise") body = "vec4 n = hv_Noise(vec3(vUV * 2.0, 0.3)); outColor = vec4(n.rgb, 1.0);";
		else if (opt.debug == "media") body = "vec4 m = texture(uMediaVolume, vec3(vUV, 1.0)); outColor = vec4(m.rgb + vec3(0.0, 0.0, m.a), 1.0);";
		else if (opt.debug == "smoke") body = "vec4 s = texture(DebugSmoke, vUV); outColor = vec4(s.rgb + vec3(0.0, 0.0, s.a * 0.2), 1.0);";
		r.AddFullscreenPass("debug", HRL_STAGE_BEFORE_TONEMAP, 1000,
			"HRL_TEXTURE2D(DebugSmoke);\nlayout(location = 0) out vec4 outColor;\nvoid main() { " + body + " }\n");
		r.Bind("debug", "DebugSmoke", "Smoke", HRL_PASS_READ);
		r.Output("debug", 0, "scene", HRL_PASS_BLEND_NONE);
		r.Enable("lens", false);
		if (!r.Ok()) std::fprintf(stderr, "debug: %s\n", r.LastError().c_str());
	}

	// ---- Embers ----
	const HRL_id emberMat = h.CreateEmberMaterial("ember", {16.f, 4.6f, 1.2f}, 9.f);
	const geo::Mesh ember = geo::Ellipsoid(V3(0, 0, 0), V3(0.02f, 0.02f, 0.02f), 4, 6);
	for (int e = 0; e < 240; ++e)
	{
		const float z = rng.Range(-46.f, 2.f);
		const float x = PathX(z) + rng.Range(-12.f, 12.f) * (0.4f + 0.6f * (-z / 46.f));
		AddMesh(app.scene, ember, emberMat, V3(x, Ground(x, z) + rng.Range(0.f, 1.f), z));
	}
	(void)opt;
}

// Cinematic camera: a slow walk towards the fire.
void CinematicCamera(App& app, float t)
{
	const float loop = std::fmod(t, 80.f);
	app.camZ = 7.f - loop * 0.32f;
	app.camX = PathX(app.camZ) + 0.6f * std::sin(t * 0.11f);
	app.camY = Ground(app.camX, app.camZ) + 1.55f + 0.1f * std::sin(t * 0.37f);
	app.yaw = -90.f + 7.f * std::sin(t * 0.07f) + 3.f * std::sin(t * 0.19f);
	app.pitch = 4.f + 2.f * std::sin(t * 0.13f);
}

void Animate(App& app, float t)
{
	if (app.cinematic) CinematicCamera(app, t);
	HRL_SetCameraLocation(app.camera, app.camX, app.camY, app.camZ);
	HRL_SetCameraRotation(app.camera, app.pitch, app.yaw, 0.f);
	for (const Bird& b : app.birds)
	{
		const float a = b.phase + t * b.speed;
		const V3 p = b.center + V3(std::cos(a) * b.radius, std::sin(a * 2.3f) * b.height, std::sin(a) * b.radius);
		// Tangent of the circle: where the bird flies.
		const float vx = -std::sin(a) * b.speed, vz = std::cos(a) * b.speed;
		HRL_SetMeshLocation(b.mesh, p.x, p.y, p.z);
		HRL_SetMeshRotation(b.mesh, 0.f, std::atan2(vx, vz) * 57.2957795f, b.speed > 0.f ? -18.f : 18.f);
	}
	if (app.heaven) app.heaven->Update();
}

#ifdef HRL_EXAMPLE_HAS_GLFW
extern "C" int glfwCreateWindowSurface(void* instance, GLFWwindow* window, const void* allocator, void* surface);
int CreateSurface(void* instance, void* surfaceOut, void* user)
{
	return glfwCreateWindowSurface(instance, static_cast<GLFWwindow*>(user), nullptr, surfaceOut);
}

int RunWindow(const Options& opt)
{
	if (!glfwInit()) { std::fprintf(stderr, "GLFW initialisation failed\n"); return 1; }
	if (!glfwVulkanSupported()) { std::fprintf(stderr, "No Vulkan loader / driver found\n"); glfwTerminate(); return 1; }
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(opt.width, opt.height, "HRL - Heaven", nullptr, nullptr);
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
	std::printf("GPU: %s\n", HRL_Vulkan_GetDeviceName());
	HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_TAA);
	HRL_Vulkan_SetRayTracedShadows(opt.rayTracedShadows ? HRL_TRUE : HRL_FALSE);

	App app;
	BuildScene(app, opt);
	double start = glfwGetTime(), last = start, lastX = 0, lastY = 0, fpsTime = 0;
	int fpsFrames = 0, shot = 0, frame = 0;
	bool looking = false, cWas = false, hWas = false, pWas = false;
	while (!glfwWindowShouldClose(window) && (opt.frames == 0 || frame++ < opt.frames))
	{
		glfwPollEvents();
		const double now = glfwGetTime();
		const float dt = (float)(now - last);
		last = now;
		fpsTime += dt; ++fpsFrames;
		if (fpsTime >= 1.0)
		{
			char title[96];
			std::snprintf(title, sizeof(title), "HRL - Heaven - %d FPS%s", fpsFrames, app.heaven ? "" : " (plain)");
			glfwSetWindowTitle(window, title);
			fpsTime = 0; fpsFrames = 0;
		}
		int w = 0, h = 0;
		glfwGetFramebufferSize(window, &w, &h);
		if (w != fbw || h != fbh) { fbw = w; fbh = h; HRL_WindowResizeCallback(w, h); }
		if (w == 0 || h == 0) { glfwWaitEvents(); continue; }
		auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
		if (key(GLFW_KEY_ESCAPE)) glfwSetWindowShouldClose(window, 1);
		if (key(GLFW_KEY_C) && !cWas) app.cinematic = !app.cinematic;
		if (key(GLFW_KEY_H) && !hWas) EnableHeaven(app, !app.heaven);
		if (key(GLFW_KEY_P) && !pWas)
		{
			const std::string path = "hrl_heaven_" + std::to_string(shot++) + ".png";
			HRL_TakeScreenshot(app.scene, path.c_str());
			std::printf("screenshot: %s\n", path.c_str());
		}
		cWas = key(GLFW_KEY_C); hWas = key(GLFW_KEY_H); pWas = key(GLFW_KEY_P);
		if (!app.cinematic)
		{
			double mx = 0, my = 0;
			glfwGetCursorPos(window, &mx, &my);
			const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
			if (rmb && looking)
			{
				app.yaw += (float)(mx - lastX) * 0.15f;
				app.pitch = std::max(-89.f, std::min(89.f, app.pitch - (float)(my - lastY) * 0.15f));
			}
			looking = rmb; lastX = mx; lastY = my;
			const float p = app.pitch * geo::kPi / 180.f, y = app.yaw * geo::kPi / 180.f;
			const float speed = (key(GLFW_KEY_LEFT_SHIFT) ? 12.f : 3.f) * dt;
			const float f = (key(GLFW_KEY_W) ? 1.f : 0.f) - (key(GLFW_KEY_S) ? 1.f : 0.f);
			const float r = (key(GLFW_KEY_D) ? 1.f : 0.f) - (key(GLFW_KEY_A) ? 1.f : 0.f);
			const float u = (key(GLFW_KEY_E) ? 1.f : 0.f) - (key(GLFW_KEY_Q) ? 1.f : 0.f);
			app.camX += (std::cos(p) * std::cos(y) * f - std::sin(y) * r) * speed;
			app.camY += (std::sin(p) * f + u) * speed;
			app.camZ += (std::cos(p) * std::sin(y) * f + std::cos(y) * r) * speed;
		}
		Animate(app, opt.time + (float)(now - start));
		HRL_BeginFrame();
		HRL_EndFrame();
	}
	app.heaven.reset();
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
	std::printf("GPU: %s\n", HRL_Vulkan_GetDeviceName());
	HRL_Vulkan_SetAntiAliasing(HRL_VULKAN_AA_TAA);
	HRL_Vulkan_SetRayTracedShadows(opt.rayTracedShadows ? HRL_TRUE : HRL_FALSE);
	App app;
	BuildScene(app, opt);
	if (opt.fixedCamera)
	{
		app.cinematic = false;
		app.camX = opt.camera[0]; app.camY = opt.camera[1]; app.camZ = opt.camera[2];
		app.pitch = opt.camera[3]; app.yaw = opt.camera[4];
	}
	// The temporal accumulations (froxels, smoke, anti-aliasing) need a few frames.
	const int frames = opt.frames > 0 ? opt.frames : 24;
	for (int i = 0; i < frames; ++i)
	{
		Animate(app, opt.time + (float)i / 30.f);
		HRL_BeginFrame();
		if (i == frames - 1) HRL_TakeScreenshot(app.scene, opt.screenshot.c_str());
		HRL_EndFrame();
	}
	app.heaven.reset();
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
		else if (a == "--time" && i + 1 < argc) opt.time = (float)std::atof(argv[++i]);
		else if (a == "--plain") opt.plain = true;
		else if (a == "--debug" && i + 1 < argc) opt.debug = argv[++i];
		else if (a == "--camera" && i + 5 < argc)
		{
			opt.fixedCamera = true;
			for (float& v : opt.camera) v = (float)std::atof(argv[++i]);
		}
		else if (a == "--rt") opt.rayTracedShadows = true;
		else if (a == "--set" && i + 1 < argc)
		{
			const std::string kv = argv[++i];
			const size_t eq = kv.find('=');
			if (eq == std::string::npos) { std::fprintf(stderr, "--set name=value\n"); return 1; }
			opt.overrides.push_back({kv.substr(0, eq), (float)std::atof(kv.c_str() + eq + 1)});
		}
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
	}
	if (opt.width < 16 || opt.height < 16 || opt.frames < 0) { std::fprintf(stderr, "invalid size / frame count\n"); return 1; }
	if (!opt.screenshot.empty()) return RunOffscreen(opt);
#ifdef HRL_EXAMPLE_HAS_GLFW
	return RunWindow(opt);
#else
	std::fprintf(stderr, "built without GLFW: use --screenshot <file.png>\n");
	return 1;
#endif
}
