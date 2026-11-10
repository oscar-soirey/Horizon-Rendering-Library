#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(push_constant) uniform Push { mat4 lightViewProj; } pc;

void main()
{
	gl_Position = pc.lightViewProj * (instances[gl_InstanceIndex].model * vec4(inPosition, 1.0));
}
