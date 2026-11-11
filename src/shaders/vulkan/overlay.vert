#version 460
#extension GL_GOOGLE_include_directive : require
#include "overlay.glsl"
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vQuad;
const vec2 kCorners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(1, 1), vec2(0, 1), vec2(0, 0));
void main()
{
	vQuad = uint(gl_InstanceIndex); // firstInstance of the draw: first quad of the viewport
	OverlayQuad q = quads[vQuad];
	vec2 corner = kCorners[gl_VertexIndex];
	vUV = mix(q.uv.xy, q.uv.zw, corner);
	// Vulkan clip space: y = -1 is the top of the viewport.
	gl_Position = vec4((q.rect.xy + corner * q.rect.zw) * 2.0 - 1.0, 0.0, 1.0);
}
