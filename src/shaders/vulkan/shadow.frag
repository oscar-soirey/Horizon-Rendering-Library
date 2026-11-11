#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// Shadow caster with alpha test: texels below the cut-off do not cast a shadow.
#include "common.glsl"

layout(set = 1, binding = 0) uniform sampler2D uTextures[];
#include "material_alpha.glsl"
#ifdef HRL_CUSTOM_SURFACE
#include "custom.glsl"
#include "hrl_user.glsl"
#endif

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vMaterial;
#ifdef HRL_CUSTOM_SURFACE
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec3 vNormal;
layout(location = 4) flat in uint vObject;
#endif

void main()
{
	Material mat = materials[vMaterial];
	vec4 albedo = texture(uTextures[nonuniformEXT(mat.tex0.x * 2u + 1u)], vUV);
	float alpha = MaterialAlpha(mat, vUV, albedo.a);
#ifdef HRL_CUSTOM_SURFACE
	// Opacity given by the surface function (the other outputs are unused here).
	hrlParamBase = mat.flags.y;
	HRL_SurfaceInput si;
	si.worldPosition = vWorldPos;
	si.normal = normalize(vNormal);
	si.uv = vUV;
	si.viewDirection = si.normal;
	si.screenUV = vec2(0.0);
	si.time = uView.camPos.w;
	si.objectId = vObject;
	HRL_Surface so;
	so.baseColor = albedo.rgb * mat.baseColor.rgb;
	so.normal = si.normal;
	so.roughness = mat.params.x;
	so.metallic = mat.params.y;
	so.specular = mat.params.z;
	so.ao = 1.0;
	so.emissive = mat.emissive.rgb;
	so.opacity = alpha;
	Surface(si, so);
	alpha = so.opacity;
#endif
	if (alpha < 0.5) discard;
}
