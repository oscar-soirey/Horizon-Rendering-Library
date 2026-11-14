// HRL Vulkan renderer - contract of the render pass shaders (HRL_CreateComputeShader,
// HRL_CreateFullscreenShader). Included after common.glsl, brdf.glsl, the
// bindless textures, material_alpha.glsl and lighting.glsl (so a pass sees the
// view, the lights, their shadows and the participating media).
//
// Set 2 holds the resources of the pass:
//   binding 0 : frame data (hrlPass) and the material parameters of the pass
//   binding 1 : hrl_Depth       depth of the scene image (reversed-Z, 0 = far)
//   binding 2 : hrl_SceneColor  copy of the HDR scene image made before the pass
//   binding 3 : hrl_SceneImage  the HDR scene image itself (compute, read / write)
//   binding 4.. the HRL_IMAGE* / HRL_TEXTURE* / HRL_BUFFER declarations, in order.
// Coordinates: the images of the scene hold every viewport, a pass works on the
// rectangle hrlPass.viewportRect of its viewport; "viewport UV" is 0..1 over
// that rectangle, origin top-left (the convention of every HRL image).
#ifndef HRL_PASS_GLSL
#define HRL_PASS_GLSL

struct HRL_PassData
{
	vec4 viewportRect;      // x, y, width, height (pixels of the scene image)
	vec4 sceneSize;         // xy : size of the scene image, zw : 1 / size
	vec4 dispatchSize;      // xyz : invocations of the dispatch / pixels of the outputs
	vec4 frame;             // x : frame number, y : time (s), z : time since the last frame (s), w : 1 when the history is valid
	vec4 jitter;            // xy : sub-pixel offset of this frame (NDC), zw : of the previous frame
	vec4 previousCamera;    // xyz : camera position of the previous frame
	vec4 previousForward;   // xyz : camera forward of the previous frame
	mat4 viewProjNoJitter;  // this frame, without the anti-aliasing offset
	mat4 previousViewProj;  // previous frame, without the anti-aliasing offset (reprojection)
};
layout(set = 2, binding = 0, std430) readonly buffer HRL_PassParameters
{
	HRL_PassData hrlPass;
	vec4 hrlParams[];
};
layout(set = 2, binding = 1) uniform sampler2D hrl_Depth;
layout(set = 2, binding = 2) uniform sampler2D hrl_SceneColor;
#ifdef HRL_COMPUTE_STAGE
layout(set = 2, binding = 3, rgba16f) uniform image2D hrl_SceneImage;
#endif

const float HRL_PI = 3.14159265358979;

// ---- Coordinates -------------------------------------------------------------
ivec2 HRL_ViewportSize() { return ivec2(hrlPass.viewportRect.zw); }
// Pixel of the scene images (hrl_Depth, hrl_SceneImage) of a viewport pixel.
ivec2 HRL_ScenePixel(ivec2 viewportPixel) { return ivec2(hrlPass.viewportRect.xy) + viewportPixel; }
vec2 HRL_SceneUV(vec2 viewportUV) { return (hrlPass.viewportRect.xy + viewportUV * hrlPass.viewportRect.zw) * hrlPass.sceneSize.zw; }
bool HRL_IsSky(float depth) { return depth <= 0.0; }

// Raw depth at a viewport UV (reversed-Z: 0 is the far plane / the sky).
float HRL_SceneDepth(vec2 viewportUV)
{
	vec2 texel = clamp(hrlPass.viewportRect.xy + viewportUV * hrlPass.viewportRect.zw,
		hrlPass.viewportRect.xy + 0.5, hrlPass.viewportRect.xy + hrlPass.viewportRect.zw - 0.5);
	return texelFetch(hrl_Depth, ivec2(texel), 0).r;
}

// World position of a viewport UV at a raw depth (the depth of this frame).
vec3 HRL_WorldPosition(vec2 viewportUV, float depth)
{
	vec4 p = uView.invViewProj * vec4(viewportUV.x * 2.0 - 1.0, 1.0 - viewportUV.y * 2.0, depth, 1.0);
	return p.xyz / p.w;
}

