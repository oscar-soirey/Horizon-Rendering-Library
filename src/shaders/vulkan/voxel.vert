#version 460
#extension GL_GOOGLE_include_directive : require
// 2D voxel worlds (vk_voxel.cpp): chunk geometry built by core/voxel_render.cpp.
#include "common.glsl"
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inLayer;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec4 vColor;
layout(location = 2) flat out int vLayer;
void main()
{
	vWorldPos = vec3(inPosition, 0.0);
	vColor = inColor;
	vLayer = int(floor(inLayer + 0.5));
	gl_Position = uView.viewProj * vec4(vWorldPos, 1.0);
}
