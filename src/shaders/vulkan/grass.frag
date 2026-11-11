#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "common.glsl"
#include "brdf.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"
#include "lighting.glsl"
#include "grass.glsl"
layout(set = 2, binding = 0, std140) uniform GrassData { GRASS_UNIFORM_BLOCK } uGrass;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 3) in vec3 vRestPos;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outObjectId;

void main()
{
	Surface s;
	s.position = vWorldPos;
	s.shadowPosition = vRestPos;
	// The normal leans towards the ground normal: both sides of a blade are lit from above.
	s.normal = normalize(vNormal);
	s.baseColor = vColor;
	s.roughness = 0.65;
	s.metallic = 0.0;
	s.specular = 0.4;
	s.ao = 1.0;
	s.emissive = vec3(0.0);
	s.transmission = 0.5;
	outColor = vec4(ShadeSurface(s), 1.0);
	outObjectId = uGrass.ids.z; // picking goes to the landscape
}
