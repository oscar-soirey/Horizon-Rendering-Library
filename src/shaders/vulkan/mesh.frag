#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef HRL_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"
#include "brdf.glsl"

layout(set = 0, binding = 4) uniform sampler2DArrayShadow uShadowMaps;
layout(set = 0, binding = 5) uniform samplerCube uIrradiance;
layout(set = 0, binding = 6) uniform samplerCube uPrefiltered;
layout(set = 0, binding = 7) uniform sampler2D uBrdfLut;
#ifdef HRL_RAY_QUERY
layout(set = 0, binding = 8) uniform accelerationStructureEXT uTlas;
#endif
// Every texture twice: index 2*i is the UNORM view, 2*i+1 the sRGB view.
layout(set = 1, binding = 0) uniform sampler2D uTextures[];

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vTangent;
layout(location = 4) in vec3 vBitangent;
layout(location = 5) flat in uint vInstance;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;

vec4 SampleLinear(uint tex, vec2 uv) { return texture(uTextures[nonuniformEXT(tex * 2u)], uv); }
vec4 SampleSRGB(uint tex, vec2 uv) { return texture(uTextures[nonuniformEXT(tex * 2u + 1u)], uv); }

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
float TraceShadow(vec3 origin, vec3 dir, float tMax)
{
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
		0xFF, origin, 0.0, dir, tMax);
	rayQueryProceedEXT(rq);
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
	return mix(1.0, shadow, clamp(light.spot.z, 0.0, 1.0));
}

void main()
{
	Instance inst = instances[vInstance];
	Material mat = materials[inst.ids.x];
	uint flags = mat.flags.x;

	vec4 albedoSample = SampleSRGB(mat.tex0.x, vUV);
	// Same opacity rule as the OpenGL backend: the opacity value replaces the alpha map.
	float alphaMap = SampleLinear(mat.tex1.y, vUV).r;
	if ((flags & MAT_ALPHA_INVERT) != 0u) alphaMap = 1.0 - alphaMap;
	float alpha = albedoSample.a * mat.baseColor.a * ((flags & MAT_OPACITY_VALUE) != 0u ? mat.params.w : alphaMap);
#ifdef HRL_ALPHA_TEST
	if (alpha < 0.5) discard;
#endif

	vec3 baseColor = albedoSample.rgb * mat.baseColor.rgb;
	float roughness = SampleLinear(mat.tex0.z, vUV).r;
	if ((flags & MAT_ROUGHNESS_INVERT) != 0u) roughness = 1.0 - roughness;
	if ((flags & MAT_ROUGHNESS_VALUE) != 0u) roughness = mat.params.x;
	roughness = clamp(roughness, 0.045, 1.0);
	float metallic = (flags & MAT_METALLIC_VALUE) != 0u ? mat.params.y : SampleLinear(mat.tex0.w, vUV).r;
	metallic = clamp(metallic, 0.0, 1.0);
	float specular = (flags & MAT_SPECULAR_VALUE) != 0u ? mat.params.z : SampleLinear(mat.tex1.z, vUV).r;
	float ao = SampleLinear(mat.tex1.x, vUV).r;
	vec3 emissive = mat.emissive.rgb * SampleSRGB(mat.tex1.w, vUV).rgb;

	// Tangent frame. A degenerate tangent falls back to the geometric normal.
	vec3 N = normalize(vNormal);
	if (!gl_FrontFacing) N = -N;
	vec3 nt = SampleLinear(mat.tex0.y, vUV).xyz * 2.0 - 1.0;
	vec3 T = vTangent - N * dot(N, vTangent);
	if (dot(T, T) > 1e-8)
	{
		T = normalize(T);
		vec3 B = cross(N, T);
		if (dot(B, vBitangent) < 0.0) B = -B;
		N = normalize(T * nt.x + B * nt.y + N * nt.z);
	}

	vec3 V = normalize(uView.camPos.xyz - vWorldPos);
	vec3 diffuseColor = baseColor * (1.0 - metallic);
	vec3 F0 = mix(vec3(0.04) * clamp(specular, 0.0, 1.0), baseColor, metallic);

	vec3 color = vec3(0.0);
	vec3 ambient = vec3(0.0);
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
			L = normalize(-light.direction.xyz);
		else
		{
			vec3 toLight = light.position.xyz - vWorldPos;
			dist = length(toLight);
			L = toLight / max(dist, 1e-4);
			attenuation = 1.0 / (1.0 + light.direction.w * dist * dist);
			if (type == LIGHT_SPOT)
				attenuation *= smoothstep(light.spot.y, light.spot.x, dot(L, normalize(-light.direction.xyz)));
		}
		if (attenuation <= 1e-5 || dot(N, L) <= 0.0) continue;
		float shadow = LightShadow(light, vWorldPos, N, L, dist);
		if (shadow <= 0.0) continue;
		color += EvalBRDF(N, V, L, diffuseColor, F0, roughness) * light.color.rgb * attenuation * shadow;
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
	color += ambient * ao + emissive;

	uint debugView = uView.counts.z;
	if (debugView == 1u) color = baseColor;
	else if (debugView == 2u) color = N * 0.5 + 0.5;

	if ((uView.counts.y & VIEW_FLAG_FOG) != 0u)
	{
		float d = length(uView.camPos.xyz - vWorldPos);
		float f = (uView.counts.y & VIEW_FLAG_FOG_EXP) != 0u
			? 1.0 - exp(-uView.fogParams.z * d)
			: clamp((d - uView.fogParams.x) / max(uView.fogParams.y - uView.fogParams.x, 1e-4), 0.0, 1.0);
		color = mix(color, uView.fogColor.rgb, f);
	}

	outColor = vec4(color, alpha);
	outObjectId = inst.ids.y;
}
