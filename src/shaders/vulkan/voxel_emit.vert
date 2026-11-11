#version 460
// Voxel light field (vk_voxel.cpp): the emissive voxels drawn into the region.
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec4 inColor;
layout(push_constant) uniform Push { vec4 region; } pc; // xy : world min, zw : 1 / world size
layout(location = 0) out vec3 vEmission;
void main()
{
	vec2 uv = (inPosition - pc.region.xy) * pc.region.zw;
	vEmission = max(inColor.rgb, vec3(0.0)) * clamp(inColor.a, 0.0, 1.0);
	gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
