// HRL Vulkan renderer - visibility tests shared by the culling compute shaders.
#ifndef HRL_CULL_COMMON_GLSL
#define HRL_CULL_COMMON_GLSL

// Per view parameters of a culling dispatch (see CullView in vk_renderer_internal.h).
#define CULL_FLAG_OCCLUSION   1u  // an HZB of this view is available
#define CULL_FLAG_PERSPECTIVE 2u
#define CULL_FLAG_SHADOW      4u  // shadow view: no LOD selection, no occlusion

struct CullViewData
{
	mat4 viewProj;      // Vulkan clip space (reversed-Z for cameras)
	vec4 planes[6];     // world space frustum planes (xyz . p + w >= 0 inside)
	vec4 camPos;        // xyz : camera used for the LOD, w : projection scale (proj[1][1])
	vec4 hzb;           // xy : size of HZB mip 0, z : mip count
	uvec4 params;       // x : item count, y : phase (0 single, 1, 2), z : viewport bit, w : CULL_FLAG_*
	uvec4 bucketBase;   // first command of each bucket
	vec4 lodInfo;       // xyz : camera forward (view-space depth of the screen-size LOD)
};

bool SphereInFrustum(vec4 planes[6], vec3 center, float radius)
{
	for (int i = 0; i < 6; ++i)
		if (dot(planes[i].xyz, center) + planes[i].w < -radius) return false;
	return true;
}

// Occlusion test of a world space box against the hierarchical depth buffer
// (reversed-Z: mip texels hold the farthest depth of their area). Returns true
// when the box is certainly hidden.
bool BoxOccluded(sampler2D hzb, mat4 viewProj, vec4 hzbInfo, vec3 bmin, vec3 bmax)
{
	vec2 lo = vec2(1.0), hi = vec2(0.0);
	float nearest = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		vec3 p = vec3((i & 1) != 0 ? bmax.x : bmin.x, (i & 2) != 0 ? bmax.y : bmin.y, (i & 4) != 0 ? bmax.z : bmin.z);
		vec4 clip = viewProj * vec4(p, 1.0);
		// A corner behind the near plane: the box surrounds the camera.
		if (clip.w <= 1e-4) return false;
		vec3 ndc = clip.xyz / clip.w;
		vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
		lo = min(lo, uv);
		hi = max(hi, uv);
		nearest = max(nearest, ndc.z);
	}
	lo = clamp(lo, vec2(0.0), vec2(1.0));
	hi = clamp(hi, vec2(0.0), vec2(1.0));
	if (any(greaterThanEqual(lo, hi))) return false;
	vec2 size = (hi - lo) * hzbInfo.xy;
	float level = ceil(log2(max(max(size.x, size.y), 1.0)));
	level = clamp(level, 0.0, hzbInfo.z - 1.0);
	ivec2 mipSize = max(ivec2(hzbInfo.xy) >> int(level), ivec2(1));
	ivec2 a = clamp(ivec2(lo * vec2(mipSize)), ivec2(0), mipSize - 1);
	ivec2 b = clamp(ivec2(hi * vec2(mipSize)), ivec2(0), mipSize - 1);
	// The box covers at most 2 x 2 texels of this mip.
	float farthest = texelFetch(hzb, a, int(level)).r;
	farthest = min(farthest, texelFetch(hzb, ivec2(b.x, a.y), int(level)).r);
	farthest = min(farthest, texelFetch(hzb, ivec2(a.x, b.y), int(level)).r);
	farthest = min(farthest, texelFetch(hzb, b, int(level)).r);
	return nearest < farthest;
}

#endif
