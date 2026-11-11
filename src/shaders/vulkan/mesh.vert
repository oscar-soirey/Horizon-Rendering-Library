#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#ifdef HRL_CUSTOM_VERTEX
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "custom.glsl"
#include "hrl_user.glsl"
#endif

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
layout(location = 5) flat out uvec2 vIds; // material, object id
layout(location = 8) flat out uint vTint;  // RGBA8 multiplied with the base colour (mesh particles)

void main()
{
	Instance inst = instances[gl_InstanceIndex];
	vec4 world = inst.model * vec4(inPosition, 1.0);
	mat3 normalMatrix = mat3(inst.normalMatrix);
	vWorldPos = world.xyz;
	vNormal = normalMatrix * inNormal;
	vTangent = mat3(inst.model) * inTangent;
	vBitangent = mat3(inst.model) * inBitangent;
	vUV = (inst.ids.z & INSTANCE_SPRITE) != 0u ? mix(inst.normalMatrix[3].xy, inst.normalMatrix[3].zw, inUV) : inUV;
	vIds = inst.ids.xy;
	vTint = (inst.ids.z & INSTANCE_TINT) != 0u ? inst.ids.w : 0xFFFFFFFFu;
#ifdef HRL_CUSTOM_VERTEX
	hrlParamBase = materials[inst.ids.x].flags.y;
	HRL_VertexData v;
	v.worldPosition = world.xyz;
	v.normal = vNormal;
	v.uv = vUV;
	v.time = uView.camPos.w;
	v.objectId = inst.ids.y;
	Vertex(v);
	world = vec4(v.worldPosition, 1.0);
	vWorldPos = world.xyz;
	vNormal = v.normal;
	vUV = v.uv;
#endif
	gl_Position = uView.viewProj * world;
}
