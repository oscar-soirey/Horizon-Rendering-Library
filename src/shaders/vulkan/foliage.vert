#version 460
#extension GL_GOOGLE_include_directive : require
// Instance of a foliage part. HRL_SHADOW : depth only (HRL_ALPHA_TEST : with
// texture coordinates for the alpha test).
#include "common.glsl"
#include "foliage.glsl"

layout(set = 2, binding = 0, std140) uniform FoliageData { FOLIAGE_UNIFORM_BLOCK } uFol;
layout(set = 2, binding = 1, std430) readonly buffer FoliageInstances { FoliageInstance fInstances[]; };
layout(set = 2, binding = 2, std430) readonly buffer FoliageVisible { uint fVisible[]; };
layout(push_constant) uniform Push { mat4 lightViewProj; uint part; } pc;

layout(location = 0) in vec3 inPosition;
#if !defined(HRL_SHADOW) || defined(HRL_ALPHA_TEST)
layout(location = 2) in vec2 inUV;
#endif
#ifndef HRL_SHADOW
layout(location = 1) in vec3 inNormal;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vTangent;
layout(location = 4) out vec3 vBitangent;
layout(location = 5) flat out uvec2 vIds;
layout(location = 6) flat out uint vDither;
layout(location = 7) out vec3 vRestPos; // without wind: matches the ray tracing structure
#elif defined(HRL_ALPHA_TEST)
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vMaterial;
#endif

void main()
{
	uint packed = fVisible[gl_InstanceIndex];
	FoliageInstance inst = fInstances[packed & 0xFFFFFFu];
	float scale = inst.positionScale.w;
	vec3 root = inst.positionScale.xyz;
	vec3 world = root + QuatRotate(inst.rotation, inPosition * scale);
#ifndef HRL_SHADOW
	vRestPos = world;
#endif
	float h01 = clamp((inPosition.y - uFol.heights.x) / max(uFol.heights.y, 1e-4), 0.0, 1.0);
	world += FoliageMotion(root, world, h01, uFol.heights.y * scale, uFol.draw.w);
	uint material = uFol.materials[pc.part];
#ifdef HRL_SHADOW
#ifdef HRL_ALPHA_TEST
	vUV = inUV;
	vMaterial = material;
#endif
	gl_Position = pc.lightViewProj * vec4(world, 1.0);
#else
	vWorldPos = world;
	vNormal = QuatRotate(inst.rotation, inNormal);
	vTangent = QuatRotate(inst.rotation, inTangent);
	vBitangent = QuatRotate(inst.rotation, inBitangent);
	vUV = inUV;
	vIds = uvec2(material, uFol.ids.x);
	vDither = packed >> 24;
	gl_Position = uView.viewProj * vec4(world, 1.0);
#endif
}
