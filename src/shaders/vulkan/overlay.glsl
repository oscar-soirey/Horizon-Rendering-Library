// 2D overlay quads (widgets, texts): see vk_overlay.cpp (OverlayQuadGPU).
#ifndef HRL_OVERLAY_GLSL
#define HRL_OVERLAY_GLSL
struct OverlayQuad
{
	vec4 rect;   // x, y, width, height: normalized to the viewport, origin top-left
	vec4 uv;     // u, v at the top-left corner, u, v at the bottom-right corner
	vec4 color;  // tint, display values (not linear)
	uvec4 info;  // x : bindless texture, y : 1 for an SDF text
};
layout(set = 0, binding = 0, std430) readonly buffer OverlayQuads { OverlayQuad quads[]; };
layout(push_constant) uniform OverlayPush
{
	uint encoded;   // 1 when the target stores display values as given (UNORM)
} pc;
#endif
