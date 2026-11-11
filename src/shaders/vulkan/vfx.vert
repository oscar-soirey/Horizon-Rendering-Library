#version 460
#extension GL_GOOGLE_include_directive : require
// Particles drawn as camera facing quads (vk_vfx.cpp), as on OpenGL.
#include "common.glsl"
struct Particle { mat4 model; vec4 color; };
layout(set = 2, binding = 0, std430) readonly buffer Particles { Particle particles[]; };
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
const vec2 kCorners[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(1, 1), vec2(0, 1), vec2(0, 0));
void main()
{
	Particle p = particles[gl_InstanceIndex];
	vec2 c = kCorners[gl_VertexIndex];
	vUV = c;
	vColor = p.color;
	gl_Position = uView.viewProj * (p.model * vec4(c - 0.5, 0.0, 1.0));
}
