// HRL Vulkan renderer - lighting shared by the lit fragment shaders (meshes,
// landscapes, foliage, impostors, grass). Needs common.glsl, brdf.glsl, the
// bindless array `uTextures` and material_alpha.glsl.
#ifndef HRL_LIGHTING_GLSL
#define HRL_LIGHTING_GLSL
#ifdef HRL_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

#include "media.glsl"

layout(set = 0, binding = 4) uniform sampler2DArrayShadow uShadowMaps;
layout(set = 0, binding = 5) uniform samplerCube uIrradiance;
layout(set = 0, binding = 6) uniform samplerCube uPrefiltered;
layout(set = 0, binding = 7) uniform sampler2D uBrdfLut;
// Point light shadow maps without ray tracing: 6 faces per light.
layout(set = 0, binding = 12) uniform sampler2DArrayShadow uPointShadowMaps;
// Light of the emissive voxels of a 2D voxel world (vk_voxel.cpp).
layout(set = 0, binding = 15) uniform sampler2D uVoxelLight;
vec3 SampleVoxelLight(vec3 position)
{
	vec2 uv = (position.xy - uView.voxelLightRegion.xy) * uView.voxelLightRegion.zw;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
	float z = position.z / max(uView.voxelLight.x, 1e-4);
	return max(textureLod(uVoxelLight, uv, 0.0).rgb, vec3(0.0)) / (1.0 + z * z);
}
// Global illumination probes: 6 directional lobes per probe (+X -X +Y -Y +Z -Z), w = valid.
layout(set = 0, binding = 14, std430) readonly buffer GIProbes { vec4 giLobes[]; };

