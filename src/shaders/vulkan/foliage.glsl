// HRL Vulkan renderer - foliage: data layout and plant motion.
#ifndef HRL_FOLIAGE_GLSL
#define HRL_FOLIAGE_GLSL

// Visible instance lists: one region per level (mesh LOD 0, 1, 2, impostor).
#define FOLIAGE_REGIONS 4
#define FOLIAGE_IMPOSTOR 3
#define FOLIAGE_MAX_PARTS 4

struct FoliageInstance
{
	vec4 positionScale; // xyz : root position, w : uniform scale
	vec4 rotation;      // quaternion
};

struct FoliageCell
{
	vec4 lo, hi;        // bounds of the instance spheres
	uvec4 range;        // x : first instance, y : count
};

// Per foliage kind (see FoliageUniform in vk_renderer_internal.h).
#define FOLIAGE_UNIFORM_BLOCK \
	vec4 bounds;      /* local bounds centre xyz, radius */ \
	vec4 heights;     /* x : local min y, y : height, z : mesh LOD count, w : impostor frames per side */ \
	vec4 lodEnd;      /* end distance of LOD 0, 1, 2; w : impostor start, 0 : none */ \
	vec4 draw;        /* x : draw distance, y : fade start, z : cross-fade band (share), w : wind response */ \
	uvec4 ids;        /* x : object id, y : albedo atlas slot, z : normal atlas slot, w : region capacity */ \
	uvec4 materials;  /* material index of each part */ \
	uvec4 lods[12];   /* part * 3 + lod : index count, first index, vertex offset, valid */ \
	vec4 impostor;    /* xyz : impostor centre (local), w : half size */

vec3 QuatRotate(vec4 q, vec3 v)
{
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

// Packed visible entry: instance index (24 bits), dither (8 bits).
uint PackVisible(uint instance, float coverage, bool complementary)
{
	uint c = uint(clamp(coverage, 0.0, 1.0) * 127.0 + 0.5);
	return instance | ((c | (complementary ? 128u : 0u)) << 24);
}

#ifdef HRL_COMMON_GLSL
// Offset of a vertex of a plant: sway with the wind (stronger towards the
// top), flutter of the leaves, bending away from the interactors.
// h01 : height of the vertex over the plant (0 root, 1 top), size : plant height.
vec3 FoliageMotion(vec3 root, vec3 world, float h01, float size, float response)
{
	vec2 dir = uView.wind.xy;
	float strength = uView.wind.z * response;
	float t = uView.camPos.w;
	vec3 offset = vec3(0.0);
	if (strength > 0.0)
	{
		float phase = dot(root.xz, vec2(0.071, 0.113));
		// Gusts travel along the wind direction.
		float gust = 0.5 + 0.5 * sin(t * 0.6 - dot(root.xz, dir) * 0.045 + sin(t * 0.21) * 2.0);
		gust = mix(1.0, 0.35 + 1.3 * gust * gust, uView.wind.w);
		float sway = (0.55 + 0.45 * sin(t * 1.7 + phase)) * gust;
		float bend = strength * sway * h01 * h01 * size * 0.12;
		offset.xz += dir * bend;
		// Small fast motion of the tips (leaves, blade ends).
		float flutter = sin(t * 6.3 + dot(world, vec3(2.1, 1.3, 1.7))) * 0.025 * strength * h01 * size;
		offset += vec3(dir.y, 0.35, -dir.x) * flutter;
		// The top moves on an arc: it goes down as it bends.
		offset.y -= bend * bend / max(size, 1e-3) * 0.6;
	}
	for (uint i = 0u; i < uView.counts.w; ++i)
	{
		vec4 s = uView.interactors[i];
		vec2 away = world.xz - s.xz;
		float d = length(away);
		float vertical = clamp(1.0 - abs(world.y - s.y) / (s.w + size), 0.0, 1.0);
		float push = clamp(1.0 - d / s.w, 0.0, 1.0) * vertical * h01;
		if (push <= 0.0) continue;
		away = d > 1e-4 ? away / d : vec2(1.0, 0.0);
		offset.xz += away * push * s.w * 0.7;
		offset.y -= push * size * 0.45;
	}
	return offset;
}
#endif

#endif
