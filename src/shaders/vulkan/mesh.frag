#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Lit surface of meshes, landscapes (HRL_LANDSCAPE) and foliage (HRL_FOLIAGE).
#include "common.glsl"
#include "brdf.glsl"
// Every texture twice: index 2*i is the UNORM view, 2*i+1 the sRGB view.
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"
#include "lighting.glsl"
#ifdef HRL_LANDSCAPE
#include "landscape.glsl"
#endif
#ifdef HRL_CUSTOM_SURFACE
#include "custom.glsl"
#include "hrl_user.glsl"
#endif

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec3 vTangent;   // landscape: xy = height texture coordinate
layout(location = 4) in vec3 vBitangent;
layout(location = 5) flat in uvec2 vIds; // material, object id
#if !defined(HRL_FOLIAGE) && !defined(HRL_LANDSCAPE)
layout(location = 8) flat in uint vTint;  // mesh particles (mesh.vert)
#endif
#ifdef HRL_FOLIAGE
layout(location = 6) flat in uint vDither; // bits 0-6 coverage, bit 7 complementary pattern
layout(location = 7) in vec3 vRestPos;     // position without the wind
#endif

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;

#ifdef HRL_SPRITE
// Sprites are pixel art: nearest texel, whatever the filter of the texture.
vec4 SampleNearest(uint slot, vec2 uv)
{
	ivec2 size = textureSize(uTextures[nonuniformEXT(slot)], 0);
	ivec2 texel = clamp(ivec2(floor(fract(uv) * vec2(size))), ivec2(0), size - 1);
	return texelFetch(uTextures[nonuniformEXT(slot)], texel, 0);
}
vec4 SampleLinear(uint tex, vec2 uv) { return SampleNearest(tex * 2u, uv); }
vec4 SampleSRGB(uint tex, vec2 uv) { return SampleNearest(tex * 2u + 1u, uv); }
#else
vec4 SampleLinear(uint tex, vec2 uv) { return texture(uTextures[nonuniformEXT(tex * 2u)], uv); }
vec4 SampleSRGB(uint tex, vec2 uv) { return texture(uTextures[nonuniformEXT(tex * 2u + 1u)], uv); }
#endif