// Indirect diffuse light of the probe volume at a point (same rule as the CPU
// reference, core/gi_probes.cpp: trilinear over the valid probes, lobes
// weighted by the normal).
vec3 SampleProbeGI(vec3 position, vec3 normal)
{
	ivec3 res = ivec3(uView.giResolution.xyz);
	vec3 grid = (position - uView.giOrigin.xyz) / max(uView.giSpacing.xyz, vec3(1e-4));
	if (any(lessThan(grid, vec3(0.0))) || any(greaterThan(grid, vec3(res - 1)))) return vec3(0.0);
	ivec3 base = clamp(ivec3(floor(grid)), ivec3(0), res - 1);
	vec3 f = grid - vec3(base);
	vec3 lobeWeights[2] = vec3[](max(normal, vec3(0.0)), max(-normal, vec3(0.0)));
	vec3 result = vec3(0.0);
	float total = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		ivec3 o = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		ivec3 p = min(base + o, res - 1);
		int index = (p.z * res.y + p.y) * res.x + p.x;
		if (giLobes[index * 6].w < 0.5) continue;
		vec3 w3 = mix(1.0 - f, f, vec3(o));
		float spatial = w3.x * w3.y * w3.z;
		if (spatial <= 0.0) continue;
		vec3 local = giLobes[index * 6 + 0].rgb * lobeWeights[0].x + giLobes[index * 6 + 1].rgb * lobeWeights[1].x +
			giLobes[index * 6 + 2].rgb * lobeWeights[0].y + giLobes[index * 6 + 3].rgb * lobeWeights[1].y +
			giLobes[index * 6 + 4].rgb * lobeWeights[0].z + giLobes[index * 6 + 5].rgb * lobeWeights[1].z;
		float directional = dot(lobeWeights[0] + lobeWeights[1], vec3(1.0));
		if (directional <= 1e-5) continue;
		result += local / directional * spatial;
		total += spatial;
	}
	return total > 1e-5 ? result / total : vec3(0.0);
}
#ifdef HRL_RAY_QUERY
layout(set = 0, binding = 8) uniform accelerationStructureEXT uTlas;
// Alpha tested shadow casters: their triangles are read to test the texture.
layout(set = 0, binding = 9, std430) readonly buffer RtVertices { float rtVertices[]; };   // HRL_Vertex3D, 14 floats
layout(set = 0, binding = 10, std430) readonly buffer RtIndices { uint rtIndices[]; };
struct RtGeometry { uint firstIndex; int vertexOffset; uint material; uint pad; };
layout(set = 0, binding = 11, std430) readonly buffer RtGeometries { RtGeometry rtGeometries[]; };
#endif
float SampleShadowLayer(vec4 clip, float layer, float bias)
{
	vec3 p = clip.xyz / clip.w;
	vec2 uv = p.xy * 0.5 + 0.5;
	if (p.z <= 0.0 || p.z >= 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return 1.0;
	float texel = uView.shadowInfo.x;
	float sum = 0.0;
	// 3x3 PCF, each tap is a hardware 2x2 bilinear comparison.
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			sum += texture(uShadowMaps, vec4(uv + vec2(x, y) * texel, layer, p.z - bias));
	return sum / 9.0;
}

// Cube face of a point light: the one of the major axis of the direction from the light.
float PointShadow(int index, vec3 lightPos, vec3 worldPos, vec3 N, float bias)
{
	vec3 d = worldPos - lightPos;
	vec3 a = abs(d);
	int face = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
	// The normal offset follows the size of a texel at that distance.
	float texelWorld = 2.0 * max(a.x, max(a.y, a.z)) * uView.shadowInfo.z;
	vec3 offsetPos = worldPos + N * (0.01 + 1.5 * texelWorld);
	vec4 clip = uView.pointViewProj[index * 6 + face] * vec4(offsetPos, 1.0);
	vec3 p = clip.xyz / clip.w;
	vec2 uv = clamp(p.xy * 0.5 + 0.5, vec2(0.0), vec2(1.0));
	if (p.z <= 0.0 || p.z >= 1.0) return 1.0;
	float layer = float(index * 6 + face);
	float texel = uView.shadowInfo.z;
	float sum = 0.0;
	for (int y = -1; y <= 1; ++y)
		for (int x = -1; x <= 1; ++x)
			sum += texture(uPointShadowMaps, vec4(uv + vec2(x, y) * texel, layer, p.z - bias * 0.01));
	return sum / 9.0;
}

float CascadeShadow(vec3 worldPos, vec3 N, vec3 L, float bias)
{
	float viewDepth = -(uView.view * vec4(worldPos, 1.0)).z;
	int cascade = HRL_MAX_CASCADES - 1;
	for (int i = 0; i < HRL_MAX_CASCADES; ++i)
		if (viewDepth < uView.cascadeSplits[i]) { cascade = i; break; }
	if (viewDepth >= uView.cascadeSplits[HRL_MAX_CASCADES - 1]) return 1.0;

	// Normal offset grows with the cascade texel footprint and the grazing angle.
	float slope = 1.0 - clamp(dot(N, L), 0.0, 1.0);
	float cascadeScale = uView.cascadeSplits[cascade] / uView.cascadeSplits[0];
	vec3 offsetPos = worldPos + N * (0.02 + 0.06 * slope) * cascadeScale;
	float shadow = SampleShadowLayer(uView.cascadeViewProj[cascade] * vec4(offsetPos, 1.0), float(cascade), bias);

	// Blend into the next cascade near the split to hide the resolution change.
	float range = uView.shadowInfo.y * uView.cascadeSplits[cascade];
	float t = (viewDepth - (uView.cascadeSplits[cascade] - range)) / max(range, 1e-4);
	if (t > 0.0 && cascade < HRL_MAX_CASCADES - 1)
	{
		float next = SampleShadowLayer(uView.cascadeViewProj[cascade + 1] * vec4(offsetPos, 1.0), float(cascade + 1), bias);
		shadow = mix(shadow, next, clamp(t, 0.0, 1.0));
	}
	return shadow;
}

#ifdef HRL_RAY_QUERY
vec2 RtVertexUV(uint vertex)
{
	return vec2(rtVertices[vertex * 14u + 6u], rtVertices[vertex * 14u + 7u]);
}

float TraceShadow(vec3 origin, vec3 dir, float tMax)
{
#ifndef HRL_COMPUTE_STAGE
	// Helper invocations (quad padding) produce no pixel: no ray for them.
	if (gl_HelperInvocation) return 1.0;
#endif
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.0, dir, tMax);
	// Opaque geometry is committed by the hardware; alpha tested instances
	// (non opaque) come here to test their texture at the hit point.
	while (rayQueryProceedEXT(rq))
	{
		if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
		RtGeometry g = rtGeometries[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)];
		// Opaque geometry (entry 0) should not come here; accept it if it does.
		if (g.material == 0xFFFFFFFFu) { rayQueryConfirmIntersectionEXT(rq); continue; }
		uint base = g.firstIndex + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)) * 3u;
		vec2 b = rayQueryGetIntersectionBarycentricsEXT(rq, false);
		vec2 uv = RtVertexUV(uint(int(rtIndices[base]) + g.vertexOffset)) * (1.0 - b.x - b.y) +
			RtVertexUV(uint(int(rtIndices[base + 1u]) + g.vertexOffset)) * b.x +
			RtVertexUV(uint(int(rtIndices[base + 2u]) + g.vertexOffset)) * b.y;
		Material mat = materials[g.material];
		float albedoAlpha = textureLod(uTextures[nonuniformEXT(mat.tex0.x * 2u + 1u)], uv, 0.0).a;
		if (MaterialAlphaLod(mat, uv, albedoAlpha) >= 0.5) rayQueryConfirmIntersectionEXT(rq);
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
#endif

