#version 460
#extension GL_GOOGLE_include_directive : require
// One view of a foliage part for its impostor atlas (orthographic, local space).
#include "common.glsl"
layout(push_constant) uniform Push
{
	vec4 right;   // xyz : view right, w : 1 / half size
	vec4 up;      // xyz : view up
	vec4 dir;     // xyz : towards the viewer
	vec4 center;  // xyz : centre of the views
	uint material;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec3 vBitangent;

void main()
{
	vec3 p = inPosition - pc.center.xyz;
	float s = pc.right.w;
	vNormal = inNormal;
	vUV = inUV;
	vTangent = inTangent;
	vBitangent = inBitangent;
	gl_Position = vec4(dot(p, pc.right.xyz) * s, -dot(p, pc.up.xyz) * s, 0.5 - 0.5 * dot(p, pc.dir.xyz) * s, 1.0);
}
