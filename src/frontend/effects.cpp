// HRL frontend - fog, volumetrics, ambient occlusion, reflections, clouds, decals.
#include "frontend_internal.h"

void HRL_SetFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogEnabled: invalid scene ID");
		return;
	}
	it->second->fog.enabled = enable;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogMode(HRL_id scene, HRL_EFogType mode)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogMode: invalid scene ID");
		return;
	}
	it->second->fog.mode = mode;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.r = r;
	it->second->fog.g = g;
	it->second->fog.b = b;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.density = density;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

void HRL_SetFogLinearRange(HRL_id scene, float start, float end)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetFogColor: invalid scene ID");
		return;
	}
	it->second->fog.range_start = start;
	it->second->fog.range_end = end;
	g_Backend.RHI_FogPropertyChanged(scene, &it->second->fog);
}

HRL_id HRL_CreateVolumetricFog(HRL_id scene)
{
	auto scene_it = ctx_.scenes.find(scene);
	if (scene_it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: invalid scene ID");
		return HRL_INVALID_ID;
	}

	if (scene_it->second->volumetric_fogs.size() >= HRL_MAX_VOLUMETRIC_FOGS)
	{
		SetErrorCode(HRL_INVALID_OPERATION, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: scene reached HRL_MAX_VOLUMETRIC_FOGS");
		return HRL_INVALID_ID;
	}

	auto* fog = new (std::nothrow) HRL_VolumetricFog();
	if (!fog)
	{
		SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR,
			"HRL_CreateVolumetricFog: failed to allocate volumetric fog");
		return HRL_INVALID_ID;
	}

	const HRL_id newId = GenerateHRL_ID();
	fog->id_ = newId;
	fog->scene_ = scene;

	scene_it->second->volumetric_fogs.emplace(newId, fog);
	ctx_.volumetric_fogs.emplace(newId, fog);
	return newId;
}

void HRL_DeleteVolumetricFog(HRL_id fogid)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_DeleteVolumetricFog: invalid ID");
		return;
	}

	HRL_VolumetricFog* fog = it->second;
	const HRL_id sceneId = fog->scene_;
	auto scene_it = ctx_.scenes.find(sceneId);
	if (scene_it != ctx_.scenes.end())
		scene_it->second->volumetric_fogs.erase(fogid);

	delete fog;
	ctx_.volumetric_fogs.erase(it);
}

void HRL_SetVolumetricFogEnabled(HRL_id fogid, int enable)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogEnabled: invalid volumetric fog ID");
		return;
	}
	it->second->enabled = (enable != HRL_FALSE);
}

void HRL_SetVolumetricFogPosition(HRL_id fogid, float x, float y, float z)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogPosition: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogPosition: position must contain finite values");
		return;
	}
	it->second->position = glm::vec3(x, y, z);
}

void HRL_SetVolumetricFogRadius(HRL_id fogid, float radius)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogRadius: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(radius) || radius <= 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogRadius: radius must be > 0");
		return;
	}
	it->second->radius = radius;
}

void HRL_SetVolumetricFogDensity(HRL_id fogid, float density)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogDensity: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(density) || density < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogDensity: density must be >= 0");
		return;
	}
	it->second->density = density;
}

void HRL_SetVolumetricFogColor(HRL_id fogid, float r, float g, float b)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogColor: invalid volumetric fog ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogColor: color must contain finite values");
		return;
	}
	it->second->color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetVolumetricFogSteps(HRL_id fogid, HRL_uint steps)
{
	auto it = ctx_.volumetric_fogs.find(fogid);
	if (it == ctx_.volumetric_fogs.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetVolumetricFogSteps: invalid volumetric fog ID");
		return;
	}
	it->second->steps = std::max<HRL_uint>(4u, std::min<HRL_uint>(64u, steps));
}

void HRL_SetGlobalVolumetricFogEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogEnabled: invalid scene ID");
		return;
	}
	it->second->global_volumetric_fog.enabled = (enable != HRL_FALSE);
}

void HRL_SetGlobalVolumetricFogDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogDensity: invalid scene ID");
		return;
	}
	if (!std::isfinite(density) || density < 0.f)
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogDensity: density must be >= 0");
		return;
	}
	it->second->global_volumetric_fog.density = density;
}

void HRL_SetGlobalVolumetricFogColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogColor: invalid scene ID");
		return;
	}
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
	{
		SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogColor: color must contain finite values");
		return;
	}
	it->second->global_volumetric_fog.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetGlobalVolumetricFogSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end())
	{
		SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR,
			"HRL_SetGlobalVolumetricFogSteps: invalid scene ID");
		return;
	}
	it->second->global_volumetric_fog.steps = std::max<HRL_uint>(4u, std::min<HRL_uint>(64u, steps));
}

void HRL_SetGodRaysEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysEnabled: invalid scene ID"); return; }
	it->second->god_rays.enabled = (enable != HRL_FALSE);
}

void HRL_SetGodRaysPosition(HRL_id scene, float x, float y, float z)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysPosition: invalid scene ID"); return; }
	it->second->god_rays.position = glm::vec3(x, y, z);
}

void HRL_SetGodRaysColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysColor: invalid scene ID"); return; }
	it->second->god_rays.color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.f), glm::vec3(1.f));
}

void HRL_SetGodRaysDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDensity: invalid scene ID"); return; }
	it->second->god_rays.density = glm::clamp(density, 0.f, 2.f);
}

void HRL_SetGodRaysDecay(HRL_id scene, float decay)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysDecay: invalid scene ID"); return; }
	it->second->god_rays.decay = glm::clamp(decay, 0.8f, 0.999f);
}

void HRL_SetGodRaysWeight(HRL_id scene, float weight)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysWeight: invalid scene ID"); return; }
	it->second->god_rays.weight = glm::clamp(weight, 0.f, 4.f);
}

void HRL_SetGodRaysSamples(HRL_id scene, HRL_uint samples)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetGodRaysSamples: invalid scene ID"); return; }
	it->second->god_rays.samples = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, samples));
}


void HRL_SetAmbientOcclusionEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionEnabled: invalid scene ID"); return; }
	it->second->ambient_occlusion_enabled = enable != 0;
}

void HRL_SetAmbientOcclusionStrength(HRL_id scene, float strength)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionStrength: invalid scene ID"); return; }
	if (!std::isfinite(strength) || strength < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionStrength: strength must be finite and >= 0"); return; }
	it->second->ambient_occlusion_strength = std::min(strength, 1.0f);
}

void HRL_SetAmbientOcclusionRadius(HRL_id scene, float radius)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionRadius: invalid scene ID"); return; }
	if (!std::isfinite(radius) || radius <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionRadius: radius must be finite and > 0"); return; }
	it->second->ambient_occlusion_radius = radius;
}

void HRL_SetAmbientOcclusionBias(HRL_id scene, float bias)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionBias: invalid scene ID"); return; }
	if (!std::isfinite(bias) || bias < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionBias: bias must be finite and >= 0"); return; }
	it->second->ambient_occlusion_bias = bias;
}

void HRL_SetAmbientOcclusionPower(HRL_id scene, float power)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionPower: invalid scene ID"); return; }
	if (!std::isfinite(power) || power <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetAmbientOcclusionPower: power must be finite and > 0"); return; }
	it->second->ambient_occlusion_power = power;
}

void HRL_SetScreenSpaceReflectionsEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsEnabled: invalid scene ID"); return; }
	it->second->screen_space_reflections_enabled = (enable != 0);
}

void HRL_SetScreenSpaceReflectionsStrength(HRL_id scene, float strength)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsStrength: invalid scene ID"); return; }
	if (!std::isfinite(strength)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsStrength: strength must be finite"); return; }
	it->second->screen_space_reflections_strength = std::max(0.0f, strength);
}

void HRL_SetScreenSpaceReflectionsMaxDistance(HRL_id scene, float distance)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsMaxDistance: invalid scene ID"); return; }
	if (!std::isfinite(distance) || distance <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsMaxDistance: distance must be > 0"); return; }
	it->second->screen_space_reflections_max_distance = distance;
}

void HRL_SetScreenSpaceReflectionsThickness(HRL_id scene, float thickness)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsThickness: invalid scene ID"); return; }
	if (!std::isfinite(thickness) || thickness <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsThickness: thickness must be > 0"); return; }
	it->second->screen_space_reflections_thickness = thickness;
}

