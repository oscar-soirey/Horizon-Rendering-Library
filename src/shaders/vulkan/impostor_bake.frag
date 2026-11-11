#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Albedo (premultiplied by coverage) and local normal + roughness of a part.
#include "common.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"
layout(push_constant) uniform Push { vec4 right, up, dir, center; uint material; } pc;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec3 vBitangent;
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

void main()
{
	Material mat = materials[pc.material];
	vec4 albedo = texture(uTextures[nonuniformEXT(mat.tex0.x * 2u + 1u)], vUV);
#ifdef HRL_ALPHA_TEST
	if (MaterialAlpha(mat, vUV, albedo.a) < 0.5) discard;
#endif
	vec3 N = normalize(vNormal);
	if (!gl_FrontFacing) N = -N;
	vec3 nt = texture(uTextures[nonuniformEXT(mat.tex0.y * 2u)], vUV).xyz * 2.0 - 1.0;
	vec3 T = vTangent - N * dot(N, vTangent);
	if (dot(T, T) > 1e-8)
	{
		T = normalize(T);
		vec3 B = cross(N, T);
		if (dot(B, vBitangent) < 0.0) B = -B;
		N = normalize(T * nt.x + B * nt.y + N * nt.z);
	}
	float roughness = (mat.flags.x & MAT_ROUGHNESS_VALUE) != 0u ? mat.params.x
		: texture(uTextures[nonuniformEXT(mat.tex0.z * 2u)], vUV).r;
	outAlbedo = vec4(albedo.rgb * mat.baseColor.rgb, 1.0);
	outNormal = vec4(N * 0.5 + 0.5, clamp(roughness, 0.0, 1.0));
}
