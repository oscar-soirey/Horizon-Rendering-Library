// HRL extras - "Heaven": a dark, heavy, smoke filled look for a viewport,
// built on CustomShaderRenderer and the programmable render pipeline (Vulkan).
//
// What it adds to the viewport:
//   * participating media in froxels (1/8 resolution, exponential slices),
//     lit by every light with its shadows (shafts through the silhouettes),
//     with an animated height fog and the smoke volumes, accumulated over
//     frames; HRL shades every surface and the sky through it;
//   * smoke volumes (AddSmoke): ellipsoids of billowing Perlin-Worley noise,
//     ray marched at half resolution against the depth, self shadowed, lit by
//     the lights (multiple scattering approximation), emitting light if asked,
//     shadowing the sun on the surfaces and in the media, accumulated over
//     frames, upsampled with the depth and composited under the translucent
//     surfaces;
//   * a lens (heat haze, soft corners, fringes, halation) and a warm grade
//     with crushed blacks, then the built-in bloom, ACES, vignette and grain;
//   * surface materials (CreateFormMaterial, CreateEmberMaterial): smooth,
//     nearly black shapes outlined by the light behind them that can tear
//     into smoke, and rising embers.
//
//   hrl::heaven::HeavenStyle heaven(scene, viewport);
//   heaven.SetGlow(0.f, 3.f, -30.f, 9.f, 4.6f, 2.f);
//   heaven.AddSmoke({...});
//   ... every frame: heaven.Update();
//
// Header-only. Needs the Vulkan backend built with glslang (run time shaders).
#ifndef HRL_EXTRAS_HEAVEN_STYLE_HPP
#define HRL_EXTRAS_HEAVEN_STYLE_HPP