// Direction from the camera through a viewport UV.
vec3 HRL_ViewRay(vec2 viewportUV)
{
	return normalize(HRL_WorldPosition(viewportUV, 0.5) - uView.camPos.xyz);
}

float HRL_ViewDepth(vec3 world) { return -(uView.view * vec4(world, 1.0)).z; }

// Viewport UV of a world position in the previous frame; false when it was off screen or behind.
bool HRL_PreviousUV(vec3 world, out vec2 uv)
{
	vec4 clip = hrlPass.previousViewProj * vec4(world, 1.0);
	if (clip.w <= 1e-5) { uv = vec2(-1.0); return false; }
	vec2 ndc = clip.xy / clip.w;
	uv = vec2(ndc.x, -ndc.y) * 0.5 + 0.5;
	return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

// Froxel coordinates of a world position in the previous frame (media mapping of the viewport).
bool HRL_PreviousFroxelUVW(vec3 world, out vec3 uvw)
{
	vec2 uv;
	bool inside = HRL_PreviousUV(world, uv);
	float viewDepth = dot(world - hrlPass.previousCamera.xyz, hrlPass.previousForward.xyz);
	float w = pow(clamp(viewDepth / uView.mediaParams.x, 0.0, 1.0), 1.0 / uView.mediaParams.y);
	uvw = vec3(uv, w);
	return inside && viewDepth > 0.0 && viewDepth < uView.mediaParams.x;
}

// ---- Lights ------------------------------------------------------------------
uint HRL_LightCount() { return uView.counts.x; }

// Light of lights[i] arriving at a point of a volume (no surface: no cosine),
// with its shadow (shadow maps or ray traced). L : direction towards the light.
// Sky lights (ambient) give 0 here: see HRL_AmbientLight.
vec3 HRL_LightRadiance(uint i, vec3 p, out vec3 L)
{
	Light light = lights[i];
	int type = int(light.position.w);
	L = vec3(0.0, 1.0, 0.0);
	if (type == LIGHT_SKY) return vec3(0.0);
	float attenuation = 1.0;
	float dist = 1e4;
	if (type == LIGHT_DIRECTIONAL)
		L = normalize(-light.direction.xyz);
	else
	{
		vec3 toLight = light.position.xyz - p;
		dist = length(toLight);
		L = toLight / max(dist, 1e-4);
		attenuation = 1.0 / (1.0 + light.direction.w * dist * dist);
		if (type == LIGHT_SPOT)
			attenuation *= smoothstep(light.spot.y, light.spot.x, dot(L, normalize(-light.direction.xyz)));
	}
	if (attenuation <= 1e-5) return vec3(0.0);
	float shadow = LightShadow(light, p, L, L, dist);
	return light.color.rgb * attenuation * shadow;
}

// Index of the first directional light (the one the media visibility applies to), -1 if none.
int HRL_MainLight()
{
	for (uint i = 0u; i < uView.counts.x; ++i)
		if (int(lights[i].position.w) == LIGHT_DIRECTIONAL) return int(i);
	return -1;
}

// Light coming from every direction: sky lights and the environment (irradiance around `dir`).
vec3 HRL_AmbientLight(vec3 dir)
{
	vec3 ambient = vec3(0.0);
	for (uint i = 0u; i < uView.counts.x; ++i)
		if (int(lights[i].position.w) == LIGHT_SKY) ambient += lights[i].color.rgb;
	if ((uView.counts.y & VIEW_FLAG_IBL) != 0u) ambient += texture(uIrradiance, dir).rgb * uView.envParams.y;
	return ambient;
}

// ---- Volumes -------------------------------------------------------------------
// Henyey-Greenstein phase function (g > 0 : forward scattering).
float HRL_PhaseHG(float cosTheta, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * HRL_PI * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// ---- Noise ---------------------------------------------------------------------
float HRL_Hash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}
float HRL_Hash13(vec3 p3)
{
	p3 = fract(p3 * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return fract((p3.x + p3.y) * p3.z);
}
// Interleaved gradient noise, moved every frame (golden ratio): per pixel jitter of ray marches.
float HRL_FrameNoise(vec2 pixel)
{
	float ign = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	return fract(ign + hrlPass.frame.x * 0.61803398875);
}

#endif
