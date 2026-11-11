#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Landscape patch (one instance per quadtree node). Shares mesh.frag
// (HRL_LANDSCAPE variant) and, without HRL_SHADOW, the varyings of mesh.vert.
#include "common.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "landscape.glsl"

layout(location = 0) in vec2 inGrid;
layout(location = 1) in vec4 inNode;

#ifdef HRL_SHADOW
layout(push_constant) uniform Push { mat4 lightViewProj; } pc;
#else
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vTangent;   // landscape: xy = height texture coordinate
layout(location = 4) out vec3 vBitangent;
layout(location = 5) flat out uvec2 vIds;
#endif

void main()
{
	vec3 world;
	vec2 s = LandscapeVertex(inGrid, inNode, world);
#ifdef HRL_SHADOW
	gl_Position = pc.lightViewProj * vec4(world, 1.0);
#else
	vWorldPos = world;
	vNormal = normalize(mat3(uLand.normalMatrix) * vec3(0.0, 1.0, 0.0));
	vec2 t = s / (uLand.samples.xy - 1.0);
	vUV = t * uLand.uvScale.xy;
	vTangent = vec3((s + 0.5) * uLand.samples.zw, 0.0);
	vBitangent = vec3(0.0);
	vIds = uLand.ids.xy;
	gl_Position = uView.viewProj * vec4(world, 1.0);
#endif
}
