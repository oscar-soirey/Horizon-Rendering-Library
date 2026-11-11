// Contract of the custom material shaders (HRL_CreateShader on Vulkan).
//
// The application writes, in the fragment source, a surface function
//     void Surface(HRL_SurfaceInput i, inout HRL_Surface s)
// that receives the surface of the material as HRL computed it (textures and
// parameters of the built-in material) and changes what it wants. In the
// vertex source it may write
//     void Vertex(inout HRL_VertexData v)
// to move the vertices (world space). Lighting, shadows, fog, picking and
// ray tracing are done by HRL. `uniform` declarations of float, int, uint,
// bool, vec2/3/4, ivec2/3/4 and sampler2D are material parameters, set with
// HRL_MaterialSetFloat / Int / Vec2 / Vec3 / Vec4 / Texture.
#ifndef HRL_CUSTOM_GLSL
#define HRL_CUSTOM_GLSL

struct HRL_VertexData
{
	vec3 worldPosition;
	vec3 normal;        // world space
	vec2 uv;
	float time;         // seconds
	uint objectId;      // mesh id
};

struct HRL_SurfaceInput
{
	vec3 worldPosition;
	vec3 normal;        // geometric normal, world space, facing the camera
	vec2 uv;
	vec3 viewDirection; // from the surface towards the camera
	vec2 screenUV;      // 0..1 over the viewport
	float time;         // seconds
	uint objectId;      // mesh id
};

struct HRL_Surface
{
	vec3 baseColor;     // linear
	vec3 normal;        // world space, normal map applied
	float roughness;
	float metallic;
	float specular;
	float ao;
	vec3 emissive;      // linear radiance
	float opacity;      // translucent materials blend with it, alpha tested ones cut at 0.5
};

// Helpers.
vec3 HRL_SRGBToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
vec3 HRL_LinearToSRGB(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

// Material parameters (filled by HRL, see vk_custom.cpp).
layout(set = 0, binding = 13, std430) readonly buffer HRL_CustomParameters { vec4 hrlParams[]; };
uint hrlParamBase = 0u;

#endif
