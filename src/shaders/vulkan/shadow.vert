#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPosition;
#ifdef HRL_ALPHA_TEST
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vMaterial;
#endif
layout(push_constant) uniform Push { mat4 lightViewProj; } pc;

void main()
{
#ifdef HRL_ALPHA_TEST
	vUV = inUV;
	vMaterial = instances[gl_InstanceIndex].ids.x;
#endif
	gl_Position = pc.lightViewProj * (instances[gl_InstanceIndex].model * vec4(inPosition, 1.0));
}
