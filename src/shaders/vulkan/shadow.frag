#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Shadow caster with alpha test: texels below the cut-off do not cast a shadow.
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vMaterial;

void main()
{
	Material mat = materials[vMaterial];
	float albedoAlpha = texture(uTextures[nonuniformEXT(mat.tex0.x * 2u + 1u)], vUV).a;
	if (MaterialAlpha(mat, vUV, albedoAlpha) < 0.5) discard;
}
