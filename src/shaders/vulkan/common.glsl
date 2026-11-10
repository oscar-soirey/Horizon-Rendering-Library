// HRL Vulkan renderer - data shared by every scene shader.
// Must match the structures of src/backend/vulkan/vk_renderer.cpp.
#ifndef HRL_COMMON_GLSL
#define HRL_COMMON_GLSL

#define HRL_MAX_CASCADES 4
#define HRL_MAX_SPOT_SHADOWS 4

#define LIGHT_POINT 0
#define LIGHT_DIRECTIONAL 1
#define LIGHT_SPOT 2
#define LIGHT_SKY 3

#define VIEW_FLAG_IBL 1u
#define VIEW_FLAG_RT_SHADOWS 2u
#define VIEW_FLAG_FOG 4u
#define VIEW_FLAG_FOG_EXP 8u

#define MAT_ROUGHNESS_VALUE 1u
#define MAT_METALLIC_VALUE 2u
#define MAT_SPECULAR_VALUE 4u
#define MAT_OPACITY_VALUE 8u
#define MAT_ROUGHNESS_INVERT 16u
#define MAT_ALPHA_INVERT 32u

layout(set = 0, binding = 0, std140) uniform ViewData
{
	mat4 viewProj;
	mat4 view;
	mat4 invViewProj;
	vec4 camPos;          // w : time (s)
	vec4 viewportSize;    // xy : pixels, zw : 1 / pixels
	mat4 cascadeViewProj[HRL_MAX_CASCADES];
	vec4 cascadeSplits;   // view-space distance where each cascade ends
	mat4 spotViewProj[HRL_MAX_SPOT_SHADOWS];
	vec4 shadowInfo;      // x : shadow map texel size, y : cascade blend range
	vec4 fogColor;
	vec4 fogParams;       // x : start, y : end, z : density
	vec4 envParams;       // x : specular mip count - 1, y : intensity
	uvec4 counts;         // x : light count, y : VIEW_FLAG_*, z : debug view
} uView;

struct Instance
{
	mat4 model;
	mat4 normalMatrix;
	uvec4 ids;            // x : material index, y : object id
};
layout(set = 0, binding = 1, std430) readonly buffer Instances { Instance instances[]; };

struct Material
{
	vec4 baseColor;       // rgb : tint, a : base alpha
	vec4 emissive;        // rgb : linear radiance
	vec4 params;          // roughness, metallic, specular, opacity
	uvec4 tex0;           // albedo, normal, roughness, metallic
	uvec4 tex1;           // ao, alpha, specular, emissive
	uvec4 flags;          // x : MAT_*
};
layout(set = 0, binding = 2, std430) readonly buffer Materials { Material materials[]; };

struct Light
{
	vec4 position;        // xyz, w : type
	vec4 direction;       // xyz : direction the light travels, w : attenuation
	vec4 color;           // rgb : color * intensity, a : shadow slot (-1 none, 0 cascades, 1.. spot)
	vec4 spot;            // cos inner, cos outer, shadow strength, shadow bias
};
layout(set = 0, binding = 3, std430) readonly buffer Lights { Light lights[]; };

#endif
