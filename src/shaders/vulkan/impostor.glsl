// HRL Vulkan renderer - hemi-octahedral impostors: the views of a plant from
// N x N directions of the upper hemisphere, in one atlas (vk_impostor.cpp).
#ifndef HRL_IMPOSTOR_GLSL
#define HRL_IMPOSTOR_GLSL

// Direction (y >= 0) <-> [0, 1]^2.
vec2 HemiOctEncode(vec3 d)
{
	d.y = max(d.y, 0.0);
	vec3 a = abs(d);
	vec2 p = d.xz / max(a.x + a.y + a.z, 1e-6);
	return vec2(p.x + p.y, p.x - p.y) * 0.5 + 0.5;
}

vec3 HemiOctDecode(vec2 uv)
{
	vec2 p = uv * 2.0 - 1.0;
	vec2 xz = vec2(p.x + p.y, p.x - p.y) * 0.5;
	return normalize(vec3(xz.x, 1.0 - abs(xz.x) - abs(xz.y), xz.y));
}

// Basis of the view of direction d (from the plant towards the viewer).
void ImpostorBasis(vec3 d, out vec3 right, out vec3 up)
{
	right = d.y > 0.999 ? vec3(1.0, 0.0, 0.0) : normalize(cross(vec3(0.0, 1.0, 0.0), d));
	up = cross(d, right);
}

vec3 ImpostorFrameDirection(ivec2 frame, float frames)
{
	return HemiOctDecode((vec2(frame) + 0.5) / frames);
}

#endif
