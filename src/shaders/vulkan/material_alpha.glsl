// Opacity of a material at a texture coordinate. Needs common.glsl and the
// bindless texture array `uTextures` (2*i : UNORM view, 2*i+1 : sRGB view).
#ifndef HRL_MATERIAL_ALPHA_GLSL
#define HRL_MATERIAL_ALPHA_GLSL

// Same opacity rule as the OpenGL backend: the opacity value replaces the alpha map.
float MaterialAlpha(Material mat, vec2 uv, float albedoAlpha)
{
	float alphaMap = texture(uTextures[nonuniformEXT(mat.tex1.y * 2u)], uv).r;
	if ((mat.flags.x & MAT_ALPHA_INVERT) != 0u) alphaMap = 1.0 - alphaMap;
	return albedoAlpha * mat.baseColor.a * ((mat.flags.x & MAT_OPACITY_VALUE) != 0u ? mat.params.w : alphaMap);
}

// Same with the most detailed mip (no derivatives: shadow rays).
float MaterialAlphaLod(Material mat, vec2 uv, float albedoAlpha)
{
	float alphaMap = textureLod(uTextures[nonuniformEXT(mat.tex1.y * 2u)], uv, 0.0).r;
	if ((mat.flags.x & MAT_ALPHA_INVERT) != 0u) alphaMap = 1.0 - alphaMap;
	return albedoAlpha * mat.baseColor.a * ((mat.flags.x & MAT_OPACITY_VALUE) != 0u ? mat.params.w : alphaMap);
}

#endif