float LightShadow(Light light, vec3 worldPos, vec3 N, vec3 L, float lightDistance)
{
	float slot = light.color.a;
	if (slot < -0.5) return 1.0;
	float shadow = 1.0;
#ifdef HRL_RAY_QUERY
	if ((uView.counts.y & VIEW_FLAG_RT_SHADOWS) != 0u)
	{
		vec3 origin = worldPos + N * (0.002 + 0.002 * length(worldPos - uView.camPos.xyz));
		shadow = TraceShadow(origin, L, lightDistance);
	}
	else
#endif
	if (slot < 0.5)
		shadow = CascadeShadow(worldPos, N, L, light.spot.w);
	else if (slot < float(HRL_MAX_SPOT_SHADOWS) + 0.5)
	{
		int s = int(slot) - 1;
		vec3 offsetPos = worldPos + N * 0.02;
		shadow = SampleShadowLayer(uView.spotViewProj[s] * vec4(offsetPos, 1.0), float(HRL_MAX_CASCADES + s), light.spot.w);
	}
	else if (slot > 9.5)
		shadow = PointShadow(int(slot + 0.5) - 10, light.position.xyz, worldPos, N, light.spot.w);
	return mix(1.0, shadow, clamp(light.spot.z, 0.0, 1.0));
}

struct ShadedSurface
{
	vec3 position;      // shaded point
	vec3 shadowPosition; // origin of the shadow lookups / rays
	vec3 normal;
	vec3 baseColor;
	float roughness, metallic, specular, ao;
	vec3 emissive;
	float transmission; // light passing through thin surfaces (leaves, grass blades), 0..1
};

