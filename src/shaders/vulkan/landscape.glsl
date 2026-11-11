// HRL Vulkan renderer - landscape (CDLOD) data and vertex placement.
// Needs common.glsl and the bindless texture array `uTextures`.
#ifndef HRL_LANDSCAPE_GLSL
#define HRL_LANDSCAPE_GLSL

#define HRL_LANDSCAPE_MAX_LODS 16

layout(set = 2, binding = 0, std140) uniform LandscapeData
{
	mat4 model;
	mat4 normalMatrix;
	vec4 samples;      // xy : sample count (W, H), zw : 1 / (W, H)
	vec4 spacing;      // xy : local distance between samples, z : height scale, w : patch grid size (quads)
	vec4 uvScale;      // xy : material UV scale
	vec4 morphCamera;  // xyz : camera position used for the LOD (same for every view)
	uvec4 ids;         // x : material, y : object id, z : height texture slot
	vec4 morph[HRL_LANDSCAPE_MAX_LODS]; // x : morph start distance, y : 1 / (end - start)
} uLand;

float LandscapeHeight(vec2 s)
{
	// Linear filtering between samples: sample i sits at texel centre i + 0.5.
	return textureLod(uTextures[nonuniformEXT(uLand.ids.z * 2u)], (s + 0.5) * uLand.samples.zw, 0.0).r * uLand.spacing.z;
}

// Height on the triangles of the full resolution grid (the split used by the
// patches and the ray tracing mesh: a-c-b, b-c-d).
float LandscapeTriangleHeight(vec2 s)
{
	vec2 maxSample = uLand.samples.xy - 1.0;
	s = clamp(s, vec2(0.0), maxSample);
	ivec2 c = ivec2(min(floor(s), maxSample - 1.0));
	vec2 f = s - vec2(c);
	uint slot = uLand.ids.z * 2u;
	float a = texelFetch(uTextures[nonuniformEXT(slot)], c, 0).r;
	float b = texelFetch(uTextures[nonuniformEXT(slot)], c + ivec2(1, 0), 0).r;
	float cc = texelFetch(uTextures[nonuniformEXT(slot)], c + ivec2(0, 1), 0).r;
	float d = texelFetch(uTextures[nonuniformEXT(slot)], c + ivec2(1, 1), 0).r;
	float h = f.x + f.y <= 1.0 ? a + (b - a) * f.x + (cc - a) * f.y : d + (cc - d) * (1.0 - f.x) + (b - d) * (1.0 - f.y);
	return h * uLand.spacing.z;
}

vec3 LandscapeLocal(vec2 s, float h)
{
	return vec3((s.x / (uLand.samples.x - 1.0) - 0.5) * uLand.spacing.x * (uLand.samples.x - 1.0), h,
		(s.y / (uLand.samples.y - 1.0) - 0.5) * uLand.spacing.y * (uLand.samples.y - 1.0));
}

// Sample-space position of a patch vertex, morphed towards the next level near
// the end of its LOD range (no crack, no popping). node : origin xy, size, lod.
vec2 LandscapeVertex(vec2 grid, vec4 node, out vec3 world)
{
	float n = uLand.spacing.w;
	vec2 maxSample = uLand.samples.xy - 1.0;
	vec2 s = min(node.xy + grid * (node.z / n), maxSample);
	vec3 w0 = (uLand.model * vec4(LandscapeLocal(s, LandscapeHeight(s)), 1.0)).xyz;
	vec4 m = uLand.morph[int(node.w)];
	float k = clamp((distance(w0, uLand.morphCamera.xyz) - m.x) * m.y, 0.0, 1.0);
	vec2 g = grid - fract(grid * 0.5) * 2.0 * k;
	s = min(node.xy + g * (node.z / n), maxSample);
	world = (uLand.model * vec4(LandscapeLocal(s, LandscapeHeight(s)), 1.0)).xyz;
	return s;
}

#endif
