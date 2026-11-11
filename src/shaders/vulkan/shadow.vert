#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#if defined(HRL_CUSTOM_VERTEX) || defined(HRL_CUSTOM_SURFACE)
// Shadow casters with a custom material: the vertex function moves them, the
// surface function gives their opacity.
#define HRL_CUSTOM_SHADOW
#endif
#ifdef HRL_CUSTOM_VERTEX
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "custom.glsl"
#include "hrl_user.glsl"
#endif

layout(location = 0) in vec3 inPosition;
#if defined(HRL_ALPHA_TEST) || defined(HRL_CUSTOM_SHADOW)
layout(location = 2) in vec2 inUV;
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vMaterial;
#endif
#ifdef HRL_CUSTOM_SHADOW
layout(location = 1) in vec3 inNormal;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec3 vNormal;
layout(location = 4) flat out uint vObject;
#endif
layout(push_constant) uniform Push { mat4 lightViewProj; } pc;

void main()
{
	Instance inst = instances[gl_InstanceIndex];
	vec4 world = inst.model * vec4(inPosition, 1.0);
#if defined(HRL_ALPHA_TEST) || defined(HRL_CUSTOM_SHADOW)
	vUV = inUV;
	vMaterial = inst.ids.x;
#endif
#ifdef HRL_CUSTOM_SHADOW
	vNormal = mat3(inst.normalMatrix) * inNormal;
	vObject = inst.ids.y;
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
	vNormal = v.normal;
	vUV = v.uv;
#endif
	vWorldPos = world.xyz;
#endif
	gl_Position = pc.lightViewProj * world;
}