// Direct lights, image based lighting, emission, debug views and fog.
vec3 ShadeSurface(ShadedSurface s)
{
	vec3 N = s.normal;
	vec3 V = normalize(uView.camPos.xyz - s.position);
	vec3 diffuseColor = s.baseColor * (1.0 - s.metallic);
	vec3 F0 = mix(vec3(0.04) * clamp(s.specular, 0.0, 1.0), s.baseColor, s.metallic);
	float roughness = clamp(s.roughness, 0.045, 1.0);

	vec3 color = vec3(0.0);
	vec3 ambient = vec3(0.0);
	bool firstDirectional = true;
	uint lightCount = uView.counts.x;
	for (uint i = 0u; i < lightCount; ++i)
	{
		Light light = lights[i];
		int type = int(light.position.w);
		if (type == LIGHT_SKY)
		{
			ambient += diffuseColor * light.color.rgb;
			continue;
		}
		vec3 L;
		float attenuation = 1.0;
		float dist = 1e4;
		if (type == LIGHT_DIRECTIONAL)
		{
			L = normalize(-light.direction.xyz);
			// Volumes of the participating media shadow the main directional light.
			if (firstDirectional) attenuation = HRL_MediaVisibility(s.position);
			firstDirectional = false;
		}
		else
		{
			vec3 toLight = light.position.xyz - s.position;
			dist = length(toLight);
			L = toLight / max(dist, 1e-4);
			attenuation = 1.0 / (1.0 + light.direction.w * dist * dist);
			if (type == LIGHT_SPOT)
				attenuation *= smoothstep(light.spot.y, light.spot.x, dot(L, normalize(-light.direction.xyz)));
		}
		float NdotL = dot(N, L);
		if (attenuation <= 1e-5 || (NdotL <= 0.0 && s.transmission <= 0.0)) continue;
		// Thin surfaces lit from behind are shadowed with the normal facing the light.
		vec3 shadowNormal = NdotL >= 0.0 ? N : -N;
		float shadow = LightShadow(light, s.shadowPosition, shadowNormal, L, dist);
		if (shadow <= 0.0) continue;
		vec3 radiance = light.color.rgb * attenuation * shadow;
		if (NdotL > 0.0) color += EvalBRDF(N, V, L, diffuseColor, F0, roughness) * radiance;
		else color += diffuseColor * s.transmission * (-NdotL) * (1.0 / 3.14159265) * radiance;
	}

	if ((uView.counts.y & VIEW_FLAG_IBL) != 0u)
	{
		float NdotV = max(dot(N, V), 1e-4);
		vec3 R = reflect(-V, N);
		vec3 irradiance = texture(uIrradiance, N).rgb;
		vec3 prefiltered = textureLod(uPrefiltered, R, roughness * uView.envParams.x).rgb;
		vec2 brdf = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
		// Multiple scattering compensation (Fdez-Aguera 2019).
		vec3 FssEss = F0 * brdf.x + brdf.y;
		float Ems = 1.0 - (brdf.x + brdf.y);
		vec3 Favg = F0 + (1.0 - F0) / 21.0;
		vec3 FmsEms = Ems * FssEss * Favg / (1.0 - Favg * Ems);
		vec3 kD = diffuseColor * (1.0 - FssEss - FmsEms);
		ambient += (FssEss * prefiltered + (FmsEms + kD) * irradiance) * uView.envParams.y;
	}
	if (uView.giResolution.w > 0.5)
		ambient += diffuseColor * SampleProbeGI(s.position, N) * uView.giOrigin.w;
	if (uView.voxelLight.y > 0.5)
		ambient += diffuseColor * SampleVoxelLight(s.position);
	color += ambient * s.ao + s.emissive;

	uint debugView = uView.counts.z;
	if (debugView == 1u) color = s.baseColor;
	else if (debugView == 2u) color = N * 0.5 + 0.5;

	if ((uView.counts.y & VIEW_FLAG_FOG) != 0u)
	{
		float d = length(uView.camPos.xyz - s.position);
		float f = (uView.counts.y & VIEW_FLAG_FOG_EXP) != 0u
			? 1.0 - exp(-uView.fogParams.z * d)
			: clamp((d - uView.fogParams.x) / max(uView.fogParams.y - uView.fogParams.x, 1e-4), 0.0, 1.0);
		color = mix(color, uView.fogColor.rgb, f);
	}
	// Participating media of the viewport (froxels written by render passes).
	if (HRL_MediaActive() && debugView == 0u) color = HRL_ApplyMedia(color, s.position);
	return color;
}

#endif
