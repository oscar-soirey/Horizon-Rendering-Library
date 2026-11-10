#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inTangent;
layout(location = 4) in vec3 inBitangent;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vTangent;
layout(location = 4) out vec3 vBitangent;
layout(location = 5) flat out uint vInstance;

void main()
{
	Instance inst = instances[gl_InstanceIndex];
	vec4 world = inst.model * vec4(inPosition, 1.0);
	mat3 normalMatrix = mat3(inst.normalMatrix);
	vWorldPos = world.xyz;
	vNormal = normalMatrix * inNormal;
	vTangent = mat3(inst.model) * inTangent;
	vBitangent = mat3(inst.model) * inBitangent;
	vUV = inUV;
	vInstance = uint(gl_InstanceIndex);
	gl_Position = uView.viewProj * world;
}