#include "custom_shader_renderer.hpp"
#include "heaven_noise.hpp"
#include "heaven_shaders.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace hrl::heaven
{
struct Color { float r, g, b; };
struct Vec3 { float x, y, z; };

struct Settings
{
	// Light source the forms are outlined by (and the default embers colour).
	Vec3 glowPosition{0.f, 4.f, -30.f};
	Color glowColor{9.f, 4.6f, 2.f};

	// Participating media.
	float mediaDistance = 110.f;      // froxels reach this view depth
	float froxelScale = 1.f / 8.f;    // of the viewport resolution
	HRL_uint froxelSlices = 96;
	Color fogAlbedo{1.f, 0.86f, 0.72f};
	float fogDensity = 0.017f;        // extinction per metre at the base height
	float fogHeightFalloff = 0.12f;
	float fogBaseHeight = 0.f;
	float fogNoiseFrequency = 0.03f;
	float fogNoiseAmount = 1.f;
	Vec3 fogWind{0.5f, 0.06f, 0.18f};
	Color fogAmbient{0.02f, 0.012f, 0.008f};
	float fogAnisotropy = 0.55f;
	float fogTemporalBlend = 0.08f;

	// Smoke volumes.
	float smokeScale = 0.5f;          // of the viewport resolution
	float smokeSteps = 40.f;
	float smokeShadowSteps = 5.f;
	float smokeShadowDistance = 12.f;
	float smokeMaxDistance = 150.f;
	Color smokeAmbient{0.02f, 0.012f, 0.008f};
	float smokeMultipleScattering = 0.55f;
	float smokeTemporalBlend = 0.2f;

	// Lens and grade (HDR).
	float hazeDistortion = 0.003f;
	float edgeBlur = 6.f;
	float aberration = 2.5f;
	float halationStrength = 0.18f;
	Color grade{1.f, 0.82f, 0.62f};
	float crush = 1.15f;
	float desaturate = 0.2f;

	// Built-in post-process (after the lens).
	float exposure = -0.6f;           // stops
	float bloomStrength = 2.5f;
	float contrast = 1.2f;
	float saturation = 0.9f;
	float vignetteStrength = 0.85f;
	float vignetteRadius = 0.55f;
	float vignetteSoftness = 0.55f;
	float filmGrain = 0.045f;
};

// A smoke volume: an ellipsoid of noise.
struct Smoke
{
	Vec3 center{0.f, 1.f, 0.f};
	Vec3 halfExtents{2.f, 2.f, 2.f};
	float density = 1.5f;             // extinction per metre in the thickest parts
	Color albedo{0.75f, 0.68f, 0.62f};
	float anisotropy = 0.45f;         // forward scattering: bright against the light
	float frequency = 0.35f;          // of the billows, per metre
	float erosion = 0.55f;            // wisps torn from the edges, 0..1
	float riseSpeed = 0.35f;          // m/s
	float softness = 0.6f;            // of the ellipsoid edge, 0..1
	Color emission{0.f, 0.f, 0.f};    // light emitted per unit of density
	float thinning = 0.5f;            // thinner towards the top, 0..1
};

// A smooth form (CreateFormMaterial).
struct Form
{
	Color color{0.03f, 0.02f, 0.015f};
	float roughness = 0.85f;
	float rimStrength = 0.3f;
	float rimPower = 5.f;
	float breath = 0.02f;             // soft swelling of the surface (metres)
	float sway = 0.f;                 // trees / figures: sway growing with the height
	float swayBase = 0.f;
	float flap = 0.f;                 // birds: wing flap amplitude (metres)
	float dissolve = 0.f;             // 0 solid, 1 torn into smoke (alpha tested)
	float dissolveHeight = 0.f;       // the dissolve grows below this height, 0 : everywhere
	bool twoSided = false;
};

class HeavenStyle
{
public:
	HeavenStyle(HRL_id scene, HRL_id viewport, const Settings& settings = Settings())
		: renderer_(scene, viewport), settings_(settings)
	{
		Build();
	}
	~HeavenStyle()
	{
		renderer_.Release();
		if (noise_ && HRL_IsValidTexture(noise_)) HRL_DeleteTexture(noise_);
	}
	HeavenStyle(const HeavenStyle&) = delete;
	HeavenStyle& operator=(const HeavenStyle&) = delete;

	bool Ok() const { return renderer_.Ok(); }
	const std::string& LastError() const { return renderer_.LastError(); }
	CustomShaderRenderer& Renderer() { return renderer_; }
	const Settings& GetSettings() const { return settings_; }
	HRL_id NoiseTexture() const { return noise_; }

	// ---- Light the forms are outlined by --------------------------------------------
	void SetGlow(float x, float y, float z, float r, float g, float b)
	{
		settings_.glowPosition = {x, y, z};
		settings_.glowColor = {r, g, b};
		renderer_.SetGlobalVec3("HvGlowPosition", x, y, z);
		renderer_.SetGlobalVec3("HvGlowColor", r, g, b);
	}

	// ---- Smoke volumes ------------------------------------------------------------------
	int AddSmoke(const Smoke& s)
	{
		const int id = nextSmoke_++;
		smoke_[id] = s;
		dirty_ = true;
		return id;
	}
	void SetSmoke(int id, const Smoke& s) { if (smoke_.count(id)) { smoke_[id] = s; dirty_ = true; } }
	void RemoveSmoke(int id) { dirty_ = smoke_.erase(id) != 0 || dirty_; }
	Smoke* GetSmoke(int id) { auto it = smoke_.find(id); dirty_ = true; return it != smoke_.end() ? &it->second : nullptr; }

	// ---- Materials -------------------------------------------------------------------
	HRL_id CreateFormMaterial(const std::string& name, const Form& f)
	{
		const HRL_id m = renderer_.CreateSurfaceMaterial(name, "form");
		if (!m) return m;
		renderer_.SetVec3(name, "FormColor", f.color.r, f.color.g, f.color.b);
		renderer_.SetFloat(name, "FormRoughness", f.roughness);
		renderer_.SetFloat(name, "RimStrength", f.rimStrength);
		renderer_.SetFloat(name, "RimPower", f.rimPower);
		renderer_.SetFloat(name, "Breath", f.breath);
		renderer_.SetFloat(name, "Sway", f.sway);
		renderer_.SetFloat(name, "SwayBase", f.swayBase);
		renderer_.SetFloat(name, "Flap", f.flap);
		renderer_.SetFloat(name, "Dissolve", f.dissolve);
		renderer_.SetFloat(name, "DissolveHeight", f.dissolveHeight);
		if (f.dissolve > 0.f)
		{
			// Alpha tested: the torn parts are cut, the rest keeps its depth and shadows.
			HRL_MaterialSetInt(m, "OpacityUseValue", 1);
			HRL_MaterialSetFloat(m, "OpacityValue", 1.f);
		}
		if (f.twoSided) HRL_MaterialSetBool(m, HRL_MATERIAL_PARAM_TWO_SIDED, HRL_TRUE);
		return m;
	}
	HRL_id CreateEmberMaterial(const std::string& name, Color color = {14.f, 4.2f, 1.1f}, float height = 10.f)
	{
		const HRL_id m = renderer_.CreateSurfaceMaterial(name, "ember");
		if (!m) return m;
		renderer_.SetVec3(name, "EmberColor", color.r, color.g, color.b);
		renderer_.SetFloat(name, "EmberHeight", height);
		return m;
	}

	// Uploads what changed (smoke volumes). Call once per frame, before HRL_EndFrame.
	void Update()
	{
		if (!dirty_) return;
		dirty_ = false;
		std::vector<float> data(4, 0.f);
		for (const auto& [id, s] : smoke_)
		{
			const float radius = std::max(s.halfExtents.x, std::max(s.halfExtents.y, s.halfExtents.z));
			const float v[20] = {
				s.center.x, s.center.y, s.center.z, radius,
				s.halfExtents.x, s.halfExtents.y, s.halfExtents.z, s.density,
				s.albedo.r, s.albedo.g, s.albedo.b, s.anisotropy,
				s.frequency, s.erosion, s.riseSpeed, s.softness,
				s.emission.r, s.emission.g, s.emission.b, s.thinning};
			data.insert(data.end(), v, v + 20);
		}
		data[0] = (float)smoke_.size();
		renderer_.SetBuffer("froxel.inject", "Volumes", data);
		renderer_.SetBuffer("smoke.march", "Volumes", data);
		// No volume: the half resolution passes have nothing to do.
		renderer_.Enable("smoke.march", !smoke_.empty());
		renderer_.Enable("smoke.composite", !smoke_.empty());
	}

	// Applies `settings` (the targets keep their sizes: those are read at construction).
	void Apply(const Settings& s)
	{
		settings_ = s;
		CustomShaderRenderer& r = renderer_;
		SetGlow(s.glowPosition.x, s.glowPosition.y, s.glowPosition.z, s.glowColor.r, s.glowColor.g, s.glowColor.b);
		r.SetVec3("froxel.inject", "FogAlbedo", s.fogAlbedo.r, s.fogAlbedo.g, s.fogAlbedo.b);
		r.SetFloat("froxel.inject", "FogDensity", s.fogDensity);
		r.SetFloat("froxel.inject", "FogHeightFalloff", s.fogHeightFalloff);
		r.SetFloat("froxel.inject", "FogBaseHeight", s.fogBaseHeight);
		r.SetFloat("froxel.inject", "FogNoiseFrequency", s.fogNoiseFrequency);
		r.SetFloat("froxel.inject", "FogNoiseAmount", s.fogNoiseAmount);
		r.SetVec3("froxel.inject", "FogWind", s.fogWind.x, s.fogWind.y, s.fogWind.z);
		r.SetVec3("froxel.inject", "FogAmbient", s.fogAmbient.r, s.fogAmbient.g, s.fogAmbient.b);
		r.SetFloat("froxel.inject", "FogAnisotropy", s.fogAnisotropy);
		r.SetFloat("froxel.inject", "TemporalBlend", s.fogTemporalBlend);
		r.SetFloat("smoke.march", "Steps", s.smokeSteps);
		r.SetFloat("smoke.march", "ShadowSteps", s.smokeShadowSteps);
		r.SetFloat("smoke.march", "ShadowDistance", s.smokeShadowDistance);
		r.SetFloat("smoke.march", "MaxDistance", s.smokeMaxDistance);
		r.SetVec3("smoke.march", "SmokeAmbient", s.smokeAmbient.r, s.smokeAmbient.g, s.smokeAmbient.b);
		r.SetFloat("smoke.march", "MultipleScattering", s.smokeMultipleScattering);
		r.SetFloat("smoke.march", "TemporalBlend", s.smokeTemporalBlend);
		r.SetFloat("lens", "HazeDistortion", s.hazeDistortion);
		r.SetFloat("lens", "EdgeBlur", s.edgeBlur);
		r.SetFloat("lens", "Aberration", s.aberration);
		r.SetFloat("lens", "HalationStrength", s.halationStrength);
		r.SetVec3("lens", "Grade", s.grade.r, s.grade.g, s.grade.b);
		r.SetFloat("lens", "Crush", s.crush);
		r.SetFloat("lens", "Desaturate", s.desaturate);
		const HRL_id g = r.Grading();
		HRL_MaterialSetFloat(g, "exposure", s.exposure);
		HRL_MaterialSetFloat(g, "bloomStrength", s.bloomStrength);
		HRL_MaterialSetFloat(g, "contrast", s.contrast);
		HRL_MaterialSetFloat(g, "saturation", s.saturation);
		HRL_MaterialSetFloat(g, "vignetteStrength", s.vignetteStrength);
		HRL_MaterialSetFloat(g, "vignetteRadius", s.vignetteRadius);
		HRL_MaterialSetFloat(g, "vignetteSoftness", s.vignetteSoftness);
		HRL_MaterialSetVec3(g, "vignetteColor", 0.f, 0.f, 0.f);
		HRL_MaterialSetFloat(g, "filmGrainStrength", s.filmGrain);
		HRL_MaterialSetFloat(g, "filmGrainScale", 1.f);
	}

private:
	void Build()
	{
		CustomShaderRenderer& r = renderer_;
		const Settings& s = settings_;
		r.SetSharedCode(glsl::kCommon);
		const std::vector<unsigned char> atlas = BakeNoiseAtlas();
		noise_ = HRL_CreateTextureFromPixels(atlas.data(), kNoiseAtlas, kNoiseAtlas);
		HRL_SetTextureMinFilter(noise_, HRL_FILTER_LINEAR);
		HRL_SetTextureMagFilter(noise_, HRL_FILTER_LINEAR);
		r.SetGlobalTexture("NoiseAtlas", noise_);

		// Froxels: scattering (with history), visibility of the sun, integrated media.
		r.AddTarget("Scatter", HRL_RENDER_TARGET_RGBA16F, s.froxelScale, s.froxelSlices, true);
		r.AddTarget("Visibility", HRL_RENDER_TARGET_RGBA16F, s.froxelScale, s.froxelSlices);
		r.AddTarget("Integrated", HRL_RENDER_TARGET_RGBA16F, s.froxelScale, s.froxelSlices);
		r.AddComputePass("froxel.inject", HRL_STAGE_BEFORE_OPAQUE, -100, std::string(glsl::kVolumesBuffer) + glsl::kFroxelInject);
		r.Bind("froxel.inject", "Scatter", "Scatter", HRL_PASS_WRITE);
		r.Bind("froxel.inject", "ScatterHistory", "Scatter", HRL_PASS_READ_PREVIOUS);
		r.Bind("froxel.inject", "Visibility", "Visibility", HRL_PASS_WRITE);
		r.DispatchOver("froxel.inject", "Scatter");
		r.AddComputePass("froxel.integrate", HRL_STAGE_BEFORE_OPAQUE, -99, glsl::kFroxelIntegrate);
		r.Bind("froxel.integrate", "Scatter", "Scatter", HRL_PASS_READ);
		r.Bind("froxel.integrate", "Integrated", "Integrated", HRL_PASS_WRITE);
		r.DispatchOver("froxel.integrate", "Integrated");
		HRL_SetRenderPassDispatchDivisor(r.Pass("froxel.integrate"), 1, 1, s.froxelSlices);
		r.SetMediaVolume("Integrated", "Visibility", s.mediaDistance, 2.f);

		// Smoke volumes: half resolution march (with history), upsampled into the scene.
		r.AddTarget("Smoke", HRL_RENDER_TARGET_RGBA16F, s.smokeScale, 1, true);
		r.AddTarget("SmokeDepth", HRL_RENDER_TARGET_RG16F, s.smokeScale, 1);
		r.AddComputePass("smoke.march", HRL_STAGE_AFTER_OPAQUE, -100, std::string(glsl::kVolumesBuffer) + glsl::kSmokeMarch);
		r.Bind("smoke.march", "Smoke", "Smoke", HRL_PASS_WRITE);
		r.Bind("smoke.march", "SmokeHistory", "Smoke", HRL_PASS_READ_PREVIOUS);
		r.Bind("smoke.march", "SmokeDepth", "SmokeDepth", HRL_PASS_WRITE);
		r.DispatchOver("smoke.march", "Smoke");
		r.AddComputePass("smoke.composite", HRL_STAGE_AFTER_OPAQUE, -99, glsl::kSmokeComposite);
		r.Bind("smoke.composite", "Smoke", "Smoke", HRL_PASS_READ);
		r.Bind("smoke.composite", "SmokeDepth", "SmokeDepth", HRL_PASS_READ);

		// Lens and grade on the final HDR image.
		r.AddComputePass("lens", HRL_STAGE_BEFORE_TONEMAP, 100, glsl::kLens);

		// Surfaces.
		r.AddSurfaceShader("form", glsl::kFormVertex, glsl::kFormSurface);
		r.AddSurfaceShader("ember", glsl::kEmberVertex, glsl::kEmberSurface);

		Apply(s);
		Update();
	}

	CustomShaderRenderer renderer_;
	Settings settings_;
	HRL_id noise_ = 0;
	std::map<int, Smoke> smoke_;
	int nextSmoke_ = 1;
	bool dirty_ = true;
};
} // namespace hrl::heaven

#endif