void HRL_SetScreenSpaceReflectionsFade(HRL_id scene, float start, float end)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsFade: invalid scene ID"); return; }
	if (!std::isfinite(start) || !std::isfinite(end) || end <= start) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsFade: invalid fade range"); return; }
	it->second->screen_space_reflections_fade_start = glm::clamp(start, 0.0f, 0.99f);
	it->second->screen_space_reflections_fade_end = glm::clamp(end, 0.0f, 1.0f);
	if (it->second->screen_space_reflections_fade_end <= it->second->screen_space_reflections_fade_start)
		it->second->screen_space_reflections_fade_end = std::min(1.0f, it->second->screen_space_reflections_fade_start + 0.01f);
}

void HRL_SetScreenSpaceReflectionsSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetScreenSpaceReflectionsSteps: invalid scene ID"); return; }
	it->second->screen_space_reflections_steps = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, steps));
}

void HRL_SetVolumetricCloudEnabled(HRL_id scene, int enable)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudEnabled: invalid scene ID"); return; }
	it->second->volumetric_cloud_enabled = (enable != HRL_FALSE);
}

void HRL_SetVolumetricCloudCoverage(HRL_id scene, float coverage)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudCoverage: invalid scene ID"); return; }
	if (!std::isfinite(coverage)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudCoverage: coverage must be finite"); return; }
	it->second->volumetric_cloud_coverage = glm::clamp(coverage, 0.0f, 1.0f);
}

void HRL_SetVolumetricCloudDensity(HRL_id scene, float density)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDensity: invalid scene ID"); return; }
	if (!std::isfinite(density) || density < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDensity: density must be finite and >= 0"); return; }
	it->second->volumetric_cloud_density = density;
}

void HRL_SetVolumetricCloudHeight(HRL_id scene, float min_height, float max_height)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudHeight: invalid scene ID"); return; }
	if (!std::isfinite(min_height) || !std::isfinite(max_height) || max_height <= min_height + 0.001f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudHeight: max height must be greater than min height"); return; }
	it->second->volumetric_cloud_height_min = min_height;
	it->second->volumetric_cloud_height_max = max_height;
}

void HRL_SetVolumetricCloudScale(HRL_id scene, float scale)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudScale: invalid scene ID"); return; }
	if (!std::isfinite(scale) || scale <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudScale: scale must be > 0"); return; }
	it->second->volumetric_cloud_scale = scale;
}

void HRL_SetVolumetricCloudDetail(HRL_id scene, float detail)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDetail: invalid scene ID"); return; }
	if (!std::isfinite(detail)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudDetail: detail must be finite"); return; }
	it->second->volumetric_cloud_detail = glm::clamp(detail, 0.0f, 1.0f);
}

void HRL_SetVolumetricCloudWind(HRL_id scene, float wind_x, float wind_z, float speed)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudWind: invalid scene ID"); return; }
	if (!std::isfinite(wind_x) || !std::isfinite(wind_z) || !std::isfinite(speed) || speed < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudWind: invalid wind parameters"); return; }
	it->second->volumetric_cloud_wind = glm::vec2(wind_x, wind_z);
	it->second->volumetric_cloud_wind_speed = speed;
}

void HRL_SetVolumetricCloudColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudColor: invalid scene ID"); return; }
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudColor: color must contain finite values"); return; }
	it->second->volumetric_cloud_color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.0f), glm::vec3(1.0f));
}

void HRL_SetVolumetricCloudLightColor(HRL_id scene, float r, float g, float b)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightColor: invalid scene ID"); return; }
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightColor: color must contain finite values"); return; }
	it->second->volumetric_cloud_light_color = glm::clamp(glm::vec3(r, g, b), glm::vec3(0.0f), glm::vec3(8.0f));
}

void HRL_SetVolumetricCloudLightAbsorption(HRL_id scene, float absorption)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightAbsorption: invalid scene ID"); return; }
	if (!std::isfinite(absorption) || absorption < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightAbsorption: absorption must be finite and >= 0"); return; }
	it->second->volumetric_cloud_light_absorption = absorption;
}

void HRL_SetVolumetricCloudLightIntensity(HRL_id scene, float intensity)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightIntensity: invalid scene ID"); return; }
	if (!std::isfinite(intensity) || intensity < 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudLightIntensity: intensity must be finite and >= 0"); return; }
	it->second->volumetric_cloud_light_intensity = intensity;
}

void HRL_SetVolumetricCloudSteps(HRL_id scene, HRL_uint steps)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudSteps: invalid scene ID"); return; }
	it->second->volumetric_cloud_steps = std::max<HRL_uint>(8u, std::min<HRL_uint>(96u, steps));
}

