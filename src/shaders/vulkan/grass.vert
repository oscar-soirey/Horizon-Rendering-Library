#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// One grass blade: position, height and orientation come from a hash of the
// tile and the blade index, the ground from the landscape heights.
#include "common.glsl"
#include "grass.glsl"
#include "foliage.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(set = 2, binding = 0, std140) uniform GrassData { GRASS_UNIFORM_BLOCK } uGrass;
layout(set = 2, binding = 1, std430) readonly buffer Tiles { GrassTile tiles[]; };
#ifdef HRL_SHADOW
layout(push_constant) uniform Push { mat4 lightViewProj; } pc;
#else
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out vec3 vRestPos;
#endif

float SampleHeight(vec2 s)
{
	return textureLod(uTextures[nonuniformEXT(uGrass.ids.x * 2u)], (s + 0.5) * uGrass.samples.zw, 0.0).r * uGrass.spacing.z;
}

// Blade levels (0 root .. 1 tip) and sides of the 15 vertices of a near blade
// (2 quads + tip) and the 3 vertices of a far blade.
const vec2 kNear[15] = vec2[](
	vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(0.4, -1.0), vec2(0.4, -1.0), vec2(0.0, 1.0), vec2(0.4, 1.0),
	vec2(0.4, -1.0), vec2(0.4, 1.0), vec2(0.75, -1.0), vec2(0.75, -1.0), vec2(0.4, 1.0), vec2(0.75, 1.0),
	vec2(0.75, -1.0), vec2(0.75, 1.0), vec2(1.0, 0.0));
const vec2 kFar[3] = vec2[](vec2(0.0, -1.0), vec2(0.0, 1.0), vec2(1.0, 0.0));

void Collapse()
{
	gl_Position = vec4(0.0, 0.0, -2.0, 1.0); // outside the depth range: nothing is drawn
#ifndef HRL_SHADOW
	vWorldPos = vec3(0.0); vNormal = vec3(0.0, 1.0, 0.0); vColor = vec3(0.0); vRestPos = vec3(0.0);
#endif
}

void main()
{
	uint slot = uint(gl_InstanceIndex) >> 16;
	uint blade = uint(gl_InstanceIndex) & 0xFFFFu;
	GrassTile tile = tiles[slot];
	float size = uGrass.grid.z;
	// Same blades in a tile whatever the view: seeded by the tile position.
	ivec2 cell = ivec2(floor(tile.data.xy / size + 0.5));
	uint state = GrassHash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ blade * 83492791u);
	vec2 world2 = tile.data.xy + vec2(GrassRandom(state), GrassRandom(state)) * size;
	float heightRand = GrassRandom(state), yaw = GrassRandom(state) * 6.2831853, lean = GrassRandom(state);
	float maskRand = GrassRandom(state), shade = GrassRandom(state);

	// Ground under the blade.
	vec3 local = (uGrass.invModel * vec4(world2.x, 0.0, world2.y, 1.0)).xyz;
	vec2 sizeLocal = uGrass.spacing.xy * (uGrass.samples.xy - 1.0);
	vec2 s = (local.xz / sizeLocal + 0.5) * (uGrass.samples.xy - 1.0);
	if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, uGrass.samples.xy - 1.0))) { Collapse(); return; }
	float h = SampleHeight(s);
	float hl = SampleHeight(s - vec2(1.0, 0.0)), hr = SampleHeight(s + vec2(1.0, 0.0));
	float hd = SampleHeight(s - vec2(0.0, 1.0)), hu = SampleHeight(s + vec2(0.0, 1.0));
	vec3 groundN = normalize(mat3(uGrass.model) * vec3(-(hr - hl) / (2.0 * uGrass.spacing.x), 1.0, -(hu - hd) / (2.0 * uGrass.spacing.y)));
	if (groundN.y < uGrass.baseColor.w) { Collapse(); return; }
	if (uGrass.ids.y != 0xFFFFFFFFu)
	{
		vec2 uv = s / (uGrass.samples.xy - 1.0);
		if (textureLod(uTextures[nonuniformEXT(uGrass.ids.y * 2u)], uv, 0.0).r <= maskRand) { Collapse(); return; }
	}
	vec3 root = (uGrass.model * vec4(local.x, h, local.z, 1.0)).xyz;

	// Blade shape: random height, facing and forward lean (stronger at the tip).
	float bladeHeight = uGrass.blade.x * (1.0 - uGrass.blade.z * heightRand);
	float keep = tile.data.z;
	float width = uGrass.blade.y * inversesqrt(keep); // fewer blades far away: wider ones
	vec3 facing = vec3(cos(yaw), 0.0, sin(yaw));
	vec3 across = vec3(-facing.z, 0.0, facing.x);
	bool near = tile.data.w > 0.5;
	vec2 v = near ? kNear[gl_VertexIndex] : kFar[gl_VertexIndex];
	float t = v.x;
	float curve = (0.15 + 0.35 * lean) * t * t;
	vec3 rest = root + vec3(0.0, t * bladeHeight, 0.0) + facing * curve * bladeHeight + across * (v.y * width * 0.5 * (1.0 - t * 0.7));
	vec3 world = rest + FoliageMotion(root, rest, t, bladeHeight, 1.6);
#ifdef HRL_SHADOW
	gl_Position = pc.lightViewProj * vec4(world, 1.0);
#else
	vWorldPos = world;
	vRestPos = rest;
	// Leaning, rounded normal: blades look soft, and lit like the ground near the root.
	vec3 bladeN = normalize(-facing + vec3(0.0, curve * 2.0, 0.0) + across * v.y * 0.4);
	vNormal = normalize(mix(groundN, bladeN, 0.35 + 0.4 * t));
	vColor = mix(uGrass.baseColor.rgb, uGrass.tipColor.rgb, t) * (0.8 + 0.4 * shade);
	gl_Position = uView.viewProj * vec4(world, 1.0);
#endif
}
