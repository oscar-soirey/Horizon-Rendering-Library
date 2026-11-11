#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Impostor: the four atlas views closest to the viewing direction, each read
// where the view ray crosses its plane, blended. HRL_SHADOW : alpha test only.
#include "common.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#ifndef HRL_SHADOW
#include "brdf.glsl"
#include "material_alpha.glsl"
#include "lighting.glsl"
#endif
#include "foliage.glsl"
#include "impostor.glsl"
layout(set = 2, binding = 0, std140) uniform FoliageData { FOLIAGE_UNIFORM_BLOCK } uFol;
layout(push_constant) uniform Push { mat4 lightViewProj; uint part; uint pad0, pad1, pad2; vec4 lightDir; } pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) flat in vec4 vRotation;
layout(location = 2) flat in vec4 vCenterScale;
layout(location = 3) flat in uint vDither;
#ifndef HRL_SHADOW
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;
#endif

vec4 QuatInverse(vec4 q) { return vec4(-q.xyz, q.w); }

void main()
{
#ifndef HRL_SHADOW
	if (vDither != 127u)
	{
		float coverage = float(vDither & 127u) / 127.0;
		float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		bool keep = (vDither & 128u) != 0u ? noise >= 1.0 - coverage : noise < coverage;
		if (!keep) discard;
	}
#endif
	// The view ray in the local space of the plant (centre at the origin, unit = half size).
	vec4 invQ = QuatInverse(vRotation);
	float unit = 1.0 / (uFol.impostor.w * vCenterScale.w);
	vec3 p = QuatRotate(invQ, vWorldPos - vCenterScale.xyz) * unit;
#ifdef HRL_SHADOW
	vec3 rayDir = QuatRotate(invQ, normalize(pc.lightDir.xyz));
	vec3 toViewer = -rayDir;
#else
	vec3 eye = QuatRotate(invQ, uView.camPos.xyz - vCenterScale.xyz) * unit;
	vec3 rayDir = normalize(p - eye);
	vec3 toViewer = normalize(eye);
#endif
	float frames = uFol.heights.w;
	vec2 g = HemiOctEncode(toViewer) * frames - 0.5;
	ivec2 g0 = ivec2(floor(g));
	vec2 f = g - vec2(g0);
	vec4 albedo = vec4(0.0), normal = vec4(0.0);
	float weightSum = 0.0;
	for (int k = 0; k < 4; ++k)
	{
		ivec2 frame = clamp(g0 + ivec2(k & 1, k >> 1), ivec2(0), ivec2(int(frames) - 1));
		float w = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
		if (w <= 0.001) continue;
		vec3 d = ImpostorFrameDirection(frame, frames);
		vec3 right, up;
		ImpostorBasis(d, right, up);
		// Where the ray crosses the plane of this view (through the centre).
		float denom = dot(rayDir, d);
		vec3 hit = abs(denom) > 1e-4 ? p - rayDir * (dot(p, d) / denom) : p;
		vec2 uv = vec2(0.5 + 0.5 * dot(hit, right), 0.5 - 0.5 * dot(hit, up));
		if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
		vec2 atlas = (vec2(frame) + uv) / frames;
		albedo += w * texture(uTextures[nonuniformEXT(uFol.ids.y * 2u + 1u)], atlas);
		normal += w * texture(uTextures[nonuniformEXT(uFol.ids.z * 2u)], atlas);
		weightSum += w;
	}
	if (weightSum <= 0.0) discard;
	float alpha = albedo.a / weightSum;
	if (alpha < 0.4) discard;
#ifndef HRL_SHADOW
	ShadedSurface s;
	s.position = vWorldPos;
	s.shadowPosition = vWorldPos;
	s.baseColor = albedo.rgb / max(albedo.a, 1e-4);
	vec3 n = normal.xyz / max(albedo.a, 1e-4) * 2.0 - 1.0;
	s.normal = normalize(QuatRotate(vRotation, n));
	s.roughness = normal.w / max(albedo.a, 1e-4);
	s.metallic = 0.0;
	s.specular = 0.5;
	s.ao = 1.0;
	s.emissive = vec3(0.0);
	s.transmission = 0.3;
	outColor = vec4(ShadeSurface(s), 1.0);
	outObjectId = uFol.ids.x;
#endif
}
