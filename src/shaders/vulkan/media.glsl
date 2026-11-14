// HRL Vulkan renderer - participating media of a viewport (HRL_SetViewportMediaVolume).
//
// Two 3D textures written by the application's render passes before the
// surfaces, in froxels over the view:
//   uMediaVolume     : rgb light scattered towards the camera between the camera
//                      and the froxel, a transmittance over the same distance;
//   uMediaVisibility : r share of the first directional light reaching the froxel
//                      (shadows of the volumes).
// Froxel coordinates: x, y over the viewport (origin top-left, as the images),
// z = (view depth / max distance) ^ (1 / exponent). Needs common.glsl.
#ifndef HRL_MEDIA_GLSL
#define HRL_MEDIA_GLSL

layout(set = 0, binding = 16) uniform sampler3D uMediaVolume;
layout(set = 0, binding = 17) uniform sampler3D uMediaVisibility;

bool HRL_MediaActive() { return uView.mediaParams.z > 0.5; }

vec3 HRL_CameraForward() { return -vec3(uView.view[0][2], uView.view[1][2], uView.view[2][2]); }

// Froxel coordinates of a world position.
vec3 HRL_FroxelUVW(vec3 world)
{
	vec4 clip = uView.viewProj * vec4(world, 1.0);
	vec2 ndc = clip.xy / max(abs(clip.w), 1e-6) * sign(clip.w);
	float viewDepth = -(uView.view * vec4(world, 1.0)).z;
	float w = pow(clamp(viewDepth / uView.mediaParams.x, 0.0, 1.0), 1.0 / uView.mediaParams.y);
	return vec3(vec2(ndc.x, -ndc.y) * 0.5 + 0.5, w);
}

// World position of froxel coordinates (inverse of HRL_FroxelUVW).
vec3 HRL_FroxelWorld(vec3 uvw)
{
	vec2 ndc = vec2(uvw.x * 2.0 - 1.0, 1.0 - uvw.y * 2.0);
	vec4 p = uView.invViewProj * vec4(ndc, 0.5, 1.0);
	vec3 dir = normalize(p.xyz / p.w - uView.camPos.xyz);
	float viewDepth = pow(max(uvw.z, 0.0), uView.mediaParams.y) * uView.mediaParams.x;
	return uView.camPos.xyz + dir * (viewDepth / max(dot(dir, HRL_CameraForward()), 1e-4));
}

// Light scattered in front of a world position (rgb) and transmittance (a).
vec4 HRL_SampleMedia(vec3 world)
{
	if (!HRL_MediaActive()) return vec4(0.0, 0.0, 0.0, 1.0);
	return textureLod(uMediaVolume, HRL_FroxelUVW(world), 0.0);
}

vec3 HRL_ApplyMedia(vec3 color, vec3 world)
{
	vec4 m = HRL_SampleMedia(world);
	return color * m.a + m.rgb;
}

// Share of the first directional light reaching a world position through the volumes.
float HRL_MediaVisibility(vec3 world)
{
	if (uView.mediaParams.w < 0.5) return 1.0;
	return clamp(textureLod(uMediaVisibility, HRL_FroxelUVW(world), 0.0).r, 0.0, 1.0);
}

#endif
