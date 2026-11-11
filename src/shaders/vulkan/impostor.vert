#version 460
#extension GL_GOOGLE_include_directive : require
// Camera (or light, HRL_SHADOW) facing quad of an impostor instance.
#include "common.glsl"
#include "foliage.glsl"

layout(set = 2, binding = 0, std140) uniform FoliageData { FOLIAGE_UNIFORM_BLOCK } uFol;
layout(set = 2, binding = 1, std430) readonly buffer FoliageInstances { FoliageInstance fInstances[]; };
layout(set = 2, binding = 2, std430) readonly buffer FoliageVisible { uint fVisible[]; };
layout(push_constant) uniform Push { mat4 lightViewProj; uint part; uint pad0, pad1, pad2; vec4 lightDir; } pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) flat out vec4 vRotation;
layout(location = 2) flat out vec4 vCenterScale; // impostor centre (world), instance scale
layout(location = 3) flat out uint vDither;

void main()
{
	uint packed = fVisible[gl_InstanceIndex];
	FoliageInstance inst = fInstances[packed & 0xFFFFFFu];
	float scale = inst.positionScale.w;
	vec3 center = inst.positionScale.xyz + QuatRotate(inst.rotation, uFol.impostor.xyz * scale);
	float halfSize = uFol.impostor.w * scale;
#ifdef HRL_SHADOW
	vec3 toViewer = -normalize(pc.lightDir.xyz);
#else
	vec3 toViewer = normalize(uView.camPos.xyz - center);
#endif
	vec3 right = abs(toViewer.y) > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), toViewer));
	vec3 up = cross(toViewer, right);
	const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
	vec2 c = corners[gl_VertexIndex];
	vec3 world = center + (right * c.x + up * c.y) * halfSize;
	vWorldPos = world;
	vRotation = inst.rotation;
	vCenterScale = vec4(center, scale);
	vDither = packed >> 24;
#ifdef HRL_SHADOW
	gl_Position = pc.lightViewProj * vec4(world, 1.0);
#else
	gl_Position = uView.viewProj * vec4(world, 1.0);
#endif
}
