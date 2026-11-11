#version 460
// Depth and normals of a viewport in the conventions of the OpenGL effect
// shaders (vk_effects.cpp): rows bottom first, depth 0..1 of the OpenGL
// projection, world space normals (rebuilt from the depth).
layout(set = 0, binding = 0) uniform sampler2D uDepth; // reversed-Z, rows top first
layout(set = 0, binding = 1, std140) uniform View
{
	mat4 invViewProj;  // Vulkan clip space of the frame -> world
	mat4 viewProjGL;   // world -> OpenGL clip space
	vec4 rect;         // viewport in the Vulkan image: x, y, width, height (pixels)
} uView;
layout(location = 0) out float outDepth;
layout(location = 1) out vec4 outNormal;

vec3 World(ivec2 p, float depth)
{
	vec2 ndc = vec2((float(p.x) + 0.5 - uView.rect.x) / uView.rect.z * 2.0 - 1.0,
		1.0 - (float(p.y) + 0.5 - uView.rect.y) / uView.rect.w * 2.0);
	vec4 w = uView.invViewProj * vec4(ndc, depth, 1.0);
	return w.xyz / w.w;
}

void main()
{
	ivec2 size = textureSize(uDepth, 0);
	// This image is flipped: its row r is row (height - 1 - r) of the Vulkan image.
	ivec2 p = ivec2(int(gl_FragCoord.x), size.y - 1 - int(gl_FragCoord.y));
	float depth = texelFetch(uDepth, p, 0).r;
	if (depth <= 0.0) { outDepth = 1.0; outNormal = vec4(0.0); return; } // sky
	vec3 center = World(p, depth);
	vec4 clip = uView.viewProjGL * vec4(center, 1.0);
	outDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);

	// Normal: the neighbours on the same surface (the closest depth on each axis).
	ivec2 lo = ivec2(uView.rect.xy), hi = ivec2(uView.rect.xy + uView.rect.zw) - 1;
	ivec2 px = clamp(p + ivec2(1, 0), lo, hi), nx = clamp(p - ivec2(1, 0), lo, hi);
	ivec2 py = clamp(p + ivec2(0, 1), lo, hi), ny = clamp(p - ivec2(0, 1), lo, hi);
	float dpx = texelFetch(uDepth, px, 0).r, dnx = texelFetch(uDepth, nx, 0).r;
	float dpy = texelFetch(uDepth, py, 0).r, dny = texelFetch(uDepth, ny, 0).r;
	vec3 ddx = abs(dpx - depth) < abs(dnx - depth) ? World(px, dpx) - center : center - World(nx, dnx);
	vec3 ddy = abs(dpy - depth) < abs(dny - depth) ? World(py, dpy) - center : center - World(ny, dny);
	vec3 n = cross(ddx, ddy);
	if (dot(n, n) < 1e-20) { outNormal = vec4(0.0); return; }
	n = normalize(n);
	// Facing the camera: towards the near point of the same pixel (reversed-Z: depth 1).
	if (dot(n, World(p, 1.0) - center) < 0.0) n = -n;
	outNormal = vec4(n, 1.0);
}