void main()
{
#ifdef HRL_FOLIAGE
	// Dithered fade (distance, LOD cross-fade): two LODs of one instance keep
	// complementary pixels, so the transition shows no hole and no double.
	if (vDither != 127u)
	{
		float coverage = float(vDither & 127u) / 127.0;
		float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		bool keep = (vDither & 128u) != 0u ? noise >= 1.0 - coverage : noise < coverage;
		if (!keep) discard;
	}
#endif
	Material mat = materials[vIds.x];
	uint flags = mat.flags.x;

	vec4 albedoSample = SampleSRGB(mat.tex0.x, vUV);
#ifdef HRL_SPRITE
	// Opacity of the sprite texels (alpha of the albedo and alpha map), as the
	// OpenGL sprite shader. Transparent texels are not drawn and not pickable.
	float alpha = albedoSample.a * SampleLinear(mat.tex1.y, vUV).r * mat.baseColor.a;
	if (alpha < 0.004) discard;
#else
	float alpha = MaterialAlpha(mat, vUV, albedoSample.a);
#endif
#ifdef HRL_ALPHA_TEST
	if (alpha < 0.5) discard;
#endif

	ShadedSurface s;
	s.position = vWorldPos;
	s.baseColor = albedoSample.rgb * mat.baseColor.rgb;
#if !defined(HRL_FOLIAGE) && !defined(HRL_LANDSCAPE)
	s.baseColor *= unpackUnorm4x8(vTint).rgb;
#endif
	float roughness = SampleLinear(mat.tex0.z, vUV).r;
	if ((flags & MAT_ROUGHNESS_INVERT) != 0u) roughness = 1.0 - roughness;
	if ((flags & MAT_ROUGHNESS_VALUE) != 0u) roughness = mat.params.x;
	s.roughness = roughness;
	float metallic = (flags & MAT_METALLIC_VALUE) != 0u ? mat.params.y : SampleLinear(mat.tex0.w, vUV).r;
	s.metallic = clamp(metallic, 0.0, 1.0);
	s.specular = (flags & MAT_SPECULAR_VALUE) != 0u ? mat.params.z : SampleLinear(mat.tex1.z, vUV).r;
	s.ao = SampleLinear(mat.tex1.x, vUV).r;
	s.emissive = mat.emissive.rgb * SampleSRGB(mat.tex1.w, vUV).rgb;
	s.transmission = 0.0;

	// Geometric normal and tangent frame.
#ifdef HRL_LANDSCAPE
	// Per pixel normal of the height field (central differences), whatever the LOD.
	vec2 texel = uLand.samples.zw;
	vec2 hc = vTangent.xy;
	uint slot = uLand.ids.z * 2u;
	float hl = textureLod(uTextures[nonuniformEXT(slot)], hc - vec2(texel.x, 0.0), 0.0).r;
	float hr = textureLod(uTextures[nonuniformEXT(slot)], hc + vec2(texel.x, 0.0), 0.0).r;
	float hd = textureLod(uTextures[nonuniformEXT(slot)], hc - vec2(0.0, texel.y), 0.0).r;
	float hu = textureLod(uTextures[nonuniformEXT(slot)], hc + vec2(0.0, texel.y), 0.0).r;
	vec3 localN = normalize(vec3(-(hr - hl) * uLand.spacing.z / (2.0 * uLand.spacing.x), 1.0,
		-(hu - hd) * uLand.spacing.z / (2.0 * uLand.spacing.y)));
	vec3 N = normalize(mat3(uLand.normalMatrix) * localN);
	vec3 tangent = normalize(mat3(uLand.model) * vec3(1.0, 0.0, 0.0));
	vec3 bitangent = cross(N, tangent);
	// Shadow rays start on the full resolution surface (the one in the ray
	// tracing structure), not on the coarser LOD that was rasterized.
	vec2 sampleCoord = hc * uLand.samples.xy - 0.5;
	s.shadowPosition = (uLand.model * vec4(LandscapeLocal(sampleCoord, LandscapeTriangleHeight(sampleCoord)), 1.0)).xyz;
#else
	vec3 N = normalize(vNormal);
	if (!gl_FrontFacing) N = -N;
	vec3 tangent = vTangent, bitangent = vBitangent;
	s.shadowPosition = vWorldPos;
#endif
#ifdef HRL_FOLIAGE
	// Shadow rays start where the plant is in the ray tracing structure (no wind).
	s.shadowPosition = vRestPos;
#ifdef HRL_ALPHA_TEST
	// Leaves: some light passes through.
	s.transmission = 0.4;
#endif
#endif

	// Normal map. A degenerate tangent keeps the geometric normal.
	vec3 nt = SampleLinear(mat.tex0.y, vUV).xyz * 2.0 - 1.0;
	vec3 T = tangent - N * dot(N, tangent);
	if (dot(T, T) > 1e-8)
	{
		T = normalize(T);
		vec3 B = cross(N, T);
		if (dot(B, bitangent) < 0.0) B = -B;
		N = normalize(T * nt.x + B * nt.y + N * nt.z);
	}
	s.normal = N;

#ifdef HRL_CUSTOM_SURFACE
	// ShadedSurface function of the application, given the built-in surface.
	hrlParamBase = mat.flags.y;
	HRL_SurfaceInput si;
	si.worldPosition = vWorldPos;
	si.normal = normalize(gl_FrontFacing ? vNormal : -vNormal);
	si.uv = vUV;
	si.viewDirection = normalize(uView.camPos.xyz - vWorldPos);
	si.screenUV = fract(gl_FragCoord.xy * uView.viewportSize.zw);
	si.time = uView.camPos.w;
	si.objectId = vIds.y;
	HRL_Surface so;
	so.baseColor = s.baseColor;
	so.normal = s.normal;
	so.roughness = s.roughness;
	so.metallic = s.metallic;
	so.specular = s.specular;
	so.ao = s.ao;
	so.emissive = s.emissive;
	so.opacity = alpha;
	Surface(si, so);
	s.baseColor = so.baseColor;
	s.normal = normalize(so.normal);
	s.roughness = so.roughness;
	s.metallic = clamp(so.metallic, 0.0, 1.0);
	s.specular = so.specular;
	s.ao = so.ao;
	s.emissive = so.emissive;
	alpha = so.opacity;
#ifdef HRL_ALPHA_TEST
	if (alpha < 0.5) discard;
#endif
#endif

	outColor = vec4(ShadeSurface(s), alpha);
	outObjectId = vIds.y;
}
