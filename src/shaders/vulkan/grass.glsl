// HRL Vulkan renderer - procedural grass on a landscape (vk_grass.cpp).
#ifndef HRL_GRASS_GLSL
#define HRL_GRASS_GLSL

#define GRASS_UNIFORM_BLOCK \
	mat4 model;        /* landscape */ \
	mat4 invModel; \
	vec4 samples;      /* landscape : xy sample count, zw 1 / count */ \
	vec4 spacing;      /* landscape : xy local distance between samples, z height scale */ \
	vec4 blade;        /* x height, y width, z height variation, w blades per square unit */ \
	vec4 baseColor;    /* rgb, w : cos(max slope) */ \
	vec4 tipColor;     /* rgb, w : draw distance */ \
	vec4 grid;         /* xy : world origin of tile (0, 0), z : tile size, w : tiles per side */ \
	uvec4 ids;         /* x : height texture slot, y : mask texture slot (~0 none), z : object id, w : max tiles */

struct GrassTile
{
	vec4 data;         // xy : world origin, z : share of the blades drawn, w : 1 when drawn with full blades
};

uint GrassHash(uint x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float GrassRandom(inout uint state)
{
	state = GrassHash(state);
	return float(state >> 8) * (1.0 / 16777216.0);
}

#endif