void HRL_SetVolumetricCloudMaxDistance(HRL_id scene, float distance)
{
	auto it = ctx_.scenes.find(scene);
	if (it == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudMaxDistance: invalid scene ID"); return; }
	if (!std::isfinite(distance) || distance <= 0.0f) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetVolumetricCloudMaxDistance: distance must be > 0"); return; }
	it->second->volumetric_cloud_max_distance = distance;
}


HRL_id HRL_CreateDecal(HRL_id scene)
{
	auto sceneIt = ctx_.scenes.find(scene);
	if (sceneIt == ctx_.scenes.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_CreateDecal: invalid scene ID"); return HRL_INVALID_ID; }
	auto* decal = new (std::nothrow) HRL_Decal();
	if (!decal) { SetErrorCode(HRL_OUT_OF_MEMORY, HRL_SEVERITY_ERROR, "HRL_CreateDecal: failed to allocate decal"); return HRL_INVALID_ID; }
	const HRL_id id = GenerateHRL_ID();
	decal->id_ = id;
	decal->scene_ = scene;
	sceneIt->second->decals.emplace(id, decal);
	ctx_.decals.emplace(id, decal);
	return id;
}

void HRL_DeleteDecal(HRL_id decalId)
{
	auto it = ctx_.decals.find(decalId);
	if (it == ctx_.decals.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, "HRL_DeleteDecal: invalid ID"); return; }
	if (auto sceneIt = ctx_.scenes.find(it->second->scene_); sceneIt != ctx_.scenes.end())
		sceneIt->second->decals.erase(decalId);
	delete it->second;
	ctx_.decals.erase(it);
}

static HRL_Decal* GetDecalForEdit(HRL_id id, const char* fn)
{
	auto it = ctx_.decals.find(id);
	if (it == ctx_.decals.end()) { SetErrorCode(HRL_ERROR_INVALID_ID, HRL_SEVERITY_ERROR, std::string(fn) + ": invalid decal ID"); return nullptr; }
	return it->second;
}

void HRL_SetDecalEnabled(HRL_id id, int enable)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalEnabled")) d->enabled = (enable != 0);
}
void HRL_SetDecalPosition(HRL_id id, float x, float y, float z)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalPosition")) { if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalPosition: invalid position"); return; } d->position = glm::vec3(x,y,z); }
}
void HRL_SetDecalRotation(HRL_id id, float pitch, float yaw, float roll)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalRotation")) { if (!std::isfinite(pitch)||!std::isfinite(yaw)||!std::isfinite(roll)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalRotation: invalid rotation"); return; } d->rotation = glm::vec3(pitch,yaw,roll); }
}
void HRL_SetDecalSize(HRL_id id, float x, float y, float z)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalSize")) { if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||x<=0||y<=0||z<=0) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalSize: size must be > 0"); return; } d->size=glm::vec3(x,y,z); }
}
void HRL_SetDecalTexture(HRL_id id, HRL_id texture)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalTexture")) d->texture = texture;
}
void HRL_SetDecalColor(HRL_id id, float r, float g, float b)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalColor")) { if (!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalColor: invalid color"); return; } d->color=glm::clamp(glm::vec3(r,g,b), glm::vec3(0.0f), glm::vec3(1.0f)); }
}
void HRL_SetDecalOpacity(HRL_id id, float opacity)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalOpacity")) { if (!std::isfinite(opacity)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalOpacity: opacity must be finite"); return; } d->opacity=glm::clamp(opacity,0.0f,1.0f); }
}
void HRL_SetDecalNormalFade(HRL_id id, float min_dot, float max_dot)
{
	if (auto* d = GetDecalForEdit(id, "HRL_SetDecalNormalFade")) { if (!std::isfinite(min_dot)||!std::isfinite(max_dot)) { SetErrorCode(HRL_INVALID_VALUE, HRL_SEVERITY_ERROR, "HRL_SetDecalNormalFade: values must be finite"); return; } d->normal_fade_min=glm::clamp(min_dot,0.0f,1.0f); d->normal_fade_max=glm::clamp(max_dot,0.0f,1.0f); if (d->normal_fade_max <= d->normal_fade_min) d->normal_fade_max=std::min(1.0f,d->normal_fade_min+0.01f); }
}

int HRL_IsValidDecal(HRL_id id)
{ return ctx_.decals.find(id) != ctx_.decals.end() ? 1 : 0; }
